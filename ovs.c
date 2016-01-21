/*
 * netifd - network interface daemon
 * Copyright (C) 2013 Helmut Schaa <helmut.schaa@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

#include "netifd.h"
#include "device.h"
#include "interface.h"
#include "system-ovs.h"

enum {
	OVS_ATTR_IFNAME,
	OVS_ATTR_BASE,
	OVS_ATTR_TAG,
	OVS_ATTR_EMPTY,
	OVS_ATTR_TYPE,
	OVS_ATTR_OPTIONS,
	__OVS_ATTR_MAX
};

static const struct blobmsg_policy ovs_attrs[__OVS_ATTR_MAX] = {
	[OVS_ATTR_IFNAME] = { "ifname", BLOBMSG_TYPE_ARRAY },
	[OVS_ATTR_BASE] = { "ovs_base", BLOBMSG_TYPE_STRING },
	[OVS_ATTR_TAG] = { "ovs_tag", BLOBMSG_TYPE_INT32 },
	[OVS_ATTR_EMPTY] = { "ovs_empty", BLOBMSG_TYPE_BOOL },
	[OVS_ATTR_TYPE] = { "ovs_type", BLOBMSG_TYPE_STRING },
	[OVS_ATTR_OPTIONS] = { "ovs_options", BLOBMSG_TYPE_STRING },
};

static const struct uci_blob_param_info ovs_attr_info[__OVS_ATTR_MAX] = {
	[OVS_ATTR_IFNAME] = { .type = BLOBMSG_TYPE_STRING },
};

static const struct uci_blob_param_list ovs_attr_list = {
	.n_params = __OVS_ATTR_MAX,
	.params = ovs_attrs,
	.info = ovs_attr_info,

	.n_next = 1,
	.next = { &device_attr_list },
};

static struct device *ovs_create(const char *name, struct blob_attr *attr);
static void ovs_config_init(struct device *dev);
static void ovs_free(struct device *dev);
static void ovs_dump_info(struct device *dev, struct blob_buf *b);
enum dev_change_type
ovs_reload(struct device *dev, struct blob_attr *attr);

const struct device_type ovs_device_type = {
	.name = "OpenVSwitch",
	.config_params = &ovs_attr_list,

	.create = ovs_create,
	.config_init = ovs_config_init,
	.reload = ovs_reload,
	.free = ovs_free,
	.dump_info = ovs_dump_info,
};

struct ovs_state {
	struct device dev;
	device_state_cb set_state;

	struct blob_attr *config_data;
	struct ovs_config config;
	struct blob_attr *ifnames;
	bool active;
	bool force_active;

	struct vlist_tree ports;
	int n_present;

	struct ovs_base *base;
};

struct ovs_port {
	struct vlist_node node;
	struct ovs_state *ost;
	struct device_user dev;
	bool present;
	char name[];
};

struct ovs_base {
	struct ovs_state *ost;
	struct device_user dev;
	bool present;
};

static void ovs_set_present(struct ovs_state *ost);

static int
ovs_disable_base(struct ovs_base *ob)
{
	if (!ob->present)
		return 0;

	device_release(&ob->dev);

	return 0;
}

static int
ovs_enable_base(struct ovs_base *ob)
{
	int ret;

	if (!ob->present)
		return 0;

	ret = device_claim(&ob->dev);
	if (ret < 0)
		goto error;

	return 0;

error:
	ob->present = false;
	return ret;
}

static void
ovs_remove_base(struct ovs_base *ob)
{
	struct ovs_state *ost = ob->ost;

	if (!ob->present)
		return;

	if (ost->dev.active)
		ovs_disable_base(ob);

	ob->present = false;
	ovs_set_present(ost);
}

static void
ovs_free_base(struct ovs_base *ob)
{
	ovs_remove_base(ob);
	device_remove_user(&ob->dev);
	free(ob);
}

static int
ovs_disable_port(struct ovs_port *op)
{
	struct ovs_state *ost = op->ost;

	if (!op->present)
		return 0;

	system_ovs_delport(&ost->dev, op->dev.dev);
	device_release(&op->dev);

	return 0;
}

static int
ovs_enable_port(struct ovs_port *op)
{
	struct ovs_state *ost = op->ost;
	int ret;

	if (!op->present)
		return 0;

	ret = device_claim(&op->dev);
	if (ret < 0)
		goto error;

	ret = system_ovs_addport(&ost->dev, op->dev.dev);
	if (ret < 0) {
		D(DEVICE, "Bridge device %s could not be added\n", op->dev.dev->ifname);
		goto error;
	}

	ret = system_ovs_settype(op->dev.dev, &ost->config);
	if (ret < 0)
		D(DEVICE, "Bridge type %s of %s could not be set\n", ost->config.type, op->dev.dev->ifname);

	ret = system_ovs_setoptions(op->dev.dev, &ost->config);
	if (ret < 0)
		D(DEVICE, "Bridge options %s of %s could not be set\n", ost->config.options, op->dev.dev->ifname);

	return 0;

error:
	op->present = false;
	ost->n_present--;
	return ret;
}

static void
ovs_remove_port(struct ovs_port *op)
{
	struct ovs_state *ost = op->ost;

	if (!op->present)
		return;

	if (ost->dev.active)
		ovs_disable_port(op);

	op->present = false;
	op->ost->n_present--;

	ovs_set_present(ost);
}

static void
ovs_free_port(struct ovs_port *op)
{
	struct device *dev = op->dev.dev;

	ovs_remove_port(op);
	device_remove_user(&op->dev);

	/*
	 * When reloading the config and moving a device from one bridge to
	 * another, the other bridge may have tried to claim this device
	 * before it was removed here.
	 * Ensure that claiming the device is retried by toggling its present
	 * state
	 */
	if (dev->present) {
		device_set_present(dev, false);
		device_set_present(dev, true);
	}

	free(op);
}

static void
ovs_port_cb(struct device_user *dev, enum device_event ev)
{
	struct ovs_port *op = container_of(dev, struct ovs_port, dev);
	struct ovs_state *ost = op->ost;

	switch (ev) {
	case DEV_EVENT_ADD:
		assert(!op->present);

		op->present = true;
		ost->n_present++;

		if (ost->dev.active)
			ovs_enable_port(op);
		else if (ost->n_present == 1)
			ovs_set_present(ost);

		break;
	case DEV_EVENT_REMOVE:
		if (dev->hotplug) {
			vlist_delete(&ost->ports, &op->node);
			return;
		}

		if (op->present)
			ovs_remove_port(op);

		break;
	default:
		return;
	}
}

static void
ovs_base_cb(struct device_user *dev, enum device_event ev)
{
	struct ovs_base *ob = container_of(dev, struct ovs_base, dev);
	struct ovs_state *ost = ob->ost;
	

	switch (ev) {
	case DEV_EVENT_ADD:
		ob->present = true;
		ovs_enable_base(ob);
		ovs_set_present(ost);

	case DEV_EVENT_REMOVE:
		if (ob->present)
			ovs_remove_base(ob);

		break;
	default:
		return;
	}
}

static int
ovs_set_down(struct ovs_state *ost)
{
	struct ovs_port *op;

	ost->set_state(&ost->dev, false);

	vlist_for_each_element(&ost->ports, op, node)
		ovs_disable_port(op);

	if (ost->base)
		ovs_disable_base(ost->base);

	system_ovs_delbr(&ost->dev);

	return 0;
}

static int
ovs_set_up(struct ovs_state *ost)
{
	struct ovs_port *op;
	int ret;

	if (!ost->force_active && !ost->n_present)
		return -ENOENT;

	if (ost->base)
		ovs_enable_base(ost->base);

	ret = system_ovs_addbr(&ost->dev, &ost->config);
	if (ret < 0)
		goto out;

	vlist_for_each_element(&ost->ports, op, node)
		ovs_enable_port(op);

	if (!ost->force_active && !ost->n_present) {
		/* initialization of all port interfaces failed */
		system_ovs_delbr(&ost->dev);
		ovs_set_present(ost);
		return -ENOENT;
	}

	ret = ost->set_state(&ost->dev, true);
	if (ret < 0)
		ovs_set_down(ost);

out:
	return ret;
}

static int
ovs_set_state(struct device *dev, bool up)
{
	struct ovs_state *ost;

	ost = container_of(dev, struct ovs_state, dev);

	if (up)
		return ovs_set_up(ost);
	else
		return ovs_set_down(ost);
}

static struct ovs_port *
ovs_create_port(struct ovs_state *ost, struct device *dev, bool hotplug)
{
	struct ovs_port *op;

	op = calloc(1, sizeof(*op) + strlen(dev->ifname) + 1);
	op->ost = ost;
	op->dev.cb = ovs_port_cb;
	op->dev.hotplug = hotplug;
	strcpy(op->name, dev->ifname);
	op->dev.dev = dev;
	vlist_add(&ost->ports, &op->node, op->name);
	if (hotplug)
		op->node.version = -1;

	return op;
}

static void
ovs_port_update(struct vlist_tree *tree, struct vlist_node *node_new,
		     struct vlist_node *node_old)
{
	struct ovs_port *op;
	struct device *dev;

	if (node_new) {
		op = container_of(node_new, struct ovs_port, node);

		if (node_old) {
			free(op);
			return;
		}

		dev = op->dev.dev;
		op->dev.dev = NULL;
		device_add_user(&op->dev, dev);
	}


	if (node_old) {
		op = container_of(node_old, struct ovs_port, node);
		ovs_free_port(op);
	}
}


static void
ovs_add_port(struct ovs_state *ost, const char *name)
{
	struct device *dev;

	dev = device_get(name, true);
	if (!dev)
		return;

	ovs_create_port(ost, dev, false);
}

static struct ovs_base *
ovs_create_base(struct ovs_state *ost, struct device *dev, bool hotplug)
{
	struct ovs_base *ob;

	ob = calloc(1, sizeof(*ob));
	if (!ob)
		return NULL;

	ob->ost = ost;
	ob->dev.cb = ovs_base_cb;

	device_add_user(&ob->dev, dev);

	return ob;
}
static void
ovs_add_base(struct ovs_state *ost, const char *name)
{
	struct device *dev;

	dev = device_get(name, true);
	if (!dev)
		return;

	ovs_create_base(ost, dev, false);
}

static int
ovs_hotplug_add(struct device *dev, struct device *port)
{
	struct ovs_state *ost = container_of(dev, struct ovs_state, dev);

	ovs_create_port(ost, port, true);

	return 0;
}

static int
ovs_hotplug_del(struct device *dev, struct device *port)
{
	struct ovs_state *ost = container_of(dev, struct ovs_state, dev);
	struct ovs_port *op;

	op = vlist_find(&ost->ports, port->ifname, op, node);
	if (!op)
		return UBUS_STATUS_NOT_FOUND;

	vlist_delete(&ost->ports, &op->node);
	return 0;
}

static int
ovs_hotplug_prepare(struct device *dev)
{
	struct ovs_state *ost;

	ost = container_of(dev, struct ovs_state, dev);
	ost->force_active = true;
	ovs_set_present(ost);

	return 0;
}

static const struct device_hotplug_ops ovs_ops = {
	.prepare = ovs_hotplug_prepare,
	.add = ovs_hotplug_add,
	.del = ovs_hotplug_del
};

static void
ovs_free(struct device *dev)
{
	struct ovs_state *ost;
	ost = container_of(dev, struct ovs_state, dev);

	if (ost->base)
		ovs_free_base(ost->base);
	vlist_flush_all(&ost->ports);
	free(ost);
}

static void
ovs_dump_info(struct device *dev, struct blob_buf *b)
{
	struct ovs_state *ost;
	struct ovs_port *op;
	void *list;

	ost = container_of(dev, struct ovs_state, dev);

	system_if_dump_info(dev, b);
	list = blobmsg_open_array(b, "ovs-ports");

	vlist_for_each_element(&ost->ports, op, node)
		blobmsg_add_string(b, NULL, op->dev.dev->ifname);

	blobmsg_close_array(b, list);
	if (ost->base)
		blobmsg_add_string(b, "ovs_base", ost->base->dev.dev->ifname);
}

static void
ovs_set_present(struct ovs_state *ost)
{
	bool present = false;
	if (ost->base) {
		/* Base device has to be available first */
		if (!ost->base->present)
			goto out;
	}

	if (ost->config.empty) {
		present = true;
		goto out;
	}

	ost->force_active = false;
	if (ost->n_present == 0)
		goto out;

	present = true;
out:
	device_set_present(&ost->dev, present);
}

static void
ovs_config_init(struct device *dev)
{
	struct ovs_state *ost;
	struct blob_attr *cur;
	int rem;

	ost = container_of(dev, struct ovs_state, dev);

	if (ost->config.empty) {
		ost->force_active = true;
		ovs_set_present(ost);
	}

	if (ost->config.base) {
		/* Pseudo bridge, requires base */
		ovs_add_base(ost, ost->config.base);
	}

	vlist_update(&ost->ports);
	if (ost->ifnames) {
		blobmsg_for_each_attr(cur, ost->ifnames, rem) {
			ovs_add_port(ost, blobmsg_data(cur));
		}
	}
	vlist_flush(&ost->ports);
}

static void
ovs_apply_settings(struct ovs_state *ost, struct blob_attr **tb)
{
	struct ovs_config *cfg = &ost->config;

	/* defaults */
	cfg->tag = 0;
	cfg->base = NULL;
	cfg->empty = false;
	cfg->type = NULL;
	cfg->options = NULL;

	if (tb[OVS_ATTR_TAG] && tb[OVS_ATTR_BASE] ) {
		cfg->tag = blobmsg_get_u32(tb[OVS_ATTR_TAG]);
		cfg->base = blobmsg_get_string(tb[OVS_ATTR_BASE]);
	}

	if (tb[OVS_ATTR_EMPTY])
		cfg->empty = blobmsg_get_bool(tb[OVS_ATTR_EMPTY]);

	if (tb[OVS_ATTR_TYPE])
		cfg->type = blobmsg_get_string(tb[OVS_ATTR_TYPE]);

	if (tb[OVS_ATTR_OPTIONS])
		cfg->options = blobmsg_get_string(tb[OVS_ATTR_OPTIONS]);
}

enum dev_change_type
ovs_reload(struct device *dev, struct blob_attr *attr)
{
	struct blob_attr *tb_dev[__DEV_ATTR_MAX];
	struct blob_attr *tb_br[__OVS_ATTR_MAX];
	enum dev_change_type ret = DEV_CONFIG_APPLIED;
	unsigned long diff;
	struct ovs_state *ost;

	BUILD_BUG_ON(sizeof(diff) < __OVS_ATTR_MAX / 8);
	BUILD_BUG_ON(sizeof(diff) < __DEV_ATTR_MAX / 8);

	ost = container_of(dev, struct ovs_state, dev);

	blobmsg_parse(device_attr_list.params, __DEV_ATTR_MAX, tb_dev,
		blob_data(attr), blob_len(attr));
	blobmsg_parse(ovs_attrs, __OVS_ATTR_MAX, tb_br,
		blob_data(attr), blob_len(attr));

	ost->ifnames = tb_br[OVS_ATTR_IFNAME];
	device_init_settings(dev, tb_dev);
	ovs_apply_settings(ost, tb_br);

	if (ost->config_data) {
		struct blob_attr *otb_dev[__DEV_ATTR_MAX];
		struct blob_attr *otb_br[__OVS_ATTR_MAX];

		blobmsg_parse(device_attr_list.params, __DEV_ATTR_MAX, otb_dev,
			blob_data(ost->config_data), blob_len(ost->config_data));

		diff = 0;
		uci_blob_diff(tb_dev, otb_dev, &device_attr_list, &diff);
		if (diff)
			ret = DEV_CONFIG_RESTART;

		blobmsg_parse(ovs_attrs, __OVS_ATTR_MAX, otb_br,
			blob_data(ost->config_data), blob_len(ost->config_data));

		diff = 0;
		uci_blob_diff(tb_br, otb_br, &ovs_attr_list, &diff);
		if (diff & ~(1 << OVS_ATTR_IFNAME))
			ret = DEV_CONFIG_RESTART;

		ovs_config_init(dev);
	}

	ost->config_data = attr;
	return ret;
}

static struct device *
ovs_create(const char *name, struct blob_attr *attr)
{
	struct ovs_state *ost;
	struct device *dev = NULL;

	ost = calloc(1, sizeof(*ost));
	if (!ost)
		return NULL;

	dev = &ost->dev;
	device_init(dev, &ovs_device_type, name);
	dev->config_pending = true;

	ost->set_state = dev->set_state;
	dev->set_state = ovs_set_state;

	dev->hotplug_ops = &ovs_ops;

	vlist_init(&ost->ports, avl_strcmp, ovs_port_update);
	ost->ports.keep_old = true;
	ovs_reload(dev, attr);

	return dev;
}
