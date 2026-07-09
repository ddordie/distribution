// SPDX-License-Identifier: GPL-2.0
/*
 * ayaneo-haptics.c — Bridge AYANEO Controller FF_RUMBLE → qcom-hv-haptics
 *
 * The Pocket S uses a USB HID "AYANEO Controller" that has no EV_FF.
 * This module patches FF_RUMBLE onto it and forwards vibration events
 * to the qcom-hv-haptics PMIC driver as FF_PERIODIC.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>

#define CONTROLLER_NAME "AYANEO Controller"
#define MAX_EFFECTS 4

struct ayaneo_state {
	struct input_dev *controller;
	struct input_dev *haptics;
	bool bridge_active;
};

struct ayaneo_ff {
	struct ff_effect effects[MAX_EFFECTS];
	bool used[MAX_EFFECTS];
};

/* ── global singleton ── */
static struct ayaneo_state state;

/* ── look up by name via the input class ── */

static int _find_haptics(struct device *dev, void *data)
{
	struct input_dev *idev = to_input_dev(dev);

	if (!test_bit(EV_FF, idev->evbit))
		return 0;
	if (test_bit(FF_PERIODIC, idev->ffbit)) {
		if (!state.haptics)
			state.haptics = idev;
	}
	return 0;
}

static int _find_controller(struct device *dev, void *data)
{
	struct input_dev *idev = to_input_dev(dev);

	if (idev->name && strstr(idev->name, CONTROLLER_NAME)) {
		state.controller = idev;
		return 1;
	}
	return 0;
}

/* ── FF callbacks (called on controller_dev by input core) ── */

static int ayaneo_ff_upload(struct input_dev *dev,
			    struct ff_effect *effect,
			    struct ff_effect *old)
{
	struct ayaneo_ff *aff = dev->ff->private;
	int id;

	if (effect->type != FF_RUMBLE)
		return -EINVAL;

	for (id = 0; id < MAX_EFFECTS; id++) {
		if (!aff->used[id]) {
			aff->effects[id] = *effect;
			aff->used[id] = true;
			effect->id = id;
			return 0;
		}
	}
	return -ENOSPC;
}

static int ayaneo_ff_playback(struct input_dev *dev, int effect_id, int value)
{
	struct ayaneo_ff *aff = dev->ff->private;
	struct ff_effect he;
	u16 mag;

	if (effect_id < 0 || effect_id >= MAX_EFFECTS || !aff->used[effect_id])
		return -EINVAL;

	if (!value) {
		input_ff_event(state.haptics, EV_FF, effect_id, 0);
		return 0;
	}

	mag = max(aff->effects[effect_id].u.rumble.strong_magnitude,
		  aff->effects[effect_id].u.rumble.weak_magnitude);
	if (!mag) {
		input_ff_event(state.haptics, EV_FF, effect_id, 0);
		return 0;
	}

	/* FF_RUMBLE → FF_PERIODIC */
	memset(&he, 0, sizeof(he));
	he.type = FF_PERIODIC;
	he.id   = effect_id;
	he.u.periodic.waveform  = FF_SINE;
	he.u.periodic.period    = 50;
	he.u.periodic.magnitude = mag;
	he.direction = aff->effects[effect_id].direction;
	he.replay    = aff->effects[effect_id].replay;
	if (aff->effects[effect_id].replay.length > 0) {
		he.u.periodic.envelope.attack_length =
			aff->effects[effect_id].replay.length / 2;
		he.u.periodic.envelope.fade_length =
			aff->effects[effect_id].replay.length / 2;
	}

	input_ff_upload(state.haptics, &he, NULL);
	input_ff_event(state.haptics, EV_FF, effect_id, 1);
	return 0;
}

static int ayaneo_ff_erase(struct input_dev *dev, int effect_id)
{
	struct ayaneo_ff *aff = dev->ff->private;

	if (effect_id >= 0 && effect_id < MAX_EFFECTS) {
		input_ff_event(state.haptics, EV_FF, effect_id, 0);
		aff->used[effect_id] = false;
	}
	return 0;
}

/* ── bridge setup ── */

static void ayaneo_bridge_setup(void)
{
	struct ayaneo_ff *aff;
	int ret;

	if (!state.controller || !state.haptics || state.bridge_active)
		return;

	pr_info("ayaneo-haptics: found '%s' (%s) and haptics (%s)\n",
		state.controller->name ?: "?",
		dev_name(&state.controller->dev),
		dev_name(&state.haptics->dev));

	aff = kzalloc(sizeof(*aff), GFP_KERNEL);
	if (!aff)
		return;

	input_set_capability(state.controller, EV_FF, FF_RUMBLE);

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
	pr_info("ayaneo-haptics: FF bridge active  "
		"RUMBLE (%s) -> PERIODIC (%s)\n",
		dev_name(&state.controller->dev),
		dev_name(&state.haptics->dev));
}

/* ── module lifecycle ── */

static int __init ayaneo_haptics_init(void)
{
	state = (struct ayaneo_state){0};

	/* first find the haptics device — any input_dev with FF_PERIODIC */
	class_for_each_device(&input_class, NULL, NULL, _find_haptics);
	if (!state.haptics) {
		pr_err("ayaneo-haptics: no haptics device found\n");
		return -ENODEV;
	}

	/* then find the controller */
	class_for_each_device(&input_class, NULL, NULL, _find_controller);
	if (!state.controller) {
		pr_err("ayaneo-haptics: no '%s' device found\n",
		       CONTROLLER_NAME);
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
		input_ff_destroy(state.controller);
		kfree(aff);
		clear_bit(EV_FF, state.controller->evbit);
		clear_bit(FF_RUMBLE, state.controller->ffbit);
		pr_info("ayaneo-haptics: FF bridge removed\n");
	}
}

module_init(ayaneo_haptics_init);
module_exit(ayaneo_haptics_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ddordie");
MODULE_DESCRIPTION("AYANEO Controller FF_RUMBLE -> qcom-hv-haptics bridge");
