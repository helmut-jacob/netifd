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
#ifndef __NETIFD_SYSTEM_OVS_H
#define __NETIFD_SYSTEM_OVS_H

#include "system.h"

struct ovs_config {
	bool empty;
	int tag;
	char *base;
};

void system_ovs_if_clear_state(struct device *dev);
int system_ovs_delbr(struct device *ovs);
int system_ovs_addbr(struct device *ovs, struct ovs_config *cfg);
int system_ovs_addport(struct device *ovs, struct device *dev);
int system_ovs_delport(struct device *ovs, struct device *dev);

#endif
