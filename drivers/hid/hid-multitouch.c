/*
 *  HID driver for multitouch panels
 *
 *  Copyright (c) 2010-2012 Stephane Chatty <chatty@enac.fr>
 *  Copyright (c) 2010-2013 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 *  Copyright (c) 2010-2012 Ecole Nationale de l'Aviation Civile, France
 *  Copyright (c) 2012-2013 Red Hat, Inc
 *
 *  This code is partly based on hid-egalax.c:
 *
 *  Copyright (c) 2010 Stephane Chatty <chatty@enac.fr>
 *  Copyright (c) 2010 Henrik Rydberg <rydberg@euromail.se>
 *  Copyright (c) 2010 Canonical, Ltd.
 *
 *  This code is partly based on hid-3m-pct.c:
 *
 *  Copyright (c) 2009-2010 Stephane Chatty <chatty@enac.fr>
 *  Copyright (c) 2010      Henrik Rydberg <rydberg@euromail.se>
 *  Copyright (c) 2010      Canonical, Ltd.
 *
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

/*
 * This driver is regularly tested thanks to the tool hid-test[1].
 * This tool relies on hid-replay[2] and a database of hid devices[3].
 * Please run these regression tests before patching this module so that
 * your patch won't break existing known devices.
 *
 * [1] https://github.com/bentiss/hid-test
 * [2] https://github.com/bentiss/hid-replay
 * [3] https://github.com/bentiss/hid-devices
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input/mt.h>
#include <linux/string.h>
#include <linux/timer.h>


MODULE_AUTHOR("Stephane Chatty <chatty@enac.fr>");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_DESCRIPTION("HID multitouch panels");
MODULE_LICENSE("GPL");

#include "hid-ids.h"

/* quirks to control the device */
#define MT_QUIRK_NOT_SEEN_MEANS_UP	BIT(0)
#define MT_QUIRK_SLOT_IS_CONTACTID	BIT(1)
#define MT_QUIRK_CYPRESS		BIT(2)
#define MT_QUIRK_SLOT_IS_CONTACTNUMBER	BIT(3)
#define MT_QUIRK_ALWAYS_VALID		BIT(4)
#define MT_QUIRK_VALID_IS_INRANGE	BIT(5)
#define MT_QUIRK_VALID_IS_CONFIDENCE	BIT(6)
#define MT_QUIRK_CONFIDENCE		BIT(7)
#define MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE	BIT(8)
#define MT_QUIRK_NO_AREA		BIT(9)
#define MT_QUIRK_IGNORE_DUPLICATES	BIT(10)
#define MT_QUIRK_HOVERING		BIT(11)
#define MT_QUIRK_CONTACT_CNT_ACCURATE	BIT(12)
#define MT_QUIRK_FORCE_GET_FEATURE	BIT(13)
#define MT_QUIRK_FIX_CONST_CONTACT_ID	BIT(14)
#define MT_QUIRK_TOUCH_SIZE_SCALING	BIT(15)
#define MT_QUIRK_STICKY_FINGERS		BIT(16)
#define MT_QUIRK_ASUS_CUSTOM_UP		BIT(17)

#define MT_INPUTMODE_TOUCHSCREEN	0x02
#define MT_INPUTMODE_TOUCHPAD		0x03

#define MT_BUTTONTYPE_CLICKPAD		0

#define MT_IO_FLAGS_RUNNING		0
#define MT_IO_FLAGS_ACTIVE_SLOTS	1
#define MT_IO_FLAGS_PENDING_SLOTS	2

struct mt_slot {
	__s32 x, y, cx, cy, p, w, h;
	__s32 contactid;	/* the device ContactID assigned to this slot */
	bool touch_state;	/* is the touch valid? */
	bool inrange_state;	/* is the finger in proximity of the sensor? */
	bool confidence_state;  /* is the touch made by a finger? */
};

struct mt_class {
	__s32 name;	/* MT_CLS */
	__s32 quirks;
	__s32 sn_move;	/* Signal/noise ratio for move events */
	__s32 sn_width;	/* Signal/noise ratio for width events */
	__s32 sn_height;	/* Signal/noise ratio for height events */
	__s32 sn_pressure;	/* Signal/noise ratio for pressure events */
	__u8 maxcontacts;
	bool is_indirect;	/* true for touchpads */
	bool export_all_inputs;	/* do not ignore mouse, keyboards, etc... */
};

struct mt_fields {
	unsigned usages[HID_MAX_FIELDS];
	unsigned int length;
};

struct mt_device {
	struct mt_slot curdata;	/* placeholder of incoming data */
	struct mt_class mtclass;	/* our mt device class */
	struct timer_list release_timer;	/* to release sticky fingers */
	struct mt_fields *fields;	/* temporary placeholder for storing the
					   multitouch fields */
	unsigned long mt_io_flags;	/* mt flags (MT_IO_FLAGS_*) */
	int cc_index;	/* contact count field index in the report */
	int cc_value_index;	/* contact count value index in the field */
	unsigned last_slot_field;	/* the last field of a slot */
	unsigned mt_report_id;	/* the report ID of the multitouch device */
	unsigned long initial_quirks;	/* initial quirks state */
	__s16 inputmode;	/* InputMode HID feature, -1 if non-existent */
	__s16 inputmode_index;	/* InputMode HID feature index in the report */
	__s16 maxcontact_report_id;	/* Maximum Contact Number HID feature,
				   -1 if non-existent */
	__u8 inputmode_value;  /* InputMode HID feature value */
	__u8 num_received;	/* how many contacts we received */
	__u8 num_expected;	/* expected last contact index */
	__u8 maxcontacts;
	__u8 touches_by_report;	/* how many touches are present in one report:
				* 1 means we should use a serial protocol
				* > 1 means hybrid (multitouch) protocol */
	__u8 buttons_count;	/* number of physical buttons per touchpad */
	bool is_buttonpad;	/* is this device a button pad? */
	bool serial_maybe;	/* need to check for serial protocol */
	bool curvalid;		/* is the current contact valid? */
	unsigned mt_flags;	/* flags to pass to input-mt */
};

static void mt_post_parse_default_settings(struct mt_device *td);
static void mt_post_parse(struct mt_device *td);

/* classes of device behavior */
#define MT_CLS_DEFAULT				0x0001

#define MT_CLS_SERIAL				0x0002
#define MT_CLS_CONFIDENCE			0x0003
#define MT_CLS_CONFIDENCE_CONTACT_ID		0x0004
#define MT_CLS_CONFIDENCE_MINUS_ONE		0x0005
#define MT_CLS_DUAL_INRANGE_CONTACTID		0x0006
#define MT_CLS_DUAL_INRANGE_CONTACTNUMBER	0x0007
/* reserved					0x0008 */
#define MT_CLS_INRANGE_CONTACTNUMBER		0x0009
#define MT_CLS_NSMU				0x000a
/* reserved					0x0010 */
/* reserved					0x0011 */
#define MT_CLS_WIN_8				0x0012
#define MT_CLS_EXPORT_ALL_INPUTS		0x0013
#define MT_CLS_WIN_8_DUAL			0x0014

/* vendor specific classes */
#define MT_CLS_3M				0x0101
/* reserved					0x0102 */
#define MT_CLS_EGALAX				0x0103
#define MT_CLS_EGALAX_SERIAL			0x0104
#define MT_CLS_TOPSEED				0x0105
#define MT_CLS_PANASONIC			0x0106
#define MT_CLS_FLATFROG				0x0107
#define MT_CLS_GENERALTOUCH_TWOFINGERS		0x0108
#define MT_CLS_GENERALTOUCH_PWT_TENFINGERS	0x0109
#define MT_CLS_LG				0x010a
#define MT_CLS_ASUS				0x010b
#define MT_CLS_VTL				0x0110
#define MT_CLS_GOOGLE				0x0111

#define MT_DEFAULT_MAXCONTACT	10
#define MT_MAX_MAXCONTACT	250

#define MT_USB_DEVICE(v, p)	HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH, v, p)
#define MT_BT_DEVICE(v, p)	HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_MULTITOUCH, v, p)

/*
 * these device-dependent functions determine what slot corresponds
 * to a valid contact that was just read.
 */

static int cypress_compute_slot(struct mt_device *td)
{
	if (td->curdata.contactid != 0 || td->num_received == 0)
		return td->curdata.contactid;
	else
		return -1;
}

static struct mt_class mt_classes[] = {
	{ .name = MT_CLS_DEFAULT,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE },
	{ .name = MT_CLS_NSMU,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP },
	{ .name = MT_CLS_SERIAL,
		.quirks = MT_QUIRK_ALWAYS_VALID},
	{ .name = MT_CLS_CONFIDENCE,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE },
	{ .name = MT_CLS_CONFIDENCE_CONTACT_ID,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID },
	{ .name = MT_CLS_CONFIDENCE_MINUS_ONE,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE },
	{ .name = MT_CLS_DUAL_INRANGE_CONTACTID,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTID,
		.maxcontacts = 2 },
	{ .name = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTNUMBER,
		.maxcontacts = 2 },
	{ .name = MT_CLS_INRANGE_CONTACTNUMBER,
		.quirks = MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTNUMBER },
	{ .name = MT_CLS_WIN_8,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_STICKY_FINGERS },
	{ .name = MT_CLS_EXPORT_ALL_INPUTS,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE,
		.export_all_inputs = true },
	{ .name = MT_CLS_WIN_8_DUAL,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE,
		.export_all_inputs = true },

	/*
	 * vendor specific classes
	 */
	{ .name = MT_CLS_3M,
		.quirks = MT_QUIRK_VALID_IS_CONFIDENCE |
			MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_TOUCH_SIZE_SCALING,
		.sn_move = 2048,
		.sn_width = 128,
		.sn_height = 128,
		.maxcontacts = 60,
	},
	{ .name = MT_CLS_EGALAX,
		.quirks =  MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_VALID_IS_INRANGE,
		.sn_move = 4096,
		.sn_pressure = 32,
	},
	{ .name = MT_CLS_EGALAX_SERIAL,
		.quirks =  MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_ALWAYS_VALID,
		.sn_move = 4096,
		.sn_pressure = 32,
	},
	{ .name = MT_CLS_TOPSEED,
		.quirks = MT_QUIRK_ALWAYS_VALID,
		.is_indirect = true,
		.maxcontacts = 2,
	},
	{ .name = MT_CLS_PANASONIC,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP,
		.maxcontacts = 4 },
	{ .name	= MT_CLS_GENERALTOUCH_TWOFINGERS,
		.quirks	= MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_VALID_IS_INRANGE |
			MT_QUIRK_SLOT_IS_CONTACTID,
		.maxcontacts = 2
	},
	{ .name	= MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		.quirks	= MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_SLOT_IS_CONTACTID
	},

	{ .name = MT_CLS_FLATFROG,
		.quirks = MT_QUIRK_NOT_SEEN_MEANS_UP |
			MT_QUIRK_NO_AREA,
		.sn_move = 2048,
		.maxcontacts = 40,
	},
	{ .name = MT_CLS_LG,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_FIX_CONST_CONTACT_ID |
			MT_QUIRK_IGNORE_DUPLICATES |
			MT_QUIRK_HOVERING |
			MT_QUIRK_CONTACT_CNT_ACCURATE },
	{ .name = MT_CLS_ASUS,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_ASUS_CUSTOM_UP },
	{ .name = MT_CLS_VTL,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_FORCE_GET_FEATURE,
	},
	{ .name = MT_CLS_GOOGLE,
		.quirks = MT_QUIRK_ALWAYS_VALID |
			MT_QUIRK_CONTACT_CNT_ACCURATE |
			MT_QUIRK_SLOT_IS_CONTACTID |
			MT_QUIRK_HOVERING
	},
	{ }
};

static ssize_t mt_show_quirks(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct mt_device *td = hid_get_drvdata(hdev);

	return sprintf(buf, "%u\n", td->mtclass.quirks);
}

static ssize_t mt_set_quirks(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct mt_device *td = hid_get_drvdata(hdev);

	unsigned long val;

	if (kstrtoul(buf, 0, &val))
		return -EINVAL;

	td->mtclass.quirks = val;

	if (td->cc_index < 0)
		td->mtclass.quirks &= ~MT_QUIRK_CONTACT_CNT_ACCURATE;

	return count;
}

static DEVICE_ATTR(quirks, S_IWUSR | S_IRUGO, mt_show_quirks, mt_set_quirks);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_quirks.attr,
	NULL
};

static const struct attribute_group mt_attribute_group = {
	.attrs = sysfs_attrs
};

static void mt_get_feature(struct hid_device *hdev, struct hid_report *report)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	int ret;
	u32 size = hid_report_len(report);
	u8 *buf;

	/*
	 * Do not fetch the feature report if the device has been explicitly
	 * marked as non-capable.
	 */
	if (td->initial_quirks & HID_QUIRK_NO_INIT_REPORTS)
		return;

	buf = hid_alloc_report_buf(report, GFP_KERNEL);
	if (!buf)
		return;

	ret = hid_hw_raw_request(hdev, report->id, buf, size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_warn(&hdev->dev, "failed to fetch feature %d\n",
			 report->id);
	} else {
		ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf,
					   size, 0);
		if (ret)
			dev_warn(&hdev->dev, "failed to report feature\n");
	}

	kfree(buf);
}

static void mt_feature_mapping(struct hid_device *hdev,
		struct hid_field *field, struct hid_usage *usage)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	switch (usage->hid) {
	case HID_DG_INPUTMODE:
		/* Ignore if value index is out of bounds. */
		if (usage->usage_index >= field->report_count) {
			dev_err(&hdev->dev, "HID_DG_INPUTMODE out of range\n");
			break;
		}

		if (td->inputmode < 0) {
			td->inputmode = field->report->id;
			td->inputmode_index = usage->usage_index;
		} else {
			/*
			 * Some elan panels wrongly declare 2 input mode
			 * features, and silently ignore when we set the
			 * value in the second field. Skip the second feature
			 * and hope for the best.
			 */
			dev_info(&hdev->dev,
				 "Ignoring the extra HID_DG_INPUTMODE\n");
		}

		break;
	case HID_DG_CONTACTMAX:
		mt_get_feature(hdev, field->report);

		td->maxcontact_report_id = field->report->id;
		td->maxcontacts = field->value[0];
		if (!td->maxcontacts &&
		    field->logical_maximum <= MT_MAX_MAXCONTACT)
			td->maxcontacts = field->logical_maximum;
		if (td->mtclass.maxcontacts)
			/* check if the maxcontacts is given by the class */
			td->maxcontacts = td->mtclass.maxcontacts;

		break;
	case HID_DG_BUTTONTYPE:
		if (usage->usage_index >= field->report_count) {
			dev_err(&hdev->dev, "HID_DG_BUTTONTYPE out of range\n");
			break;
		}

		mt_get_feature(hdev, field->report);
		if (field->value[usage->usage_index] == MT_BUTTONTYPE_CLICKPAD)
			td->is_buttonpad = true;

		break;
	case 0xff0000c5:
		/* Retrieve the Win8 blob once to enable some devices */
		if (usage->usage_index == 0)
			mt_get_feature(hdev, field->report);
		break;
	}
}

static void set_abs(struct input_dev *input, unsigned int code,
		struct hid_field *field, int snratio)
{
	int fmin = field->logical_minimum;
	int fmax = field->logical_maximum;
	int fuzz = snratio ? (fmax - fmin) / snratio : 0;
	input_set_abs_params(input, code, fmin, fmax, fuzz, 0);
	input_abs_set_res(input, code, hidinput_calc_abs_res(field, code));
}

static void mt_store_field(struct hid_usage *usage, struct mt_device *td,
		struct hid_input *hi)
{
	struct mt_fields *f = td->fields;

	if (f->length >= HID_MAX_FIELDS)
		return;

	f->usages[f->length++] = usage->hid;
}

static int mt_touch_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_class *cls = &td->mtclass;
	int code;
	struct hid_usage *prev_usage = NULL;

	if (field->application == HID_DG_TOUCHSCREEN)
		td->mt_flags |= INPUT_MT_DIRECT;

	/*
	 * Model touchscreens providing buttons as touchpads.
	 */
	if (field->application == HID_DG_TOUCHPAD ||
	    (usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
		td->mt_flags |= INPUT_MT_POINTER;
		td->inputmode_value = MT_INPUTMODE_TOUCHPAD;
	}

	/* count the buttons on touchpads */
	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON)
		td->buttons_count++;

	if (usage->usage_index)
		prev_usage = &field->usage[usage->usage_index - 1];

	switch (usage->hid & HID_USAGE_PAGE) {

	case HID_UP_GENDESK:
		switch (usage->hid) {
		case HID_GD_X:
			if (prev_usage && (prev_usage->hid == usage->hid)) {
				hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_TOOL_X);
				set_abs(hi->input, ABS_MT_TOOL_X, field,
					cls->sn_move);
			} else {
				hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_POSITION_X);
				set_abs(hi->input, ABS_MT_POSITION_X, field,
					cls->sn_move);
			}

			mt_store_field(usage, td, hi);
			return 1;
		case HID_GD_Y:
			if (prev_usage && (prev_usage->hid == usage->hid)) {
				hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_TOOL_Y);
				set_abs(hi->input, ABS_MT_TOOL_Y, field,
					cls->sn_move);
			} else {
				hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_POSITION_Y);
				set_abs(hi->input, ABS_MT_POSITION_Y, field,
					cls->sn_move);
			}

			mt_store_field(usage, td, hi);
			return 1;
		}
		return 0;

	case HID_UP_DIGITIZER:
		switch (usage->hid) {
		case HID_DG_INRANGE:
			if (cls->quirks & MT_QUIRK_HOVERING) {
				hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_DISTANCE);
				input_set_abs_params(hi->input,
					ABS_MT_DISTANCE, 0, 1, 0, 0);
			}
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_CONFIDENCE:
			if ((cls->name == MT_CLS_WIN_8 ||
				cls->name == MT_CLS_WIN_8_DUAL) &&
				field->application == HID_DG_TOUCHPAD)
				cls->quirks |= MT_QUIRK_CONFIDENCE;
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_TIPSWITCH:
			hid_map_usage(hi, usage, bit, max, EV_KEY, BTN_TOUCH);
			input_set_capability(hi->input, EV_KEY, BTN_TOUCH);
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_CONTACTID:
			mt_store_field(usage, td, hi);
			td->touches_by_report++;
			td->mt_report_id = field->report->id;
			return 1;
		case HID_DG_WIDTH:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_TOUCH_MAJOR);
			if (!(cls->quirks & MT_QUIRK_NO_AREA))
				set_abs(hi->input, ABS_MT_TOUCH_MAJOR, field,
					cls->sn_width);
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_HEIGHT:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_TOUCH_MINOR);
			if (!(cls->quirks & MT_QUIRK_NO_AREA)) {
				set_abs(hi->input, ABS_MT_TOUCH_MINOR, field,
					cls->sn_height);
				input_set_abs_params(hi->input,
					ABS_MT_ORIENTATION, 0, 1, 0, 0);
			}
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_TIPPRESSURE:
			hid_map_usage(hi, usage, bit, max,
					EV_ABS, ABS_MT_PRESSURE);
			set_abs(hi->input, ABS_MT_PRESSURE, field,
				cls->sn_pressure);
			mt_store_field(usage, td, hi);
			return 1;
		case HID_DG_CONTACTCOUNT:
			/* Ignore if indexes are out of bounds. */
			if (field->index >= field->report->maxfield ||
			    usage->usage_index >= field->report_count)
				return 1;
			td->cc_index = field->index;
			td->cc_value_index = usage->usage_index;
			return 1;
		case HID_DG_CONTACTMAX:
			/* we don't set td->last_slot_field as contactcount and
			 * contact max are global to the report */
			return -1;
		case HID_DG_TOUCH:
			/* Legacy devices use TIPSWITCH and not TOUCH.
			 * Let's just ignore this field. */
			return -1;
		}
		/* let hid-input decide for the others */
		return 0;

	case HID_UP_BUTTON:
		code = BTN_MOUSE + ((usage->hid - 1) & HID_USAGE);
		/*
		 * MS PTP spec says that external buttons left and right have
		 * usages 2 and 3.
		 */
		if ((cls->name == MT_CLS_WIN_8 ||
			cls->name == MT_CLS_WIN_8_DUAL) &&
		    field->application == HID_DG_TOUCHPAD &&
		    (usage->hid & HID_USAGE) > 1)
			code--;
		hid_map_usage(hi, usage, bit, max, EV_KEY, code);
		if (!*bit)
			return -1;
		input_set_capability(hi->input, EV_KEY, code);
		return 1;

	case 0xff000000:
		/* we do not want to map these: no input-oriented meaning */
		return -1;
	}

	return 0;
}

static int mt_compute_slot(struct mt_device *td, struct input_dev *input)
{
	__s32 quirks = td->mtclass.quirks;

	if (quirks & MT_QUIRK_SLOT_IS_CONTACTID)
		return td->curdata.contactid;

	if (quirks & MT_QUIRK_CYPRESS)
		return cypress_compute_slot(td);

	if (quirks & MT_QUIRK_SLOT_IS_CONTACTNUMBER)
		return td->num_received;

	if (quirks & MT_QUIRK_SLOT_IS_CONTACTID_MINUS_ONE)
		return td->curdata.contactid - 1;

	return input_mt_get_slot_by_key(input, td->curdata.contactid);
}

/*
 * this function is called when a whole contact has been processed,
 * so that it can assign it to a slot and store the data there
 */
static void mt_complete_slot(struct mt_device *td, struct input_dev *input)
{
	if ((td->mtclass.quirks & MT_QUIRK_CONTACT_CNT_ACCURATE) &&
	    td->num_received >= td->num_expected)
		return;

	if (td->curvalid || (td->mtclass.quirks & MT_QUIRK_ALWAYS_VALID)) {
		int active;
		int slotnum = mt_compute_slot(td, input);
		struct mt_slot *s = &td->curdata;
		struct input_mt *mt = input->mt;

		if (slotnum < 0 || slotnum >= td->maxcontacts)
			return;

		if ((td->mtclass.quirks & MT_QUIRK_IGNORE_DUPLICATES) && mt) {
			struct input_mt_slot *slot = &mt->slots[slotnum];
			if (input_mt_is_active(slot) &&
			    input_mt_is_used(mt, slot))
				return;
		}

		if (!(td->mtclass.quirks & MT_QUIRK_CONFIDENCE))
			s->confidence_state = 1;
		active = (s->touch_state || s->inrange_state) &&
							s->confidence_state;

		input_mt_slot(input, slotnum);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, active);
		if (active) {
			/* this finger is in proximity of the sensor */
			int wide = (s->w > s->h);
			int major = max(s->w, s->h);
			int minor = min(s->w, s->h);

			/*
			 * divided by two to match visual scale of touch
			 * for devices with this quirk
			 */
			if (td->mtclass.quirks & MT_QUIRK_TOUCH_SIZE_SCALING) {
				major = major >> 1;
				minor = minor >> 1;
			}

			input_event(input, EV_ABS, ABS_MT_POSITION_X, s->x);
			input_event(input, EV_ABS, ABS_MT_POSITION_Y, s->y);
			input_event(input, EV_ABS, ABS_MT_TOOL_X, s->cx);
			input_event(input, EV_ABS, ABS_MT_TOOL_Y, s->cy);
			input_event(input, EV_ABS, ABS_MT_DISTANCE,
				!s->touch_state);
			input_event(input, EV_ABS, ABS_MT_ORIENTATION, wide);
			input_event(input, EV_ABS, ABS_MT_PRESSURE, s->p);
			input_event(input, EV_ABS, ABS_MT_TOUCH_MAJOR, major);
			input_event(input, EV_ABS, ABS_MT_TOUCH_MINOR, minor);

			set_bit(MT_IO_FLAGS_ACTIVE_SLOTS, &td->mt_io_flags);
		}
	}

	td->num_received++;
}

/*
 * this function is called when a whole packet has been received and processed,
 * so that it can decide what to send to the input layer.
 */
static void mt_sync_frame(struct mt_device *td, struct input_dev *input)
{
	input_mt_sync_frame(input);
	input_sync(input);
	td->num_received = 0;
	if (test_bit(MT_IO_FLAGS_ACTIVE_SLOTS, &td->mt_io_flags))
		set_bit(MT_IO_FLAGS_PENDING_SLOTS, &td->mt_io_flags);
	else
		clear_bit(MT_IO_FLAGS_PENDING_SLOTS, &td->mt_io_flags);
	clear_bit(MT_IO_FLAGS_ACTIVE_SLOTS, &td->mt_io_flags);
}

static int mt_touch_event(struct hid_device *hid, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	/* we will handle the hidinput part later, now remains hiddev */
	if (hid->claimed & HID_CLAIMED_HIDDEV && hid->hiddev_hid_event)
		hid->hiddev_hid_event(hid, field, usage, value);

	return 1;
}

static void mt_process_mt_event(struct hid_device *hid, struct hid_field *field,
				struct hid_usage *usage, __s32 value,
				bool first_packet)
{
	struct mt_device *td = hid_get_drvdata(hid);
	__s32 cls = td->mtclass.name;
	__s32 quirks = td->mtclass.quirks;
	struct input_dev *input = field->hidinput->input;

	if (hid->claimed & HID_CLAIMED_INPUT) {
		switch (usage->hid) {
		case HID_DG_INRANGE:
			if (quirks & MT_QUIRK_VALID_IS_INRANGE)
				td->curvalid = value;
			if (quirks & MT_QUIRK_HOVERING)
				td->curdata.inrange_state = value;
			break;
		case HID_DG_TIPSWITCH:
			if (quirks & MT_QUIRK_NOT_SEEN_MEANS_UP)
				td->curvalid = value;
			td->curdata.touch_state = value;
			break;
		case HID_DG_CONFIDENCE:
			if (quirks & MT_QUIRK_CONFIDENCE)
				td->curdata.confidence_state = value;
			if (quirks & MT_QUIRK_VALID_IS_CONFIDENCE)
				td->curvalid = value;
			break;
		case HID_DG_CONTACTID:
			td->curdata.contactid = value;
			break;
		case HID_DG_TIPPRESSURE:
			td->curdata.p = value;
			break;
		case HID_GD_X:
			if (usage->code == ABS_MT_TOOL_X)
				td->curdata.cx = value;
			else
				td->curdata.x = value;
			break;
		case HID_GD_Y:
			if (usage->code == ABS_MT_TOOL_Y)
				td->curdata.cy = value;
			else
				td->curdata.y = value;
			break;
		case HID_DG_WIDTH:
			td->curdata.w = value;
			break;
		case HID_DG_HEIGHT:
			td->curdata.h = value;
			break;
		case HID_DG_CONTACTCOUNT:
			break;
		case HID_DG_TOUCH:
			/* do nothing */
			break;

		default:
			/*
			 * For Win8 PTP touchpads we should only look at
			 * non finger/touch events in the first_packet of
			 * a (possible) multi-packet frame.
			 */
			if ((cls == MT_CLS_WIN_8 || cls == MT_CLS_WIN_8_DUAL) &&
			    !first_packet)
				return;

			if (usage->type)
				input_event(input, usage->type, usage->code,
						value);
			return;
		}

		if (usage->usage_index + 1 == field->report_count) {
			/* we only take into account the last report. */
			if (usage->hid == td->last_slot_field)
				mt_complete_slot(td, field->hidinput->input);
		}

	}
}

static void mt_touch_report(struct hid_device *hid, struct hid_report *report)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct hid_field *field;
	bool first_packet;
	unsigned count;
	int r, n;

	/* sticky fingers release in progress, abort */
	if (test_and_set_bit_lock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags))
		return;

	/*
	 * Includes multi-packet support where subsequent
	 * packets are sent with zero contactcount.
	 */
	if (td->cc_index >= 0) {
		struct hid_field *field = report->field[td->cc_index];
		int value = field->value[td->cc_value_index];
		if (value)
			td->num_expected = value;
	}

	first_packet = td->num_received == 0;
	for (r = 0; r < report->maxfield; r++) {
		field = report->field[r];
		count = field->report_count;

		if (!(HID_MAIN_ITEM_VARIABLE & field->flags))
			continue;

		for (n = 0; n < count; n++)
			mt_process_mt_event(hid, field, &field->usage[n],
					    field->value[n], first_packet);
	}

	if (td->num_received >= td->num_expected)
		mt_sync_frame(td, report->field[0]->hidinput->input);

	/*
	 * Windows 8 specs says 2 things:
	 * - once a contact has been reported, it has to be reported in each
	 *   subsequent report
	 * - the report rate when fingers are present has to be at least
	 *   the refresh rate of the screen, 60 or 120 Hz
	 *
	 * I interprete this that the specification forces a report rate of
	 * at least 60 Hz for a touchscreen to be certified.
	 * Which means that if we do not get a report whithin 16 ms, either
	 * something wrong happens, either the touchscreen forgets to send
	 * a release. Taking a reasonable margin allows to remove issues
	 * with USB communication or the load of the machine.
	 *
	 * Given that Win 8 devices are forced to send a release, this will
	 * only affect laggish machines and the ones that have a firmware
	 * defect.
	 */
	if (td->mtclass.quirks & MT_QUIRK_STICKY_FINGERS) {
		if (test_bit(MT_IO_FLAGS_PENDING_SLOTS, &td->mt_io_flags))
			mod_timer(&td->release_timer,
				  jiffies + msecs_to_jiffies(100));
		else
			del_timer(&td->release_timer);
	}

	clear_bit_unlock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags);
}

static int mt_touch_input_configured(struct hid_device *hdev,
					struct hid_input *hi)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct mt_class *cls = &td->mtclass;
	struct input_dev *input = hi->input;
	int ret;

	if (!td->maxcontacts)
		td->maxcontacts = MT_DEFAULT_MAXCONTACT;

	mt_post_parse(td);
	if (td->serial_maybe)
		mt_post_parse_default_settings(td);

	if (cls->is_indirect)
		td->mt_flags |= INPUT_MT_POINTER;

	if (cls->quirks & MT_QUIRK_NOT_SEEN_MEANS_UP)
		td->mt_flags |= INPUT_MT_DROP_UNUSED;

	/* check for clickpads */
	if ((td->mt_flags & INPUT_MT_POINTER) && (td->buttons_count == 1))
		td->is_buttonpad = true;

	if (td->is_buttonpad)
		__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);

	ret = input_mt_init_slots(input, td->maxcontacts, td->mt_flags);
	if (ret)
		return ret;

	td->mt_flags = 0;
	return 0;
}

#define mt_map_key_clear(c)	hid_map_usage_clear(hi, usage, bit, \
						    max, EV_KEY, (c))
static int mt_input_mapping(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	/*
	 * If mtclass.export_all_inputs is not set, only map fields from
	 * TouchScreen or TouchPad collections. We need to ignore fields
	 * that belong to other collections such as Mouse that might have
	 * the same GenericDesktop usages.
	 */
	if (!td->mtclass.export_all_inputs &&
	    field->application != HID_DG_TOUCHSCREEN &&
	    field->application != HID_DG_PEN &&
	    field->application != HID_DG_TOUCHPAD &&
	    field->application != HID_GD_KEYBOARD &&
	    field->application != HID_GD_SYSTEM_CONTROL &&
	    field->application != HID_CP_CONSUMER_CONTROL &&
	    field->application != HID_GD_WIRELESS_RADIO_CTLS &&
	    !(field->application == HID_VD_ASUS_CUSTOM_MEDIA_KEYS &&
	      td->mtclass.quirks & MT_QUIRK_ASUS_CUSTOM_UP))
		return -1;

	/*
	 * Some Asus keyboard+touchpad devices have the hotkeys defined in the
	 * touchpad report descriptor. We need to treat these as an array to
	 * map usages to input keys.
	 */
	if (field->application == HID_VD_ASUS_CUSTOM_MEDIA_KEYS &&
	    td->mtclass.quirks & MT_QUIRK_ASUS_CUSTOM_UP &&
	    (usage->hid & HID_USAGE_PAGE) == HID_UP_CUSTOM) {
		set_bit(EV_REP, hi->input->evbit);
		if (field->flags & HID_MAIN_ITEM_VARIABLE)
			field->flags &= ~HID_MAIN_ITEM_VARIABLE;
		switch (usage->hid & HID_USAGE) {
		case 0x10: mt_map_key_clear(KEY_BRIGHTNESSDOWN);	break;
		case 0x20: mt_map_key_clear(KEY_BRIGHTNESSUP);		break;
		case 0x35: mt_map_key_clear(KEY_DISPLAY_OFF);		break;
		case 0x6b: mt_map_key_clear(KEY_F21);			break;
		case 0x6c: mt_map_key_clear(KEY_SLEEP);			break;
		default:
			return -1;
		}
		return 1;
	}

	/*
	 * some egalax touchscreens have "application == HID_DG_TOUCHSCREEN"
	 * for the stylus.
	 * The check for mt_report_id ensures we don't process
	 * HID_DG_CONTACTCOUNT from the pen report as it is outside the physical
	 * collection, but within the report ID.
	 */
	if (field->physical == HID_DG_STYLUS)
		return 0;
	else if ((field->physical == 0) &&
		 (field->report->id != td->mt_report_id) &&
		 (td->mt_report_id != -1))
		return 0;

	if (field->application == HID_DG_TOUCHSCREEN ||
	    field->application == HID_DG_TOUCHPAD)
		return mt_touch_input_mapping(hdev, hi, field, usage, bit, max);

	/* let hid-core decide for the others */
	return 0;
}

static int mt_input_mapped(struct hid_device *hdev, struct hid_input *hi,
		struct hid_field *field, struct hid_usage *usage,
		unsigned long **bit, int *max)
{
	/*
	 * some egalax touchscreens have "application == HID_DG_TOUCHSCREEN"
	 * for the stylus.
	 */
	if (field->physical == HID_DG_STYLUS)
		return 0;

	if (field->application == HID_DG_TOUCHSCREEN ||
	    field->application == HID_DG_TOUCHPAD) {
		/* We own these mappings, tell hid-input to ignore them */
		return -1;
	}

	/* let hid-core decide for the others */
	return 0;
}

static int mt_event(struct hid_device *hid, struct hid_field *field,
				struct hid_usage *usage, __s32 value)
{
	struct mt_device *td = hid_get_drvdata(hid);

	if (field->report->id == td->mt_report_id)
		return mt_touch_event(hid, field, usage, value);

	return 0;
}

static void mt_report(struct hid_device *hid, struct hid_report *report)
{
	struct mt_device *td = hid_get_drvdata(hid);
	struct hid_field *field = report->field[0];

	if (!(hid->claimed & HID_CLAIMED_INPUT))
		return;

	if (report->id == td->mt_report_id)
		return mt_touch_report(hid, report);

	if (field && field->hidinput && field->hidinput->input)
		input_sync(field->hidinput->input);
}

static void mt_set_input_mode(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;
	struct mt_class *cls = &td->mtclass;
	char *buf;
	u32 report_len;

	if (td->inputmode < 0)
		return;

	re = &(hdev->report_enum[HID_FEATURE_REPORT]);
	r = re->report_id_hash[td->inputmode];
	if (r) {
		if (cls->quirks & MT_QUIRK_FORCE_GET_FEATURE) {
			report_len = hid_report_len(r);
			buf = hid_alloc_report_buf(r, GFP_KERNEL);
			if (!buf) {
				hid_err(hdev, "failed to allocate buffer for report\n");
				return;
			}
			hid_hw_raw_request(hdev, r->id, buf, report_len,
					   HID_FEATURE_REPORT,
					   HID_REQ_GET_REPORT);
			kfree(buf);
		}
		r->field[0]->value[td->inputmode_index] = td->inputmode_value;
		hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
	}
}

static void mt_set_maxcontacts(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	struct hid_report *r;
	struct hid_report_enum *re;
	int fieldmax, max;

	if (td->maxcontact_report_id < 0)
		return;

	if (!td->mtclass.maxcontacts)
		return;

	re = &hdev->report_enum[HID_FEATURE_REPORT];
	r = re->report_id_hash[td->maxcontact_report_id];
	if (r) {
		max = td->mtclass.maxcontacts;
		fieldmax = r->field[0]->logical_maximum;
		max = min(fieldmax, max);
		if (r->field[0]->value[0] != max) {
			r->field[0]->value[0] = max;
			hid_hw_request(hdev, r, HID_REQ_SET_REPORT);
		}
	}
}

static void mt_post_parse_default_settings(struct mt_device *td)
{
	__s32 quirks = td->mtclass.quirks;

	/* unknown serial device needs special quirks */
	if (td->touches_by_report == 1) {
		quirks |= MT_QUIRK_ALWAYS_VALID;
		quirks &= ~MT_QUIRK_NOT_SEEN_MEANS_UP;
		quirks &= ~MT_QUIRK_VALID_IS_INRANGE;
		quirks &= ~MT_QUIRK_VALID_IS_CONFIDENCE;
		quirks &= ~MT_QUIRK_CONTACT_CNT_ACCURATE;
	}

	td->mtclass.quirks = quirks;
}

static void mt_post_parse(struct mt_device *td)
{
	struct mt_fields *f = td->fields;
	struct mt_class *cls = &td->mtclass;

	if (td->touches_by_report > 0) {
		int field_count_per_touch = f->length / td->touches_by_report;
		td->last_slot_field = f->usages[field_count_per_touch - 1];
	}

	if (td->cc_index < 0)
		cls->quirks &= ~MT_QUIRK_CONTACT_CNT_ACCURATE;
}

static int mt_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct mt_device *td = hid_get_drvdata(hdev);
	const char *suffix = NULL;
	struct hid_field *field = hi->report->field[0];
	int ret;

	if (hi->report->id == td->mt_report_id) {
		ret = mt_touch_input_configured(hdev, hi);
		if (ret)
			return ret;
	}

	/*
	 * some egalax touchscreens have "application == HID_DG_TOUCHSCREEN"
	 * for the stylus. Check this first, and then rely on the application
	 * field.
	 */
	if (hi->report->field[0]->physical == HID_DG_STYLUS) {
		suffix = "Pen";
		/* force BTN_STYLUS to allow tablet matching in udev */
		__set_bit(BTN_STYLUS, hi->input->keybit);
	} else {
		switch (field->application) {
		case HID_GD_KEYBOARD:
			suffix = "Keyboard";
			break;
		case HID_GD_KEYPAD:
			suffix = "Keypad";
			break;
		case HID_GD_MOUSE:
			suffix = "Mouse";
			break;
		case HID_DG_STYLUS:
			suffix = "Pen";
			/* force BTN_STYLUS to allow tablet matching in udev */
			__set_bit(BTN_STYLUS, hi->input->keybit);
			break;
		case HID_DG_TOUCHSCREEN:
			/* we do not set suffix = "Touchscreen" */
			break;
		case HID_DG_TOUCHPAD:
			suffix = "Touchpad";
			break;
		case HID_GD_SYSTEM_CONTROL:
			suffix = "System Control";
			break;
		case HID_CP_CONSUMER_CONTROL:
			suffix = "Consumer Control";
			break;
		case HID_GD_WIRELESS_RADIO_CTLS:
			suffix = "Wireless Radio Control";
			break;
		case HID_VD_ASUS_CUSTOM_MEDIA_KEYS:
			suffix = "Custom Media Keys";
			break;
		default:
			suffix = "UNKNOWN";
			break;
		}
	}

	if (suffix) {
		hi->input->name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
						 "%s %s", hdev->name, suffix);
		if (!hi->input->name)
			return -ENOMEM;
	}

	return 0;
}

static void mt_fix_const_field(struct hid_field *field, unsigned int usage)
{
	if (field->usage[0].hid != usage ||
	    !(field->flags & HID_MAIN_ITEM_CONSTANT))
		return;

	field->flags &= ~HID_MAIN_ITEM_CONSTANT;
	field->flags |= HID_MAIN_ITEM_VARIABLE;
}

static void mt_fix_const_fields(struct hid_device *hdev, unsigned int usage)
{
	struct hid_report *report;
	int i;

	list_for_each_entry(report,
			    &hdev->report_enum[HID_INPUT_REPORT].report_list,
			    list) {

		if (!report->maxfield)
			continue;

		for (i = 0; i < report->maxfield; i++)
			if (report->field[i]->maxusage >= 1)
				mt_fix_const_field(report->field[i], usage);
	}
}

static void mt_release_contacts(struct hid_device *hid)
{
	struct hid_input *hidinput;
	struct mt_device *td = hid_get_drvdata(hid);

	list_for_each_entry(hidinput, &hid->inputs, list) {
		struct input_dev *input_dev = hidinput->input;
		struct input_mt *mt = input_dev->mt;
		int i;

		if (mt) {
			for (i = 0; i < mt->num_slots; i++) {
				input_mt_slot(input_dev, i);
				input_mt_report_slot_state(input_dev,
							   MT_TOOL_FINGER,
							   false);
			}
			input_mt_sync_frame(input_dev);
			input_sync(input_dev);
		}
	}

	td->num_received = 0;
}

static void mt_expired_timeout(unsigned long arg)
{
	struct hid_device *hdev = (void *)arg;
	struct mt_device *td = hid_get_drvdata(hdev);

	/*
	 * An input report came in just before we release the sticky fingers,
	 * it will take care of the sticky fingers.
	 */
	if (test_and_set_bit_lock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags))
		return;
	if (test_bit(MT_IO_FLAGS_PENDING_SLOTS, &td->mt_io_flags))
		mt_release_contacts(hdev);
	clear_bit_unlock(MT_IO_FLAGS_RUNNING, &td->mt_io_flags);
}

static int mt_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
	struct mt_device *td;
	struct mt_class *mtclass = mt_classes; /* MT_CLS_DEFAULT */

	for (i = 0; mt_classes[i].name ; i++) {
		if (id->driver_data == mt_classes[i].name) {
			mtclass = &(mt_classes[i]);
			break;
		}
	}

	td = devm_kzalloc(&hdev->dev, sizeof(struct mt_device), GFP_KERNEL);
	if (!td) {
		dev_err(&hdev->dev, "cannot allocate multitouch data\n");
		return -ENOMEM;
	}
	td->mtclass = *mtclass;
	td->inputmode = -1;
	td->maxcontact_report_id = -1;
	td->inputmode_value = MT_INPUTMODE_TOUCHSCREEN;
	td->cc_index = -1;
	td->mt_report_id = -1;
	hid_set_drvdata(hdev, td);

	td->fields = devm_kzalloc(&hdev->dev, sizeof(struct mt_fields),
				  GFP_KERNEL);
	if (!td->fields) {
		dev_err(&hdev->dev, "cannot allocate multitouch fields data\n");
		return -ENOMEM;
	}

	if (id->vendor == HID_ANY_ID && id->product == HID_ANY_ID)
		td->serial_maybe = true;

	/*
	 * Store the initial quirk state
	 */
	td->initial_quirks = hdev->quirks;

	/* This allows the driver to correctly support devices
	 * that emit events over several HID messages.
	 */
	hdev->quirks |= HID_QUIRK_NO_INPUT_SYNC;

	/*
	 * This allows the driver to handle different input sensors
	 * that emits events through different reports on the same HID
	 * device.
	 */
	hdev->quirks |= HID_QUIRK_MULTI_INPUT;
	hdev->quirks |= HID_QUIRK_NO_EMPTY_INPUT;

	/*
	 * Some multitouch screens do not like to be polled for input
	 * reports. Fortunately, the Win8 spec says that all touches
	 * should be sent during each report, making the initialization
	 * of input reports unnecessary. For Win7 devices, well, let's hope
	 * they will still be happy (this is only be a problem if a touch
	 * was already there while probing the device).
	 *
	 * In addition some touchpads do not behave well if we read
	 * all feature reports from them. Instead we prevent
	 * initial report fetching and then selectively fetch each
	 * report we are interested in.
	 */
	hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS;

	setup_timer(&td->release_timer, mt_expired_timeout, (long)hdev);

	ret = hid_parse(hdev);
	if (ret != 0)
		return ret;

	if (mtclass->quirks & MT_QUIRK_FIX_CONST_CONTACT_ID)
		mt_fix_const_fields(hdev, HID_DG_CONTACTID);

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	ret = sysfs_create_group(&hdev->dev.kobj, &mt_attribute_group);
	if (ret)
		dev_warn(&hdev->dev, "Cannot allocate sysfs group for %s\n",
				hdev->name);

	mt_set_maxcontacts(hdev);
	mt_set_input_mode(hdev);

	/* release .fields memory as it is not used anymore */
	devm_kfree(&hdev->dev, td->fields);
	td->fields = NULL;

	return 0;
}

#ifdef CONFIG_PM
static int mt_reset_resume(struct hid_device *hdev)
{
	mt_release_contacts(hdev);
	mt_set_maxcontacts(hdev);
	mt_set_input_mode(hdev);
	return 0;
}

static int mt_resume(struct hid_device *hdev)
{
	/* Some Elan legacy devices require SET_IDLE to be set on resume.
	 * It should be safe to send it to other devices too.
	 * Tested on 3M, Stantum, Cypress, Zytronic, eGalax, and Elan panels. */

	hid_hw_idle(hdev, 0, 0, HID_REQ_SET_IDLE);

	return 0;
}
#endif

static void mt_remove(struct hid_device *hdev)
{
	struct mt_device *td = hid_get_drvdata(hdev);

	del_timer_sync(&td->release_timer);

	sysfs_remove_group(&hdev->dev.kobj, &mt_attribute_group);
	hid_hw_stop(hdev);
	hdev->quirks = td->initial_quirks;
}

/*
 * This list contains only:
 * - VID/PID of products not working with the default multitouch handling
 * - 2 generic rules.
 * So there is no point in adding here any device with MT_CLS_DEFAULT.
 */
static const struct hid_device_id mt_devices[] = {

	/* 3M panels */
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M1968) },
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M2256) },
	{ .driver_data = MT_CLS_3M,
		MT_USB_DEVICE(USB_VENDOR_ID_3M,
			USB_DEVICE_ID_3M3266) },

	/* Alps devices */
	{ .driver_data = MT_CLS_WIN_8_DUAL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ALPS_JP,
			HID_DEVICE_ID_ALPS_U1_DUAL_PTP) },
	{ .driver_data = MT_CLS_WIN_8_DUAL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ALPS_JP,
			HID_DEVICE_ID_ALPS_U1_DUAL_3BTN_PTP) },

	/* Lenovo X1 TAB Gen 2 */
	{ .driver_data = MT_CLS_WIN_8_DUAL,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			   USB_VENDOR_ID_LENOVO,
			   USB_DEVICE_ID_LENOVO_X1_TAB) },

	/* Anton devices */
	{ .driver_data = MT_CLS_EXPORT_ALL_INPUTS,
		MT_USB_DEVICE(USB_VENDOR_ID_ANTON,
			USB_DEVICE_ID_ANTON_TOUCH_PAD) },

	/* Asus T304UA */
	{ .driver_data = MT_CLS_ASUS,
		HID_DEVICE(BUS_USB, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_ASUSTEK,
			USB_DEVICE_ID_ASUSTEK_T304_KEYBOARD) },

	/* Atmel panels */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_ATMEL,
			USB_DEVICE_ID_ATMEL_MXT_DIGITIZER) },

	/* Baanto multitouch devices */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_BAANTO,
			USB_DEVICE_ID_BAANTO_MT_190W2) },

	/* Cando panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH) },
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_MULTI_TOUCH_15_6) },

	/* Chunghwa Telecom touch panels */
	{  .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CHUNGHWAT,
			USB_DEVICE_ID_CHUNGHWAT_MULTITOUCH) },

	/* Cirque devices */
	{ .driver_data = MT_CLS_WIN_8_DUAL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			I2C_VENDOR_ID_CIRQUE,
			I2C_PRODUCT_ID_CIRQUE_121F) },

	/* CJTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CJTOUCH,
			USB_DEVICE_ID_CJTOUCH_MULTI_TOUCH_0020) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CJTOUCH,
			USB_DEVICE_ID_CJTOUCH_MULTI_TOUCH_0040) },

	/* CVTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_CVTOUCH,
			USB_DEVICE_ID_CVTOUCH_SCREEN) },

	/* eGalax devices (resistive) */
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_480D) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_480E) },

	/* eGalax devices (capacitive) */
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7207) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_720C) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7224) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_722A) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_725E) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7262) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_726B) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72A1) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72AA) },
	{ .driver_data = MT_CLS_EGALAX,
		HID_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72C4) },
	{ .driver_data = MT_CLS_EGALAX,
		HID_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72D0) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_72FA) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7302) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_7349) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_73F7) },
	{ .driver_data = MT_CLS_EGALAX_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_A001) },
	{ .driver_data = MT_CLS_EGALAX,
		MT_USB_DEVICE(USB_VENDOR_ID_DWAV,
			USB_DEVICE_ID_DWAV_EGALAX_MULTITOUCH_C002) },

	/* Elitegroup panel */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_ELITEGROUP,
			USB_DEVICE_ID_ELITEGROUP_05D8) },

	/* Flatfrog Panels */
	{ .driver_data = MT_CLS_FLATFROG,
		MT_USB_DEVICE(USB_VENDOR_ID_FLATFROG,
			USB_DEVICE_ID_MULTITOUCH_3200) },

	/* FocalTech Panels */
	{ .driver_data = MT_CLS_SERIAL,
		MT_USB_DEVICE(USB_VENDOR_ID_CYGNAL,
			USB_DEVICE_ID_FOCALTECH_FTXXXX_MULTITOUCH) },

	/* GeneralTouch panel */
	{ .driver_data = MT_CLS_GENERALTOUCH_TWOFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN7_TWOFINGERS) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PWT_TENFINGERS) },
	{ .driver_data = MT_CLS_GENERALTOUCH_TWOFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0101) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0102) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_0106) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_010A) },
	{ .driver_data = MT_CLS_GENERALTOUCH_PWT_TENFINGERS,
		MT_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH,
			USB_DEVICE_ID_GENERAL_TOUCH_WIN8_PIT_E100) },

	/* Gametel game controller */
	{ .driver_data = MT_CLS_NSMU,
		MT_BT_DEVICE(USB_VENDOR_ID_FRUCTEL,
			USB_DEVICE_ID_GAMETEL_MT_MODE) },

	/* GoodTouch panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_GOODTOUCH,
			USB_DEVICE_ID_GOODTOUCH_000f) },

	/* Hanvon panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTID,
		MT_USB_DEVICE(USB_VENDOR_ID_HANVON_ALT,
			USB_DEVICE_ID_HANVON_ALT_MULTITOUCH) },

	/* HONOR GLO-GXXX panel */
	{ .driver_data = MT_CLS_VTL,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			0x347d, 0x7853) },

	/* Ilitek dual touch panel */
	{  .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_ILITEK,
			USB_DEVICE_ID_ILITEK_MULTITOUCH) },

	/* LG Melfas panel */
	{ .driver_data = MT_CLS_LG,
		HID_USB_DEVICE(USB_VENDOR_ID_LG,
			USB_DEVICE_ID_LG_MELFAS_MT) },

	/* MosArt panels */
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUS_T91MT)},
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_ASUS,
			USB_DEVICE_ID_ASUSTEK_MULTITOUCH_YFO) },
	{ .driver_data = MT_CLS_CONFIDENCE_MINUS_ONE,
		MT_USB_DEVICE(USB_VENDOR_ID_TURBOX,
			USB_DEVICE_ID_TURBOX_TOUCHSCREEN_MOSART) },

	/* Panasonic panels */
	{ .driver_data = MT_CLS_PANASONIC,
		MT_USB_DEVICE(USB_VENDOR_ID_PANASONIC,
			USB_DEVICE_ID_PANABOARD_UBT780) },
	{ .driver_data = MT_CLS_PANASONIC,
		MT_USB_DEVICE(USB_VENDOR_ID_PANASONIC,
			USB_DEVICE_ID_PANABOARD_UBT880) },

	/* Novatek Panel */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_NOVATEK,
			USB_DEVICE_ID_NOVATEK_PCT) },

	/* Ntrig Panel */
	{ .driver_data = MT_CLS_NSMU,
		HID_DEVICE(BUS_I2C, HID_GROUP_MULTITOUCH_WIN_8,
			USB_VENDOR_ID_NTRIG, 0x1b05) },

	/* PixArt optical touch screen */
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN) },
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN1) },
	{ .driver_data = MT_CLS_INRANGE_CONTACTNUMBER,
		MT_USB_DEVICE(USB_VENDOR_ID_PIXART,
			USB_DEVICE_ID_PIXART_OPTICAL_TOUCH_SCREEN2) },

	/* PixCir-based panels */
	{ .driver_data = MT_CLS_DUAL_INRANGE_CONTACTID,
		MT_USB_DEVICE(USB_VENDOR_ID_CANDO,
			USB_DEVICE_ID_CANDO_PIXCIR_MULTI_TOUCH) },

	/* Quanta-based panels */
	{ .driver_data = MT_CLS_CONFIDENCE_CONTACT_ID,
		MT_USB_DEVICE(USB_VENDOR_ID_QUANTA,
			USB_DEVICE_ID_QUANTA_OPTICAL_TOUCH_3001) },

	/* Stantum panels */
	{ .driver_data = MT_CLS_CONFIDENCE,
		MT_USB_DEVICE(USB_VENDOR_ID_STANTUM_STM,
			USB_DEVICE_ID_MTP_STM)},

	/* TopSeed panels */
	{ .driver_data = MT_CLS_TOPSEED,
		MT_USB_DEVICE(USB_VENDOR_ID_TOPSEED2,
			USB_DEVICE_ID_TOPSEED2_PERIPAD_701) },

	/* Touch International panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_TOUCH_INTL,
			USB_DEVICE_ID_TOUCH_INTL_MULTI_TOUCH) },

	/* Unitec panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_UNITEC,
			USB_DEVICE_ID_UNITEC_USB_TOUCH_0709) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_UNITEC,
			USB_DEVICE_ID_UNITEC_USB_TOUCH_0A19) },

	/* VTL panels */
	{ .driver_data = MT_CLS_VTL,
		MT_USB_DEVICE(USB_VENDOR_ID_VTL,
			USB_DEVICE_ID_VTL_MULTITOUCH_FF3F) },

	/* Wistron panels */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_WISTRON,
			USB_DEVICE_ID_WISTRON_OPTICAL_TOUCH) },

	/* XAT */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XAT,
			USB_DEVICE_ID_XAT_CSR) },

	/* Xiroku */
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR1) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_SPX2) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_MPX2) },
	{ .driver_data = MT_CLS_NSMU,
		MT_USB_DEVICE(USB_VENDOR_ID_XIROKU,
			USB_DEVICE_ID_XIROKU_CSR2) },

	/* Google MT devices */
	{ .driver_data = MT_CLS_GOOGLE,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_ANY, USB_VENDOR_ID_GOOGLE,
			USB_DEVICE_ID_GOOGLE_TOUCH_ROSE) },

	/* Generic MT device */
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH, HID_ANY_ID, HID_ANY_ID) },

	/* Generic Win 8 certified MT device */
	{  .driver_data = MT_CLS_WIN_8,
		HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH_WIN_8,
			HID_ANY_ID, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, mt_devices);

static const struct hid_usage_id mt_grabbed_usages[] = {
	{ HID_ANY_ID, HID_ANY_ID, HID_ANY_ID },
	{ HID_ANY_ID - 1, HID_ANY_ID - 1, HID_ANY_ID - 1}
};

static struct hid_driver mt_driver = {
	.name = "hid-multitouch",
	.id_table = mt_devices,
	.probe = mt_probe,
	.remove = mt_remove,
	.input_mapping = mt_input_mapping,
	.input_mapped = mt_input_mapped,
	.input_configured = mt_input_configured,
	.feature_mapping = mt_feature_mapping,
	.usage_table = mt_grabbed_usages,
	.event = mt_event,
	.report = mt_report,
#ifdef CONFIG_PM
	.reset_resume = mt_reset_resume,
	.resume = mt_resume,
#endif
};
module_hid_driver(mt_driver);
