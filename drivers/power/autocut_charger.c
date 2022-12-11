/*
 * drivers/power/autocut_charger.c
 *
 * AutoCut Charger.
 *
 * Copyright (C) 2019, Ryan Andri <https://github.com/ryan-andri>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include "power_supply.h"

static struct delayed_work autocut_charger_work;

static int battery_charging_enabled(struct power_supply *batt_psy, bool enable)
{
	const union power_supply_propval ret = {enable,};

	if (batt_psy->set_property)
		return batt_psy->set_property(batt_psy,
				POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED,
				&ret);

	return 0;
}

static void autocut_charger_worker(struct work_struct *work)
{
	struct power_supply *batt_psy = power_supply_get_by_name("battery");
	struct power_supply *usb_psy = power_supply_get_by_name("usb");
	union power_supply_propval status, bat_percent;
	union power_supply_propval present = {0,}, charging_enabled = {0,};
	int ms_timer = 1000, rc = 0;

	/* re-schdule and increase the timer if not ready */
	if (!batt_psy->get_property || !usb_psy->get_property)
	{
		ms_timer = 10000;
		goto reschedule;
	}

	/* get values from /sys/class/power_supply/battery */
	batt_psy->get_property(batt_psy,
			POWER_SUPPLY_PROP_STATUS, &status);
	batt_psy->get_property(batt_psy,
			POWER_SUPPLY_PROP_CAPACITY, &bat_percent);
	batt_psy->get_property(batt_psy,
			POWER_SUPPLY_PROP_BATTERY_CHARGING_ENABLED, &charging_enabled);

	/* get values from /sys/class/power_supply/usb */
	usb_psy->get_property(usb_psy,
			POWER_SUPPLY_PROP_PRESENT, &present);

	if (status.intval == POWER_SUPPLY_STATUS_CHARGING || present.intval)
	{
		if (charging_enabled.intval && bat_percent.intval >= 99)
		{
			rc = battery_charging_enabled(batt_psy, 0);
			if (rc)
				pr_err("Failed to disable battery charging!\n");
		}
		else if (!charging_enabled.intval && bat_percent.intval < 100)
		{
			rc = battery_charging_enabled(batt_psy, 1);
			if (rc)
				pr_err("Failed to enable battery charging!\n");
		}
	}
	else if (bat_percent.intval < 100 || !present.intval)
	{
		if (!charging_enabled.intval)
		{
			rc = battery_charging_enabled(batt_psy, 1);
			if (rc)
				pr_err("Failed to enable battery charging!\n");
		}
	}

reschedule:
	schedule_delayed_work(&autocut_charger_work, msecs_to_jiffies(ms_timer));
}

static int __init autocut_charger_init(void)
{
	INIT_DELAYED_WORK(&autocut_charger_work, autocut_charger_worker);
	schedule_delayed_work(&autocut_charger_work, msecs_to_jiffies(10000));

	pr_info("%s: Initialized.\n", __func__);

	return 0;
}
late_initcall(autocut_charger_init);

