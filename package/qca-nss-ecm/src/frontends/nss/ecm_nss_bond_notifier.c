/*
 **************************************************************************
 * Copyright (c) 2014-2015 The Linux Foundation. All rights reserved.
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 **************************************************************************
 */

/*
 * ecm_nss_bond_notifier.c
 * 	Bonding notifier functionality.
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/icmp.h>
#include <linux/debugfs.h>
#include <linux/kthread.h>
#include <linux/pkt_sched.h>
#include <linux/string.h>
#include <net/route.h>
#include <net/ip.h>
#include <net/tcp.h>
#include <asm/unaligned.h>
#include <asm/uaccess.h>	/* for put_user */
#include <net/ipv6.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <linux/tcp.h>

#include <linux/inetdevice.h>
#include <linux/if_arp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_bridge.h>
#include <linux/if_bridge.h>
#include <linux/if_bonding.h>
#include <net/arp.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_l3proto.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/ipv4/nf_conntrack_ipv4.h>
#include <net/netfilter/ipv4/nf_defrag_ipv4.h>

/*
 * Debug output levels
 * 0 = OFF
 * 1 = ASSERTS / ERRORS
 * 2 = 1 + WARN
 * 3 = 2 + INFO
 * 4 = 3 + TRACE
 */
#define DEBUG_LEVEL ECM_BOND_NOTIFIER_DEBUG_LEVEL

#include <nss_api_if.h>

#include "ecm_types.h"
#include "ecm_db_types.h"
#include "ecm_state.h"
#include "ecm_tracker.h"
#include "ecm_classifier.h"
#include "ecm_front_end_types.h"
#include "ecm_tracker_udp.h"
#include "ecm_tracker_tcp.h"
#include "ecm_tracker_datagram.h"
#include "ecm_db.h"
#include "ecm_classifier_default.h"
#include "ecm_nss_ipv4.h"
#include "ecm_interface.h"
// GGG #include "ecm_front_end_ipv6.h"

#define ECM_BOND_ID_INVALID			0xFFFFFFFF

/*
 * Locking of the classifier - concurrency control
 */
static DEFINE_SPINLOCK(ecm_nss_bond_notifier_lock);			/* Protect against SMP access between netfilter, events and private threaded function. */

/*
 * Debugfs dentry object.
 */
static struct dentry *ecm_nss_bond_notifier_dentry;

/*
 * General operational control
 */
static int ecm_nss_bond_notifier_stopped = 0;			/* When non-zero further traffic will not be processed */

/*
 * ecm_nss_bond_notifier_bond_cb
 *	Bond driver notifier
 */
static struct bond_cb ecm_nss_bond_notifier_bond_cb;

/*
 * NSS Context for LAG function
 */
static void *ecm_nss_bond_notifier_nss_context = NULL;		/* Registration for LAG */

/*
 * ecm_nss_bond_notifier_send_lag_state()
 *	Send the currnet LAG state of a physical interface that has changed state in the bonding driver.
 */
static nss_tx_status_t ecm_nss_bond_notifier_send_lag_state(struct nss_ctx_instance *nss_ctx, struct net_device *slave, enum nss_lag_state_change_ev slave_state)
{
	int32_t lagid = 0;
	int32_t bondid = 0;
	int32_t slave_ifnum;
	nss_tx_status_t nss_tx_status;
	struct nss_lag_msg nm;
	struct nss_lag_state_change *nlsc = NULL;

	DEBUG_INFO("Send LAG update for: %p (%s)\n", slave, slave->name);

	/*
	 * Can only handle interfaces known to the nss
	 */
	slave_ifnum = nss_cmn_get_interface_number(nss_ctx, slave);
	if (slave_ifnum < 0) {
		DEBUG_WARN("Not an NSS interface: %p\n", slave);
		return NSS_TX_FAILURE_BAD_PARAM;
	}

	/*
	 * Figure out the aggregation id of this slave
	 */
	memset(&nm, 0, sizeof(nm));
	bondid = bond_get_id(slave->master);
	if (bondid == ECM_BOND_ID_INVALID) {
		DEBUG_WARN("Invalid LAG group id 0x%x\n", bondid);
		return NSS_TX_FAILURE;
	}

	lagid = bondid + NSS_LAG0_INTERFACE_NUM;

	/*
	 * Construct a message to the NSS to update it
	 */
	nss_lag_msg_init(&nm, lagid,
			 NSS_TX_METADATA_LAG_STATE_CHANGE,
			 sizeof(struct nss_lag_state_change),
			 NULL, NULL);

	nlsc = &nm.msg.state;

	nlsc->event = slave_state;

	nlsc->interface = slave_ifnum;

	nss_tx_status = nss_lag_tx(nss_ctx, &nm);
	if (nss_tx_status != NSS_TX_SUCCESS) {
		DEBUG_WARN("%p: Send LAG update failed, status: %d\n", slave, nss_tx_status);
		return NSS_TX_FAILURE;
	}
	DEBUG_TRACE("%p: Send LAG update sent\n", slave);
	return NSS_TX_SUCCESS;
}

/*
 * ecm_nss_bond_notifier_bond_release()
 *	Callback when a slave device is released from slavedom and no longer a part of a bonded interface.
 */
static void ecm_nss_bond_notifier_bond_release(struct net_device *slave_dev)
{
	/*
	 * If operations have stopped then do not process event
	 */
	DEBUG_INFO("Bond slave release: %p (%s)\n", slave_dev, slave_dev->name);
	spin_lock_bh(&ecm_nss_bond_notifier_lock);
	if (unlikely(ecm_nss_bond_notifier_stopped)) {
		DEBUG_WARN("Ignoring bond release event - stopped\n");
		spin_unlock_bh(&ecm_nss_bond_notifier_lock);
		return;
	}
	spin_unlock_bh(&ecm_nss_bond_notifier_lock);
	ecm_nss_bond_notifier_send_lag_state(ecm_nss_bond_notifier_nss_context, slave_dev, NSS_LAG_RELEASE);
}

/*
 * ecm_nss_bond_notifier_bond_enslave()
 *	Callback when a device is enslaved by a LAG master device
 */
static void ecm_nss_bond_notifier_bond_enslave(struct net_device *slave_dev)
{
	/*
	 * If operations have stopped then do not process event
	 */
	DEBUG_INFO("Bond slave enslave: %p (%s)\n", slave_dev, slave_dev->name);
	spin_lock_bh(&ecm_nss_bond_notifier_lock);
	if (unlikely(ecm_nss_bond_notifier_stopped)) {
		DEBUG_WARN("Ignoring bond enslave event - stopped\n");
		spin_unlock_bh(&ecm_nss_bond_notifier_lock);
		return;
	}
	spin_unlock_bh(&ecm_nss_bond_notifier_lock);
	ecm_nss_bond_notifier_send_lag_state(ecm_nss_bond_notifier_nss_context, slave_dev, NSS_LAG_ENSLAVE);
}

/*
 * ecm_nss_bond_notifier_bond_link_down()
 *	Callback when a link goes down on a LAG slave
 */
static void ecm_nss_bond_notifier_bond_link_down(struct net_device *slave_dev)
{
	struct net_device *master;

	/*
	 * If operations have stopped then do not process event
	 */
	spin_lock_bh(&ecm_nss_bond_notifier_lock);
	if (unlikely(ecm_nss_bond_notifier_stopped)) {
		DEBUG_WARN("Ignoring bond link down event - stopped\n");
		spin_unlock_bh(&ecm_nss_bond_notifier_lock);
		return;
	}
	spin_unlock_bh(&ecm_nss_bond_notifier_lock);

	/*
	 * A net device that is a LAG slave has lost link.
	 * Due to the heiarchical nature of network topologies, this can
	 * change the packet transmit path for any connection that is using
	 * a device that it sitting "higher" in the heirarchy. Regenerate all
	 * connections using the LAG master.
	 */

	master = ecm_interface_get_and_hold_dev_master(slave_dev);
	ecm_interface_dev_regenerate_connections(master);
	dev_put(master);
}

/*
 * ecm_bond_notifier_bond_link_up()
 *	Callback when a device is enslaved by a LAG master device
 */
static void ecm_nss_bond_notifier_bond_link_up(struct net_device *slave_dev)
{
	struct net_device *master;

	/*
	 * If operations have stopped then do not process event
	 */
	spin_lock_bh(&ecm_nss_bond_notifier_lock);
	if (unlikely(ecm_nss_bond_notifier_stopped)) {
		DEBUG_WARN("Ignoring bond enslave event - stopped\n");
		spin_unlock_bh(&ecm_nss_bond_notifier_lock);
		return;
	}
	spin_unlock_bh(&ecm_nss_bond_notifier_lock);

	/*
	 * Tricky to handle, this one.
	 * A net device that is a LAG slave has become active.
	 * Due to the heiarchical nature of network topologies, this can change the packet transmit path
	 * for any connection that is using a device that it sitting "higher" in the heirarchy.
	 * Regenerate all connections using the LAG master.
	 */

	master = ecm_interface_get_and_hold_dev_master(slave_dev);
	ecm_interface_dev_regenerate_connections(master);
	dev_put(master);
}

/*
 * ecm_nss_bond_notifier_lag_event_cb()
 *	Handle LAG event from the NSS driver
 */
static void ecm_nss_bond_notifier_lag_event_cb(void *if_ctx, struct nss_lag_msg *msg)
{
	/*
	 * If operations have stopped then do not process event
	 */
	spin_lock_bh(&ecm_nss_bond_notifier_lock);
	if (unlikely(ecm_nss_bond_notifier_stopped)) {
		DEBUG_WARN("Ignoring LAG event event - stopped\n");
		spin_unlock_bh(&ecm_nss_bond_notifier_lock);
		return;
	}
	spin_unlock_bh(&ecm_nss_bond_notifier_lock);

	/*
	 * GGG TODO Figure out if there is anything we need to do here, the old CM did nothing..
	 */
	switch (msg->cm.type)
	{
		default:
			DEBUG_INFO("Unknown LAG event from NSS: %d", msg->cm.type);
			break;
	}
}

void ecm_nss_bond_notifier_stop(int num)
{
	ecm_nss_bond_notifier_stopped = num;
}
EXPORT_SYMBOL(ecm_nss_bond_notifier_stop);

/*
 * ecm_nss_bond_notifier_init()
 */
int ecm_nss_bond_notifier_init(struct dentry *dentry)
{
	DEBUG_INFO("ECM Bonding Notifier init\n");

	ecm_nss_bond_notifier_dentry = debugfs_create_dir("ecm_nss_bond_notifier", dentry);
	if (!ecm_nss_bond_notifier_dentry) {
		DEBUG_ERROR("Failed to create ecm bond notifier directory in debugfs\n");
		return -1;
	}

	if (!debugfs_create_u32("stop", S_IRUGO | S_IWUSR, ecm_nss_bond_notifier_dentry,
					(u32 *)&ecm_nss_bond_notifier_stopped)) {
		DEBUG_ERROR("Failed to create ecm bond notifier stopped file in debugfs\n");
		debugfs_remove_recursive(ecm_nss_bond_notifier_dentry);
		return -1;
	}

	/*
	 * Register Link Aggregation interfaces with NSS driver
	 */
	ecm_nss_bond_notifier_nss_context = nss_register_lag_if(NSS_LAG0_INTERFACE_NUM, NULL, ecm_nss_bond_notifier_lag_event_cb, NULL);
	ecm_nss_bond_notifier_nss_context = nss_register_lag_if(NSS_LAG1_INTERFACE_NUM, NULL, ecm_nss_bond_notifier_lag_event_cb, NULL);
	ecm_nss_bond_notifier_nss_context = nss_register_lag_if(NSS_LAG2_INTERFACE_NUM, NULL, ecm_nss_bond_notifier_lag_event_cb, NULL);
	ecm_nss_bond_notifier_nss_context = nss_register_lag_if(NSS_LAG3_INTERFACE_NUM, NULL, ecm_nss_bond_notifier_lag_event_cb, NULL);

	/*
	 * Register Link Aggregation callbacks with the bonding driver
	 */
	ecm_nss_bond_notifier_bond_cb.bond_cb_link_up = ecm_nss_bond_notifier_bond_link_up;
	ecm_nss_bond_notifier_bond_cb.bond_cb_link_down = ecm_nss_bond_notifier_bond_link_down;
	ecm_nss_bond_notifier_bond_cb.bond_cb_release = ecm_nss_bond_notifier_bond_release;
	ecm_nss_bond_notifier_bond_cb.bond_cb_enslave = ecm_nss_bond_notifier_bond_enslave;
	bond_register_cb(&ecm_nss_bond_notifier_bond_cb);

	return 0;
}
EXPORT_SYMBOL(ecm_nss_bond_notifier_init);

/*
 * ecm_nss_bond_notifier_exit()
 */
void ecm_nss_bond_notifier_exit(void)
{
	DEBUG_INFO("ECM Bonding Notifier exit\n");

	/*
	 * Unregister from the bond driver
	 */
	bond_unregister_cb();

	/*
	 * Unregister Link Aggregation interfaces with NSS driver
	 */
	nss_unregister_lag_if(NSS_LAG0_INTERFACE_NUM);
	nss_unregister_lag_if(NSS_LAG1_INTERFACE_NUM);
	nss_unregister_lag_if(NSS_LAG2_INTERFACE_NUM);
	nss_unregister_lag_if(NSS_LAG3_INTERFACE_NUM);

	/*
	 * Remove the debugfs files recursively.
	 */
	if (ecm_nss_bond_notifier_dentry) {
		debugfs_remove_recursive(ecm_nss_bond_notifier_dentry);
	}
}
EXPORT_SYMBOL(ecm_nss_bond_notifier_exit);
