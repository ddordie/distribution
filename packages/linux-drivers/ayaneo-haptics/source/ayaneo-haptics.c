// SPDX-License-Identifier: GPL-2.0
/*
 * ayaneo-haptics.c — AYANEO Controller FF_RUMBLE → HID output report
 *
 * Pocket S vibration motor is embedded in the AYANEO Controller MCU
 * and controlled via HID output report, NOT PMIC haptics.
 *
 * Protocol: 8-byte HID output report
 *   bytes[4] = left motor  (0=off, any non-zero=on)
 *   bytes[5] = right motor (0=off, any non-zero=on)
 *   rest = 0
 *
 * IMPORTANT: hid_hw_output_report() may sleep (USB control transfer),
 * so we defer all HID I/O to a workqueue. FF callbacks are called
 * under event_lock spinlock and must not block.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/hid.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define CONTROLLER_NAME "AYANEO Controller"
#define MAX_EFFECTS 4
#define HID_REPORT_SIZE 8
#define LEFT_MOTOR_BYTE  4
#define RIGHT_MOTOR_BYTE 5
#define RUMBLE_TIMEOUT_MS 5000  /* safety: auto-stop after 5s */

struct ayaneo_state {
	struct input_dev *controller;
	struct hid_device *hid_dev;
	bool bridge_active;
	struct work_struct rumble_work;
	u8 cur_left;
	u8 cur_right;
	struct delayed_work stop_work;
};

struct ayaneo_ff {
	struct ff_effect effects[MAX_EFFECTS];
	bool used[MAX_EFFECTS];
};

static struct ayaneo_state state;

static int _find_controller(struct device *dev, void *data)
{
	struct input_dev *idev = to_input_dev(dev);

	if (idev->name && strstr(idev->name, CONTROLLER_NAME)) {
		state.controller = idev;
		return 1;
	}
	return 0;
}

/* Runs in workqueue context — safe to sleep (USB I/O) */
static void rumble_work_fn(struct work_struct *work)
{
	u8 report[HID_REPORT_SIZE];

	if (!state.hid_dev)
		return;

	memset(report, 0, HID_REPORT_SIZE);
	report[LEFT_MOTOR_BYTE]  = state.cur_left;
	report[RIGHT_MOTOR_BYTE] = state.cur_right;

	hid_hw_output_report(state.hid_dev, report, HID_REPORT_SIZE);
}

static void stop_work_fn(struct work_struct *work)
{
	state.cur_left  = 0;
	state.cur_right = 0;
	schedule_work(&state.rumble_work);
}

/* Schedule a rumble update (non-blocking, safe from any context) */
static void schedule_rumble(u8 left, u8 right)
{
	state.cur_left  = left;
	state.cur_right = right;

	/* Cancel any pending auto-stop */
	cancel_delayed_work(&state.stop_work);

	/* Schedule the HID write */
	schedule_work(&state.rumble_work);

	/* If rumbling, schedule auto-stop safety net */
	if (left || right)
		schedule_delayed_work(&state.stop_work,
			msecs_to_jiffies(RUMBLE_TIMEOUT_MS));
}

/* ── FF callbacks (called under event_lock — MUST NOT block) ── */

static int ayaneo_ff_upload(struct input_dev *dev,
		struct ff_effect *effect, struct ff_effect *old)
{
	struct ayaneo_ff *aff = dev->ff->private;
	int id;

	for (id = 0; id < MAX_EFFECTS; id++) {
		if (!aff->used[id]) {
			aff->effects[id] = *effect;
			aff->used[id] = true;
			effect->id = id;
			pr_info("ayaneo-haptics: upload effect %d type=0x%x\n",
				id, effect->type);
			return 0;
		}
	}
	return -ENOSPC;
}

static int ayaneo_ff_playback(struct input_dev *dev, int effect_id, int value)
{
	struct ayaneo_ff *aff = dev->ff->private;
	u16 strong = 0, weak = 0;

	if (effect_id < 0 || effect_id >= MAX_EFFECTS || !aff->used[effect_id])
		return -EINVAL;

	pr_info("ayaneo-haptics: playback id=%d value=%d\n", effect_id, value);

	if (!value) {
		schedule_rumble(0, 0);
		return 0;
	}

	switch (aff->effects[effect_id].type) {
	case FF_RUMBLE:
		strong = aff->effects[effect_id].u.rumble.strong_magnitude;
		weak   = aff->effects[effect_id].u.rumble.weak_magnitude;
		break;
	case FF_CONSTANT:
		strong = aff->effects[effect_id].u.constant.level;
		weak = strong;
		break;
	default:
		return 0;
	}

	schedule_rumble(strong ? 0xFF : 0, weak ? 0xFF : 0);
	return 0;
}

static int ayaneo_ff_erase(struct input_dev *dev, int effect_id)
{
	struct ayaneo_ff *aff = dev->ff->private;

	pr_info("ayaneo-haptics: erase id=%d\n", effect_id);

	if (effect_id >= 0 && effect_id < MAX_EFFECTS && aff->used[effect_id]) {
		schedule_rumble(0, 0);
		aff->used[effect_id] = false;
	}
	return 0;
}

/* ── bridge setup ── */

static void ayaneo_bridge_setup(void)
{
	struct ayaneo_ff *aff;
	int ret;

	if (!state.controller || state.bridge_active)
		return;

	state.hid_dev = input_get_drvdata(state.controller);
	if (!state.hid_dev) {
		pr_err("ayaneo-haptics: cannot get hid_device from '%s'\n",
			state.controller->name ?: "?");
		return;
	}

	pr_info("ayaneo-haptics: found '%s' (%s), hid=%s\n",
		state.controller->name ?: "?",
		dev_name(&state.controller->dev),
		dev_name(&state.hid_dev->dev));

	aff = kzalloc(sizeof(*aff), GFP_KERNEL);
	if (!aff)
		return;

	INIT_WORK(&state.rumble_work, rumble_work_fn);
	INIT_DELAYED_WORK(&state.stop_work, stop_work_fn);

	input_set_capability(state.controller, EV_FF, FF_RUMBLE);
	input_set_capability(state.controller, EV_FF, FF_CONSTANT);
	input_set_capability(state.controller, EV_FF, FF_PERIODIC);

	ret = input_ff_create(state.controller, MAX_EFFECTS);
	if (ret) {
		pr_err("ayaneo-haptics: input_ff_create failed (%d)\n", ret);
		kfree(aff);
		return;
	}

	state.controller->ff->private  = aff;
	state.controller->ff->upload   = ayaneo_ff_upload;
	state.controller->ff->playback = ayaneo_ff_playback;
	state.controller->ff->erase    = ayaneo_ff_erase;

	state.bridge_active = true;
	pr_info("ayaneo-haptics: FF bridge active (HID output report)\n");
}

/* ── module lifecycle ── */

static int __init ayaneo_haptics_init(void)
{
	state = (struct ayaneo_state){0};

	class_for_each_device(&input_class, NULL, NULL, _find_controller);
	if (!state.controller) {
		pr_err("ayaneo-haptics: no '%s' device found\n", CONTROLLER_NAME);
		return -ENODEV;
	}

	ayaneo_bridge_setup();
	if (!state.bridge_active)
		return -EIO;

	return 0;
}

static void __exit ayaneo_haptics_exit(void)
{
	if (state.bridge_active && state.controller) {
		struct ayaneo_ff *aff = state.controller->ff->private;

		cancel_delayed_work_sync(&state.stop_work);
		cancel_work_sync(&state.rumble_work);
		/* Send stop command synchronously before cleanup */
		if (state.hid_dev) {
			u8 report[HID_REPORT_SIZE] = {0};
			hid_hw_output_report(state.hid_dev, report, HID_REPORT_SIZE);
		}
		input_ff_destroy(state.controller);
		kfree(aff);
		clear_bit(EV_FF, state.controller->evbit);
		clear_bit(FF_RUMBLE, state.controller->ffbit);
		clear_bit(FF_CONSTANT, state.controller->ffbit);
		clear_bit(FF_PERIODIC, state.controller->ffbit);
		pr_info("ayaneo-haptics: FF bridge removed\n");
	}
}

module_init(ayaneo_haptics_init);
module_exit(ayaneo_haptics_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ddordie");
MODULE_DESCRIPTION("AYANEO Controller FF -> HID output report bridge");
