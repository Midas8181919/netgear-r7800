/*
 *  linux/fs/hfsplus/bitmap.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handling of allocation file
 */

#include <linux/pagemap.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

#define PAGE_CACHE_BITS	(PAGE_CACHE_SIZE * 8)

int hfsplus_block_allocate(hfsplus_handle_t *hfsplus_handle, struct super_block *sb, u32 size, u32 offset, u32 *max)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(sb);
	struct page *page;
	struct address_space *mapping;
	__be32 *pptr, *curr, *end;
	u32 mask, start, len, n;
	__be32 val;
	int i;

	len = *max;
	if (!len)
		return size;

	dprint(DBG_BITMAP, "block_allocate: %u,%u,%u\n", size, offset, len);
	mutex_lock(&sbi->alloc_mutex);
	mapping = sbi->alloc_file->i_mapping;
	page = read_mapping_page(mapping, offset / PAGE_CACHE_BITS, NULL);
	if (IS_ERR(page)) {
		start = size;
		goto out;
	}
	pptr = kmap(page);
	curr = pptr + (offset & (PAGE_CACHE_BITS - 1)) / 32;
	i = offset % 32;
	offset &= ~(PAGE_CACHE_BITS - 1);
	if ((size ^ offset) / PAGE_CACHE_BITS)
		end = pptr + PAGE_CACHE_BITS / 32;
	else
		end = pptr + ((size + 31) & (PAGE_CACHE_BITS - 1)) / 32;

	/* scan the first partial u32 for zero bits */
	val = *curr;
	if (~val) {
		n = be32_to_cpu(val);
		mask = (1U << 31) >> i;
		for (; i < 32; mask >>= 1, i++) {
			if (!(n & mask))
				goto found;
		}
	}
	curr++;

	/* scan complete u32s for the first zero bit */
	while (1) {
		while (curr < end) {
			val = *curr;
			if (~val) {
				n = be32_to_cpu(val);
				mask = 1 << 31;
				for (i = 0; i < 32; mask >>= 1, i++) {
					if (!(n & mask))
						goto found;
				}
			}
			curr++;
		}
		kunmap(page);
		offset += PAGE_CACHE_BITS;
		if (offset >= size)
			break;
		page = read_mapping_page(mapping, offset / PAGE_CACHE_BITS,
					 NULL);
		if (IS_ERR(page)) {
			start = size;
			goto out;
		}
		curr = pptr = kmap(page);
		if ((size ^ offset) / PAGE_CACHE_BITS)
			end = pptr + PAGE_CACHE_BITS / 32;
		else
			end = pptr + ((size + 31) & (PAGE_CACHE_BITS - 1)) / 32;
	}
	dprint(DBG_BITMAP, "bitmap full\n");
	start = size;
	goto out;

found:
	start = offset + (curr - pptr) * 32 + i;
	if (start >= size) {
		dprint(DBG_BITMAP, "bitmap full\n");
		goto out;
	}
	/* do any partial u32 at the start */
	len = min(size - start, len);
	while (1) {
		n |= mask;
		if (++i >= 32)
			break;
		mask >>= 1;
		if (!--len || n & mask)
			goto done;
	}
	if (!--len)
		goto done;
	*curr++ = cpu_to_be32(n);
	/* do full u32s */
	while (1) {
		while (curr < end) {
			n = be32_to_cpu(*curr);
			if (len < 32)
				goto last;
			if (n) {
				len = 32;
				goto last;
			}
			*curr++ = cpu_to_be32(0xffffffff);
			len -= 32;
		}
		hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
		kunmap(page);
		offset += PAGE_CACHE_BITS;
		page = read_mapping_page(mapping, offset / PAGE_CACHE_BITS,
					 NULL);
		if (IS_ERR(page)) {
			start = size;
			goto out;
		}
		pptr = kmap(page);
		curr = pptr;
		end = pptr + PAGE_CACHE_BITS / 32;
	}
last:
	/* do any partial u32 at end */
	mask = 1U << 31;
	for (i = 0; i < len; i++) {
		if (n & mask)
			break;
		n |= mask;
		mask >>= 1;
	}
done:
	*curr = cpu_to_be32(n);
	hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
	kunmap(page);
	*max = offset + (curr - pptr) * 32 + i - start;
	sbi->free_blocks -= *max;
	sb->s_dirt = 1;
	dprint(DBG_BITMAP, "-> %u,%u\n", start, *max);
out:
	mutex_unlock(&sbi->alloc_mutex);
	return start;
}

int hfsplus_block_free(hfsplus_handle_t *hfsplus_handle, struct super_block *sb, u32 offset, u32 count)
{
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(sb);
	struct page *page;
	struct address_space *mapping;
	__be32 *pptr, *curr, *end;
	u32 mask, len, pnr;
	int i;

	/* is there any actual work to be done? */
	if (!count)
		return 0;

	dprint(DBG_BITMAP, "block_free: %u,%u\n", offset, count);
	/* are all of the bits in range? */
	if ((offset + count) > sbi->total_blocks)
		return -2;

	mutex_lock(&sbi->alloc_mutex);
	mapping = sbi->alloc_file->i_mapping;
	pnr = offset / PAGE_CACHE_BITS;
	page = read_mapping_page(mapping, pnr, NULL);
	pptr = kmap(page);
	curr = pptr + (offset & (PAGE_CACHE_BITS - 1)) / 32;
	end = pptr + PAGE_CACHE_BITS / 32;
	len = count;

	/* do any partial u32 at the start */
	i = offset % 32;
	if (i) {
		int j = 32 - i;
		mask = 0xffffffffU << j;
		if (j > count) {
			mask |= 0xffffffffU >> (i + count);
			*curr++ &= cpu_to_be32(mask);
			goto out;
		}
		*curr++ &= cpu_to_be32(mask);
		count -= j;
	}

	/* do full u32s */
	while (1) {
		while (curr < end) {
			if (count < 32)
				goto done;
			*curr++ = 0;
			count -= 32;
		}
		if (!count)
			break;
		hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
		kunmap(page);
		page = read_mapping_page(mapping, ++pnr, NULL);
		pptr = kmap(page);
		curr = pptr;
		end = pptr + PAGE_CACHE_BITS / 32;
	}
done:
	/* do any partial u32 at end */
	if (count) {
		mask = 0xffffffffU >> count;
		*curr &= cpu_to_be32(mask);
	}
out:
	hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
	kunmap(page);
	sbi->free_blocks += len;
	sb->s_dirt = 1;
	mutex_unlock(&sbi->alloc_mutex);

	return 0;
}
