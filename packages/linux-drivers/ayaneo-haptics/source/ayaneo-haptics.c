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
	int haptics_id[MAX_EFFECTS];  /* haptics driver's effect slot */
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

	if (effect->type != FF_RUMBLE && effect->type != FF_PERIODIC)
		return -EINVAL;

	for (id = 0; id < MAX_EFFECTS; id++) {
		if (!aff->used[id]) {
			aff->effects[id] = *effect;
			aff->used[id] = true;
			aff->haptics_id[id] = -1;
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
	struct ff_effect he;
	u16 mag;
	int ret;

	pr_info("ayaneo-haptics: playback id=%d value=%d\n", effect_id, value);

	if (effect_id < 0 || effect_id >= MAX_EFFECTS || !aff->used[effect_id]) {
		pr_info("ayaneo-haptics: playback rejected (bad id/unused)\n");
		return -EINVAL;
	}

	if (!value) {
		pr_info("ayaneo-haptics: playback stop id=%d haptics_id=%d\n",
			effect_id, aff->haptics_id[effect_id]);
		if (aff->haptics_id[effect_id] >= 0)
			state.haptics->ff->playback(state.haptics,
				aff->haptics_id[effect_id], 0);
		return 0;
	}

	if (aff->effects[effect_id].type == FF_RUMBLE) {
		mag = max(aff->effects[effect_id].u.rumble.strong_magnitude,
			  aff->effects[effect_id].u.rumble.weak_magnitude);
	} else {
		/* FF_PERIODIC: use magnitude directly */
		mag = aff->effects[effect_id].u.periodic.magnitude;
	}
	if (!mag) {
		if (aff->haptics_id[effect_id] >= 0) {
			state.haptics->ff->playback(state.haptics,
				aff->haptics_id[effect_id], 0);
		}
		return 0;
	}

	/* Haptics driver ffbit uses old encoding (FF_PERIODIC=0x10),
	 * but its switch() in haptics_upload_effect uses new encoding
	 * (FF_PERIODIC=0x51). Pass new value for the switch. */
#define HAPTICS_FF_PERIODIC 0x51

	/* FF_RUMBLE → HAPTICS_FF_PERIODIC, or pass through PERIODIC as-is */
	memset(&he, 0, sizeof(he));
	he.type = HAPTICS_FF_PERIODIC;
	he.id   = -1;
	if (aff->effects[effect_id].type == FF_PERIODIC) {
		/* passthrough the PERIODIC effect directly */
		he.u.periodic = aff->effects[effect_id].u.periodic;
		he.direction  = aff->effects[effect_id].direction;
		he.replay     = aff->effects[effect_id].replay;
	} else {
		he.u.periodic.waveform  = FF_SINE;
		he.u.periodic.period    = 50;
		he.u.periodic.magnitude = mag;
		he.direction = aff->effects[effect_id].direction;
		he.replay    = aff->effects[effect_id].replay;
		if (he.replay.length > 0) {
			he.u.periodic.envelope.attack_length = he.replay.length / 2;
			he.u.periodic.envelope.fade_length   = he.replay.length / 2;
		}
	}

	/* Bypass input_ff_upload() which rejects old FF type 0x10
	 * (below FF_EFFECT_MIN=79 in kernel 7.1). Do slot allocation
	 * and call haptics' upload callback directly. */
	{
		struct ff_device *ff = state.haptics->ff;
		int id;

		mutex_lock(&ff->mutex);
		for (id = 0; id < ff->max_effects; id++)
			if (!ff->effect_owners[id])
				break;
		if (id >= ff->max_effects) {
			mutex_unlock(&ff->mutex);
			pr_info("ayaneo-haptics: haptics out of effect slots\n");
			return 0;
		}
		he.id = id;
		ff->effect_owners[id] = (void *)1; /* marker */
		pr_info("ayaneo-haptics: calling haptics upload id=%d type=0x%x waveform=0x%x mag=%u\n",
			id, he.type, he.u.periodic.waveform, he.u.periodic.magnitude);
		ret = ff->upload(state.haptics, &he, NULL);
		pr_info("ayaneo-haptics: haptics upload returned %d\n", ret);
		if (ret < 0)
			ff->effect_owners[id] = NULL;
		mutex_unlock(&ff->mutex);
	}
	if (ret == 0) {
		int play_ret;

		aff->haptics_id[effect_id] = he.id;
		pr_info("ayaneo-haptics: playback fwd id=%d mag=%u -> haptics_id=%d\n",
			effect_id, mag, he.id);
		play_ret = state.haptics->ff->playback(state.haptics, he.id, 1);
		pr_info("ayaneo-haptics: haptics playback returned %d\n", play_ret);
	} else {
		pr_info("ayaneo-haptics: playback upload to haptics FAILED ret=%d (mag=%u)\n",
			ret, mag);
	}
	return 0;
}

static int ayaneo_ff_erase(struct input_dev *dev, int effect_id)
{
	struct ayaneo_ff *aff = dev->ff->private;

	pr_info("ayaneo-haptics: erase id=%d\n", effect_id);

	if (effect_id >= 0 && effect_id < MAX_EFFECTS && aff->used[effect_id]) {
		if (aff->haptics_id[effect_id] >= 0) {
			state.haptics->ff->playback(state.haptics,
				aff->haptics_id[effect_id], 0);
			state.haptics->ff->effect_owners[aff->haptics_id[effect_id]] = NULL;
			aff->haptics_id[effect_id] = -1;
		}
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
	input_set_capability(state.controller, EV_FF, FF_PERIODIC);

	pr_info("ayaneo-haptics: after set_capability ffbit[0]=0x%lx ffbit[1]=0x%lx\n",
		state.controller->ffbit[0], state.controller->ffbit[1]);

	ret = input_ff_create(state.controller, MAX_EFFECTS);
	if (ret) {
		pr_err("ayaneo-haptics: input_ff_create failed (%d)\n", ret);
		kfree(aff);
		return;
	}

	pr_info("ayaneo-haptics: after ff_create ffbit[0]=0x%lx ffbit[1]=0x%lx\n",
		state.controller->ffbit[0], state.controller->ffbit[1]);

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

	class_for_each_device(&input_class, NULL, NULL, _find_haptics);
	if (!state.haptics) {
		pr_err("ayaneo-haptics: no haptics device found\n");
		return -ENODEV;
	}

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
