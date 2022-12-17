/*
 * drivers/power/autocut_charger.c
 * drivers/power/supply/autocut_charger.c
 *
 * AutoCut Charger.
 *
 * Copyright (C) 2022, RooGhz720 <https://github.com/RooGhz720>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

static struct delayed_work autocut_charger_work;
static bool checked = false;
static bool full_disable_charging = false;

static void autocut_charger_worker(struct work_struct *work)
{
	struct power_supply *batt_psy = power_supply_get_by_name("battery");
	struct power_supply *usb_psy = power_supply_get_by_name("usb");
	union power_supply_propval present = {0,}, charging_enabled = {0,}, val = {0, };
	union power_supply_propval bat_percent, check;
	int rc = 0;

	if (!checked) {
		/* mark as already checked */
		checked = true;

		/*
		 * Check compatibility ps properties
		 * if not support at all, then exit service.
		 */
		rc = power_supply_get_property(batt_psy,
		     POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &check);
		if (rc) {
			rc = power_supply_get_property(batt_psy,
			     POWER_SUPPLY_PROP_CHARGING_ENABLED, &check);
			if (rc) {
				pr_err("autocut_charger: Charging driver not supported!\n");
				cancel_delayed_work_sync(&autocut_charger_work);
				return;
			}
			full_disable_charging = true;
		}
	}

	if (full_disable_charging)
		power_supply_get_property(batt_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &charging_enabled);
	else
		power_supply_get_property(batt_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &charging_enabled);

	power_supply_get_property(batt_psy,
			POWER_SUPPLY_PROP_CAPACITY, &bat_percent);
	power_supply_get_property(usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &present);

	if (full_disable_charging) {
		if (present.intval) {
			if (charging_enabled.intval && bat_percent.intval >= 100) {
				val.intval = false;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to disable battery charging!\n", __func__);
			} else if (!charging_enabled.intval && bat_percent.intval <= 90) {
				val.intval = true;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to enable battery charging!\n", __func__);
			}
		} else {
			if (!charging_enabled.intval) {
				val.intval = true;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to enable battery charging!\n", __func__);
			}
		}
	} else {
		if (present.intval) {
			if (charging_enabled.intval && bat_percent.intval >= 99) {
				val.intval = false;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to disable battery charging!\n", __func__);
			} else if (!charging_enabled.intval && bat_percent.intval < 100) {
				val.intval = true;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to enable battery charging!\n", __func__);
			}
		} else {
			if (!charging_enabled.intval) {
				val.intval = true;
				rc = power_supply_set_property(batt_psy,
					POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &val);
				if (rc)
					pr_err("%s: Failed to enable battery charging!\n", __func__);
			}
		}
	}

	schedule_delayed_work(&autocut_charger_work, msecs_to_jiffies(1000));
}

static int __init autocut_charger_init(void)
{
	if (!strstr(saved_command_line, "androidboot.mode=charger")) {
		INIT_DELAYED_WORK(&autocut_charger_work, autocut_charger_worker);
		/* start worker in at least 20 seconds after boot completed */
		schedule_delayed_work(&autocut_charger_work, msecs_to_jiffies(20000));
		pr_info("%s: Initialized.\n", __func__);
	}

	return 0;
}
late_initcall(autocut_charger_init);

static void __exit autocut_charger_exit(void)
{
	if (!strstr(saved_command_line, "androidboot.mode=charger"))
		cancel_delayed_work_sync(&autocut_charger_work);
}
module_exit(autocut_charger_exit);
