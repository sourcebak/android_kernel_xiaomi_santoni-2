/*
 * drivers/power/charger_limiter.c
 *
 * Charger limiter.
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

/* DO NOT EDIT */
extern int power_supply_set_charging_enabled(struct power_supply *psy, bool enable);
static struct kobject *charger_limiter;
static struct delayed_work charger_limiter_work;
static struct workqueue_struct *charge_limiter_wq;
static bool charging_off = false;
static bool cl_is_enabled = false;
static bool charge_should_off = false;

/* Tunables */
static int enabled = 0;
static int charging_below = 95;
static int charging_limit = 100;

static void reschedule_worker(int ms)
{
	queue_delayed_work(charge_limiter_wq,
		&charger_limiter_work, msecs_to_jiffies(ms));
}

static void enable_disable_charging(struct power_supply *batt_psy, bool enable)
{
	int rc = 0;

	if (charging_off && enable) {
		rc = power_supply_set_charging_enabled(batt_psy, 1);
		if (!rc)
			charging_off = false;
	} else if (!charging_off && !enable) {
		rc = power_supply_set_charging_enabled(batt_psy, 0);
		if (!rc)
			charging_off = true;
	}
}

static void charger_limiter_worker(struct work_struct *work)
{
	struct power_supply *batt_psy = power_supply_get_by_name("battery");
	struct power_supply *usb_psy = power_supply_get_by_name("usb");
	union power_supply_propval status, bat_percent, usb_connect = {0,};
	int rc = 0, ms_timer = 1000;

	if (!batt_psy->get_property || !usb_psy->get_property) {
		ms_timer = 5000;
		goto reschedule;
	}

	batt_psy->get_property(batt_psy, POWER_SUPPLY_PROP_STATUS, &status);
	batt_psy->get_property(batt_psy, POWER_SUPPLY_PROP_CAPACITY, &bat_percent);
	usb_psy->get_property(usb_psy, POWER_SUPPLY_PROP_PRESENT, &usb_connect);

	if (bat_percent.intval <= charging_below)
		enable_disable_charging(batt_psy, true);

	if (status.intval == POWER_SUPPLY_STATUS_CHARGING || usb_connect.intval) {
		if (bat_percent.intval <= charging_below) {
			enable_disable_charging(batt_psy, true);
		} else if (bat_percent.intval >= charging_limit) {
			if (charge_should_off) {
				enable_disable_charging(batt_psy, false);
				charge_should_off = false;
			} else {
				charge_should_off = true;
				// atleast 10 seconds
				ms_timer = 10000;
			}
		}
	}

reschedule:
	reschedule_worker(ms_timer);
}

static int start_charger_limiter(void)
{
	charge_limiter_wq = alloc_workqueue("charge_limiter_wq", WQ_HIGHPRI, 0);
	if (!charge_limiter_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&charger_limiter_work, charger_limiter_worker);
	reschedule_worker(1000);

	return 0;
}

static void stop_charger_limiter(void)
{
	struct power_supply *batt_psy = power_supply_get_by_name("battery");

	cancel_delayed_work_sync(&charger_limiter_work);
	destroy_workqueue(charge_limiter_wq);

	if (batt_psy)
		enable_disable_charging(batt_psy, true);
}

/******************************************************************/
/*                         SYSFS START                            */
/******************************************************************/
static ssize_t show_enabled(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", enabled);
}

static ssize_t store_enabled(struct kobject *kobj,
				struct attribute *attr, const char *buf,
				size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	enabled = input;

	if (enabled) {
		if (!cl_is_enabled) {
			start_charger_limiter();
			cl_is_enabled = true;
		}
	} else {
		if (cl_is_enabled) {
			stop_charger_limiter();
			cl_is_enabled = false;
		}
	}

	return count;
}

static struct global_attr enabled_attr =
	__ATTR(enabled, 0644,
	show_enabled, store_enabled);

static ssize_t show_charging_below(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", charging_below);
}

static ssize_t store_charging_below(struct kobject *kobj,
					struct attribute *attr, const char *buf,
					size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	charging_below = input;

	if (charging_below >= charging_limit)
		charging_below = charging_limit - 5;

	return count;
}

static struct global_attr charging_below_attr =
	__ATTR(charging_below, 0644,
	show_charging_below, store_charging_below);

static ssize_t show_charging_limit(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", charging_limit);
}

static ssize_t store_charging_limit(struct kobject *kobj,
					struct attribute *attr, const char *buf,
					size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	charging_limit = input;

	if (charging_limit <= charging_below)
		charging_limit = charging_below + 5;

	return count;
}

static struct global_attr charging_limit_attr =
	__ATTR(charging_limit, 0644,
	show_charging_limit, store_charging_limit);

static struct attribute *charger_limiter_attributes[] = {
	&enabled_attr.attr,
	&charging_below_attr.attr,
	&charging_limit_attr.attr,
	NULL
};

static struct attribute_group charger_limiter_attr_group = {
	.attrs = charger_limiter_attributes,
	.name = "parameters",
};

/******************************************************************/
/*                           SYSFS END                            */
/******************************************************************/

static int __init charger_limiter_init(void)
{
	int rc = 0;

	charger_limiter = kobject_create_and_add("charger_limiter", kernel_kobj);
	rc = sysfs_create_group(charger_limiter, &charger_limiter_attr_group);
	if (rc)
		pr_warn("charger_limiter: Failed to create sysfs group");

	return 0;
}
late_initcall(charger_limiter_init);
