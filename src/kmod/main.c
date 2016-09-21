/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *          Patrick MacArthur <pam@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/net_namespace.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>
#include <linux/list.h>
#include <linux/kernel.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "obj.h"
#include "cm.h"
#include "verbs.h"

MODULE_AUTHOR("Bernard Metzler");
MODULE_DESCRIPTION("Userspace Software iWARP Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("0.3");

static int device_count = 1;
module_param(device_count, int, 0444);
MODULE_PARM_DESC(device_count, "Maximum number of devices usiw can attach to");

static struct siw_dev **siw_devlist;
DEFINE_SPINLOCK(siw_dev_lock);

static ssize_t show_sw_version(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct siw_dev *sdev = container_of(dev, struct siw_dev, ofa_dev.dev);

	return sprintf(buf, "%x\n", sdev->attrs.version);
}

static DEVICE_ATTR(sw_version, S_IRUGO, show_sw_version, NULL);

static struct device_attribute *siw_dev_attributes[] = {
	&dev_attr_sw_version,
};

static void usiw_device_release(struct device *dev)
{
	pr_info("%s device released\n", dev_name(dev));
}

static struct device usiw_generic_dma_device = {
	.archdata.dma_ops	= &usiw_dma_generic_ops,
	.init_name		= "usiw-v0",
	.release		= usiw_device_release
};

static struct bus_type usiw_bus = {
	.name	= "usiw",
};

static int siw_modify_port(struct ib_device *ofa_dev, u8 port, int mask,
			   struct ib_port_modify *props)
{
	return -EOPNOTSUPP;
}


static int siw_device_register(struct siw_dev *sdev)
{
	struct ib_device *ofa_dev = &sdev->ofa_dev;
	int rv, i;
	static int dev_id = 1;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 34)
	rv = ib_register_device(ofa_dev, NULL);
#else
	rv = ib_register_device(ofa_dev);
#endif
	if (rv) {
		pr_debug(DBG_DM "(dev=%s): "
		       "ib_register_device failed: rv=%d\n", ofa_dev->name, rv);
		return rv;
	}

	for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i) {
		rv = device_create_file(&ofa_dev->dev, siw_dev_attributes[i]);
		if (rv) {
			pr_debug(DBG_DM "(dev=%s): "
				"device_create_file failed: i=%d, rv=%d\n",
				ofa_dev->name, i, rv);
			ib_unregister_device(ofa_dev);
			return rv;
		}
	}
	siw_debugfs_add_device(sdev);

	sdev->attrs.vendor_part_id = dev_id++;

	sdev->is_registered = 1;
	return 0;
}

static void siw_device_deregister(struct siw_dev *sdev)
{
	int i;

	siw_debugfs_del_device(sdev);

	if (sdev->is_registered) {
		pr_debug(DBG_DM ": deregister %s netdev=%s\n", sdev->ofa_dev.name,
			sdev->netdev ? sdev->netdev->name : "(unattached)");

		for (i = 0; i < ARRAY_SIZE(siw_dev_attributes); ++i)
			device_remove_file(&sdev->ofa_dev.dev,
					   siw_dev_attributes[i]);

		ib_unregister_device(&sdev->ofa_dev);
	}
	WARN_ON(atomic_read(&sdev->num_ctx));
	WARN_ON(atomic_read(&sdev->num_qp));
	WARN_ON(atomic_read(&sdev->num_cq));
	WARN_ON(atomic_read(&sdev->num_pd));
	WARN_ON(atomic_read(&sdev->num_cep));

	i = 0;

	while (!list_empty(&sdev->cep_list)) {
		struct siw_cep *cep = list_entry(sdev->cep_list.next,
						 struct siw_cep, devq);
		list_del(&cep->devq);
		pr_debug(": Free CEP (0x%p), state: %d\n",
			cep, cep->state);
		kfree(cep);
		i++;
	}
	if (i)
		pr_warning("siw_device_deregister: free'd %d CEPs\n", i);

	sdev->is_registered = 0;
}

static void siw_device_destroy(struct siw_dev *sdev)
{
	if (!sdev)
		return;

	pr_debug(DBG_DM ": destroy siw device %s netdev=%s\n",
			sdev->ofa_dev.name,
			sdev->netdev ? sdev->netdev->name : "(unattached)");

	siw_idr_release(sdev);
	kfree(sdev->ofa_dev.iwcm);
	if (sdev->netdev)
		dev_put(sdev->netdev);
	ib_dealloc_device(&sdev->ofa_dev);
}


static struct siw_dev *siw_dev_from_netdev(struct net_device *dev)
{
	struct siw_dev *sdev;
	int x;
	for (x = 0; x < device_count; x++) {
		sdev = siw_devlist[x];
		if (sdev->netdev == dev)
			return sdev;
	}
	if (strncmp(dev->name, "kni", 3)) {
		return NULL;
	}
	if (kstrtoint(dev->name + 3, 10, &x) || x < 0 || x >= device_count) {
		return NULL;
	}
	sdev = siw_devlist[x];
	return sdev->netdev ? NULL : sdev;
}

static void siw_device_assign_netdev(struct siw_dev *sdev,
				     struct net_device *netdev)
{
	struct ib_device *ofa_dev = &sdev->ofa_dev;
	dev_hold(netdev);
	sdev->netdev = netdev;

	if (netdev->type != ARPHRD_LOOPBACK) {
		/* HACK HACK HACK: change the node GUID to the hardware address
		 * now that we know what it is */
		pr_debug(DBG_DM ": set node guid for %s based on MAC address\n",
				ofa_dev->name);
		memset(&ofa_dev->node_guid, 0, sizeof(ofa_dev->node_guid));
		memcpy(&ofa_dev->node_guid, netdev->dev_addr, 6);
	}
	WARN_ON_ONCE(ofa_dev->node_guid == 0);
}

static struct siw_dev *siw_device_create(int list_index)
{
	struct siw_dev *sdev = (struct siw_dev *)ib_alloc_device(sizeof *sdev);
	struct ib_device *ofa_dev;
	size_t gidlen;

	if (!sdev)
		return NULL;

	ofa_dev = &sdev->ofa_dev;

	ofa_dev->iwcm = kmalloc(sizeof(struct iw_cm_verbs), GFP_KERNEL);
	if (!ofa_dev->iwcm) {
		ib_dealloc_device(ofa_dev);
		return NULL;
	}

	snprintf(ofa_dev->name, IB_DEVICE_NAME_MAX,
			USIW_DEV_PREFIX "%d", list_index);

	ofa_dev->owner = THIS_MODULE;

	ofa_dev->uverbs_cmd_mask =
	    (1ull << IB_USER_VERBS_CMD_GET_CONTEXT) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_DEVICE) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_PORT) |
	    (1ull << IB_USER_VERBS_CMD_ALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_DEALLOC_PD) |
	    (1ull << IB_USER_VERBS_CMD_REG_MR) |
	    (1ull << IB_USER_VERBS_CMD_DEREG_MR) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_CQ) |
	    (1ull << IB_USER_VERBS_CMD_POLL_CQ) |
	    (1ull << IB_USER_VERBS_CMD_REQ_NOTIFY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_CQ) |
	    (1ull << IB_USER_VERBS_CMD_CREATE_QP) |
	    (1ull << IB_USER_VERBS_CMD_QUERY_QP) |
	    (1ull << IB_USER_VERBS_CMD_MODIFY_QP) |
	    (1ull << IB_USER_VERBS_CMD_DESTROY_QP);

	ofa_dev->node_type = RDMA_NODE_RNIC;
	memcpy(ofa_dev->node_desc, USIW_NODE_DESC, sizeof(USIW_NODE_DESC));

	/*
	 * Our device does not yet have an associated HW address,
	 * but connection mangagement lib expects gid != 0
	 */
	gidlen = min(strlen(ofa_dev->name), (size_t)6);
	memcpy(&ofa_dev->node_guid, ofa_dev->name, gidlen);

	/*
	 * Current model (one-to-one device association):
	 * One Softiwarp device per net_device or, equivalently,
	 * per physical port.
	 */
	ofa_dev->phys_port_cnt = 1;

	ofa_dev->num_comp_vectors = 1;
	ofa_dev->dma_device = &usiw_generic_dma_device;
	ofa_dev->query_device = siw_query_device;
	ofa_dev->query_port = siw_query_port;
	ofa_dev->query_qp = siw_query_qp;
	ofa_dev->modify_port = siw_modify_port;
	ofa_dev->query_pkey = siw_query_pkey;
	ofa_dev->query_gid = siw_query_gid;
	ofa_dev->alloc_ucontext = siw_alloc_ucontext;
	ofa_dev->dealloc_ucontext = siw_dealloc_ucontext;
	ofa_dev->mmap = siw_mmap;
	ofa_dev->alloc_pd = siw_alloc_pd;
	ofa_dev->dealloc_pd = siw_dealloc_pd;
	ofa_dev->create_ah = siw_create_ah;
	ofa_dev->destroy_ah = siw_destroy_ah;
	ofa_dev->create_qp = siw_create_qp;
	ofa_dev->modify_qp = siw_ofed_modify_qp;
	ofa_dev->destroy_qp = siw_destroy_qp;
	ofa_dev->create_cq = siw_create_cq;
	ofa_dev->destroy_cq = siw_destroy_cq;
	ofa_dev->resize_cq = NULL;
	ofa_dev->poll_cq = siw_poll_cq;
	ofa_dev->get_dma_mr = siw_get_dma_mr;
	ofa_dev->reg_user_mr = siw_reg_user_mr;
	ofa_dev->dereg_mr = siw_dereg_mr;
	ofa_dev->alloc_mw = NULL;
	ofa_dev->dealloc_mw = NULL;

#ifdef HAVE_IB_REG_PHYS_MR
	ofa_dev->reg_phys_mr = NULL;
	ofa_dev->rereg_phys_mr = NULL;
#endif
#ifdef HAVE_IB_BIND_MW
	ofa_dev->bind_mw = NULL;
#endif

	ofa_dev->create_srq = siw_create_srq;
	ofa_dev->modify_srq = siw_modify_srq;
	ofa_dev->query_srq = siw_query_srq;
	ofa_dev->destroy_srq = siw_destroy_srq;
	ofa_dev->post_srq_recv = siw_post_srq_recv;

	ofa_dev->attach_mcast = NULL;
	ofa_dev->detach_mcast = NULL;
	ofa_dev->process_mad = siw_no_mad;

	ofa_dev->req_notify_cq = siw_req_notify_cq;
	ofa_dev->post_send = siw_post_send;
	ofa_dev->post_recv = siw_post_receive;

	ofa_dev->dma_ops = &siw_dma_mapping_ops;

	ofa_dev->iwcm->connect = siw_connect;
	ofa_dev->iwcm->accept = siw_accept;
	ofa_dev->iwcm->reject = siw_reject;
	ofa_dev->iwcm->create_listen = siw_create_listen;
	ofa_dev->iwcm->destroy_listen = siw_destroy_listen;
	ofa_dev->iwcm->add_ref = siw_qp_get_ref;
	ofa_dev->iwcm->rem_ref = siw_qp_put_ref;
	ofa_dev->iwcm->get_qp = siw_get_ofaqp;
	/*
	 * set and register sw version + user if type
	 */
	sdev->attrs.version = VERSION_ID_USIW;

	sdev->attrs.vendor_id = USIW_VENDOR_ID;
	sdev->attrs.vendor_part_id = USIW_VENDOR_PART_ID;
	sdev->attrs.sw_version = VERSION_ID_USIW;
	sdev->attrs.max_qp = SIW_MAX_QP;
	sdev->attrs.max_ird = SIW_MAX_IRD;
	sdev->attrs.max_ord = SIW_MAX_ORD;
	sdev->attrs.cap_flags = 0;
	sdev->attrs.max_cq = SIW_MAX_CQ;
	sdev->attrs.max_pd = SIW_MAX_PD;

	siw_idr_init(sdev);
	INIT_LIST_HEAD(&sdev->cep_list);

	atomic_set(&sdev->num_ctx, 0);
	atomic_set(&sdev->num_qp, 0);
	atomic_set(&sdev->num_cq, 0);
	atomic_set(&sdev->num_pd, 0);
	atomic_set(&sdev->num_cep, 0);

	sdev->is_registered = 0;

	return sdev;
}



static int siw_netdev_event(struct notifier_block *nb, unsigned long event,
			    void *arg)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
	struct net_device	*netdev = arg;
#else
	struct net_device	*netdev = netdev_notifier_info_to_dev(arg);
#endif
	struct in_device	*in_dev;
	struct siw_dev		*sdev;

	pr_debug(DBG_DM " (dev=%s): Event %lu\n", netdev->name, event);

	if (dev_net(netdev) != &init_net)
		goto done;

	if (!spin_trylock(&siw_dev_lock))
		/* The module is being removed */
		goto done;

	sdev = siw_dev_from_netdev(netdev);
	if (!sdev)
		goto unlock;

	if (WARN_ONCE(!sdev->is_registered, "IB device %s not registered with IB core\n",
					sdev->ofa_dev.name))
		goto unlock;

	switch (event) {

	case NETDEV_UP:
		in_dev = in_dev_get(netdev);
		if (!in_dev) {
			pr_debug(DBG_DM ": %s: no in_dev\n", netdev->name);
			sdev->state = IB_PORT_INIT;
			break;
		}

		if (in_dev->ifa_list) {
			sdev->state = IB_PORT_ACTIVE;
			siw_port_event(sdev, 1, IB_EVENT_PORT_ACTIVE);
		} else {
			pr_debug(DBG_DM ": %s: no ifa\n", netdev->name);
			sdev->state = IB_PORT_INIT;
		}
		in_dev_put(in_dev);

		pr_debug(DBG_DM ": Interface '%s' for device '%s' up, HWaddr=%02x.%02x.%02x.%02x.%02x.%02x\n",
				sdev->netdev->name, sdev->ofa_dev.name,
				*(u8 *)sdev->netdev->dev_addr,
				*((u8 *)sdev->netdev->dev_addr + 1),
				*((u8 *)sdev->netdev->dev_addr + 2),
				*((u8 *)sdev->netdev->dev_addr + 3),
				*((u8 *)sdev->netdev->dev_addr + 4),
				*((u8 *)sdev->netdev->dev_addr + 5));

		break;

	case NETDEV_DOWN:
		sdev->state = IB_PORT_DOWN;
		siw_port_event(sdev, 1, IB_EVENT_PORT_ERR);
		break;

	case NETDEV_REGISTER:
		siw_device_assign_netdev(sdev, netdev);
		sdev->state = IB_PORT_INIT;
		pr_debug(DBG_DM ": attach siw device %s to netdev %s\n",
				sdev->ofa_dev.name, netdev->name);
		break;

	case NETDEV_UNREGISTER:
		sdev->state = IB_PORT_DOWN;
		siw_port_event(sdev, 1, IB_EVENT_PORT_ERR);
		/* FIXME: I assume this can race with something that needs
		 * netdev */
		dev_put(sdev->netdev);
		sdev->netdev = NULL;
		break;

	case NETDEV_CHANGEADDR:
		/* HACK HACK HACK: change the node GUID to the hardware address
		 * now that we know what it is */
		pr_debug(DBG_DM ": set node guid for %s based on MAC address\n",
				sdev->ofa_dev.name);
		memset(&sdev->ofa_dev.node_guid, 0,
				sizeof(sdev->ofa_dev.node_guid));
		memcpy(&sdev->ofa_dev.node_guid, netdev->dev_addr, 6);
		siw_port_event(sdev, 1, IB_EVENT_LID_CHANGE);

		break;
	/*
	 * Todo: Other netdev events are currently not handled.
	 */
	default:
		pr_debug(DBG_DM " sdev=%s got unhandled netdev event %lu for netdev=%s\n",
				sdev->ofa_dev.name, event, netdev->name);
		break;
	}
unlock:
	spin_unlock(&siw_dev_lock);
done:
	return NOTIFY_OK;
}

static struct notifier_block siw_netdev_nb = {
	.notifier_call = siw_netdev_event,
};

/*
 * siw_init_module - Initialize Softiwarp module and register with netdev
 *                   subsystem to create Softiwarp devices per net_device
 */
static __init int siw_init_module(void)
{
	int x, rv;

	/*
	 * The xprtrdma module needs at least some rudimentary bus to set
	 * some devices path MTU.
	 */
	rv = bus_register(&usiw_bus);
	if (rv)
		goto out_nobus;

	usiw_generic_dma_device.bus = &usiw_bus;

	rv = device_register(&usiw_generic_dma_device);
	if (rv)
		goto out;

	rv = siw_cm_init();
	if (rv)
		goto out_unregister;

	siw_debug_init();

	siw_devlist = kcalloc(device_count, sizeof(*siw_devlist), GFP_KERNEL);
	if (!siw_devlist) {
		goto out_unregister;
	}
	for (x = 0; x < device_count; x++) {
		siw_devlist[x] = siw_device_create(x);
		if (!siw_devlist[x]) {
			goto out_free_devlist;
		}
		if (siw_device_register(siw_devlist[x])) {
			goto out_free_devlist;
		}
	}

	rv = register_netdevice_notifier(&siw_netdev_nb);
	if (rv) {
		siw_debugfs_delete();
		goto out_unregister;
	}

	pr_info("Userspace SoftiWARP kernel support attached\n");
	return 0;

out_free_devlist:
	for (x = 0; x < device_count; x++) {
		if (!siw_devlist[x]) {
			break;
		}
		if (siw_devlist[x]->is_registered) {
			siw_device_deregister(siw_devlist[x]);
		}
		siw_device_destroy(siw_devlist[x]);
	}
	kfree(siw_devlist);
out_unregister:
	device_unregister(&usiw_generic_dma_device);

out:
	bus_unregister(&usiw_bus);
out_nobus:
	pr_info("Userspace SoftIWARP kernel support attach failed. Error: %d\n",
			rv);
	siw_cm_exit();

	return rv;
}


static void __exit siw_exit_module(void)
{
	int x;

	spin_lock(&siw_dev_lock);
	unregister_netdevice_notifier(&siw_netdev_nb);
	spin_unlock(&siw_dev_lock);

	siw_cm_exit();

	for (x = 0; x < device_count; x++) {
		struct siw_dev *sdev = siw_devlist[x];
		if (!sdev)
			continue;
		if (sdev->is_registered)
			siw_device_deregister(sdev);
		siw_device_destroy(sdev);
	}
	kfree(siw_devlist);
	siw_debugfs_delete();

	device_unregister(&usiw_generic_dma_device);

	bus_unregister(&usiw_bus);

	pr_info("Userspace SoftiWARP kernel support detached\n");
}

module_init(siw_init_module);
module_exit(siw_exit_module);