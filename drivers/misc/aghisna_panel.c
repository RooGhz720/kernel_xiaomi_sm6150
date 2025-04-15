/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2022 ZyCromerZ
 * Copyright (C) 2023 RooGhz
 *
 * Inspired from lyb display
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <misc/aghisna_panel.h>

bool __read_mostly jenis_dimensi = false;
module_param(jenis_dimensi, bool, 0644);

static int __init read_old_mdsi(char *s)
{
    int status;
	if (s)
		status = simple_strtoul(s, NULL, 0);

	if ( status > 0 ) {
		jenis_dimensi = true;
	} else {
		jenis_dimensi = false;
	}
	return 1;
}
__setup("aghisna.dimen=", read_old_mdsi);

static int __init prepare_driver_init(void) {
 printk(KERN_INFO "aghisna display initialized");
 return 0;
}
static void __exit prepare_driver_exit(void) {
 printk(KERN_INFO "aghisna display exit");
}

module_init(prepare_driver_init);
module_exit(prepare_driver_exit);
