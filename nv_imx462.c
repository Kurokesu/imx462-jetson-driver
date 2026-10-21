// SPDX-License-Identifier: GPL-2.0
/*
 * imx462 sensor driver
 *
 * Copyright (c) 2016-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * Copyright (c) 2025-2026, UAB Kurokesu. All rights reserved.
 */

/* #define DEBUG */

#include <nvidia/conftest.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>
#include "imx462_mode_tbls.h"

/* IMX462 Register Definitions */
#define IMX462_MIN_FRAME_LENGTH (1125)
#define IMX462_MAX_FRAME_LENGTH (0x1FFFF)
#define IMX462_MIN_COARSE_EXPOSURE (1)
#define IMX462_MIN_SHS1 (1)

#define IMX462_VMAX_ADDR_LSB 0x3018
#define IMX462_VMAX_ADDR_MID 0x3019
#define IMX462_VMAX_ADDR_MSB 0x301A

#define IMX462_COARSE_TIME_SHS1_ADDR_LSB 0x3020
#define IMX462_COARSE_TIME_SHS1_ADDR_MID 0x3021
#define IMX462_COARSE_TIME_SHS1_ADDR_MSB 0x3022

#define IMX462_GAIN_ADDR 0x3014

#define IMX462_GROUP_HOLD_ADDR 0x3001

/* TODO: IMX462 has no model ID. We read a few registers with known values. */
#define IMX462_MODEL_ID_ADDR_MSB 0x3004
#define IMX462_MODEL_ID_ADDR_LSB 0x3008

/* Test pattern generator */
#define IMX462_PGCTRL 0x308C
#define IMX462_PGCTRL_REGEN BIT(0)
#define IMX462_PGCTRL_THRU BIT(1)
#define IMX462_PGCTRL_MODE(n) ((n) << 4)

/* Conversion gain */
#define IMX462_FDG_HCG BIT(4)
#define IMX462_FDG_LCG 0
#define IMX462_FRSEL_60FPS 0x01

static const struct of_device_id imx462_of_match[] = {
	{ .compatible = "sony,imx462" },
	{},
};

MODULE_DEVICE_TABLE(of, imx462_of_match);

static int test_mode;
module_param(test_mode, int, 0644);

static bool hcg_mode;
module_param(hcg_mode, bool, 0644);
MODULE_PARM_DESC(hcg_mode, "Enable HCG mode");

static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

enum imx462_Config {
	TWO_LANE_CONFIG,
	FOUR_LANE_CONFIG,
};

struct imx462 {
	struct i2c_client *i2c_client;
	struct v4l2_subdev *subdev;
	u32 frame_length;
	s64 exposure_us;
	struct camera_common_data *s_data;
	struct tegracam_device *tc_dev;
	enum imx462_Config config;
};

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static inline void imx462_get_vmax_regs(imx462_reg *regs, u32 vmax)
{
	regs->addr = IMX462_VMAX_ADDR_MSB;
	regs->val = (vmax >> 16) & 0x0f;

	(regs + 1)->addr = IMX462_VMAX_ADDR_MID;
	(regs + 1)->val = (vmax >> 8) & 0xff;

	(regs + 2)->addr = IMX462_VMAX_ADDR_LSB;
	(regs + 2)->val = (vmax) & 0xff;
}

static inline void imx462_get_coarse_time_regs_shs1(imx462_reg *regs,
						    u32 coarse_time)
{
	regs->addr = IMX462_COARSE_TIME_SHS1_ADDR_MSB;
	regs->val = (coarse_time >> 16) & 0x0f;

	(regs + 1)->addr = IMX462_COARSE_TIME_SHS1_ADDR_MID;
	(regs + 1)->val = (coarse_time >> 8) & 0xff;

	(regs + 2)->addr = IMX462_COARSE_TIME_SHS1_ADDR_LSB;
	(regs + 2)->val = (coarse_time) & 0xff;
}

static inline void imx462_get_gain_reg(imx462_reg *regs, u8 gain)
{
	regs->addr = IMX462_GAIN_ADDR;
	regs->val = gain;
}

static inline int imx462_read_reg(struct camera_common_data *s_data, u16 addr,
				  u8 *val)
{
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(s_data->regmap, addr, &reg_val);
	*val = reg_val & 0xff;

	return err;
}

static inline int imx462_write_reg(struct camera_common_data *s_data, u16 addr,
				   u8 val)
{
	int err = 0;

	err = regmap_write(s_data->regmap, addr, val);
	if (err)
		dev_err(s_data->dev, "%s: i2c write failed, 0x%x = %x",
			__func__, addr, val);

	return err;
}

static int imx462_write_table(struct imx462 *priv, const imx462_reg table[])
{
	int err;

	dev_dbg(priv->s_data->dev, "%s: Writing register table\n", __func__);

	err = regmap_util_write_table_8(priv->s_data->regmap, table, NULL, 0,
					IMX462_TABLE_WAIT_MS, IMX462_TABLE_END);

	if (err) {
		dev_err(priv->s_data->dev, "%s: Failed to write table (%d)\n",
			__func__, err);
	} else {
		dev_dbg(priv->s_data->dev,
			"%s: Register table written successfully\n", __func__);
	}

	return err;
}

static int imx462_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = tc_dev->dev;
	int err = 0;

	err = imx462_write_reg(s_data, IMX462_GROUP_HOLD_ADDR, val);
	if (err)
		dev_dbg(dev, "%s: Group hold control error\n", __func__);

	return err;
}

static int imx462_set_coarse_time(struct imx462 *priv, s64 val)
{
	struct camera_common_data *s_data = priv->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode];
	struct device *dev = priv->tc_dev->dev;
	imx462_reg reg_list[3];
	int err = 0;
	u32 coarse_time_shs1;
	u32 reg_shs1;
	int i = 0;

	if (mode->control_properties.exposure_factor == 0 ||
	    mode->image_properties.line_length == 0) {
		dev_err(dev, "%s:error line_len = %d, exposure_factor = %d\n",
			__func__, mode->control_properties.exposure_factor,
			mode->image_properties.line_length);
		err = -EINVAL;
		goto fail;
	}

	coarse_time_shs1 = DIV_ROUND_CLOSEST(
		mode->signal_properties.pixel_clock.val * val /
			mode->image_properties.line_length,
		mode->control_properties.exposure_factor);

	if (priv->frame_length == 0)
		priv->frame_length = IMX462_MIN_FRAME_LENGTH;

	/* SHS1 = VMAX - coarse_time - 1, valid SHS1 range is 1 to VMAX - 2 */
	if (coarse_time_shs1 > priv->frame_length - IMX462_MIN_SHS1 - 1)
		coarse_time_shs1 = priv->frame_length - IMX462_MIN_SHS1 - 1;
	if (coarse_time_shs1 < IMX462_MIN_COARSE_EXPOSURE)
		coarse_time_shs1 = IMX462_MIN_COARSE_EXPOSURE;

	reg_shs1 = priv->frame_length - coarse_time_shs1 - 1;

	dev_dbg(dev, "%s: coarse1:%d, shs1:%d, FL:%d\n", __func__,
		coarse_time_shs1, reg_shs1, priv->frame_length);

	imx462_get_coarse_time_regs_shs1(reg_list, reg_shs1);

	for (i = 0; i < 3; i++) {
		err = imx462_write_reg(priv->s_data, reg_list[i].addr,
				       reg_list[i].val);
		if (err)
			goto fail;
	}

	return 0;

fail:
	dev_dbg(dev, "%s: set coarse time error\n", __func__);
	return err;
}

static int imx462_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct device *dev = s_data->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	imx462_reg reg_gain;
	int err = 0;
	u8 gain;

	if (mode->control_properties.gain_factor == 0) {
		dev_err(dev, "%s:error, gain_factor is 0\n", __func__);
		return -EINVAL;
	}

	/* translate value */
	gain = val / 3;
	dev_dbg(dev, "%s: gain reg: %d\n", __func__, gain);

	imx462_get_gain_reg(&reg_gain, gain);

	err = imx462_write_reg(s_data, reg_gain.addr, reg_gain.val);
	if (err)
		dev_dbg(dev, "%s: GAIN control error\n", __func__);

	return err;
}

static int imx462_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct imx462 *priv = (struct imx462 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];

	int err = 0;
	imx462_reg vmax_regs[3];
	u32 vmax;
	int i;

	dev_dbg(dev, "%s: Setting framerate control to: %lld\n", __func__, val);

	if (val == 0 || mode->image_properties.line_length == 0)
		return -EINVAL;

	vmax = mode->signal_properties.pixel_clock.val *
	       mode->control_properties.framerate_factor /
	       mode->image_properties.line_length / val;

	dev_dbg(dev, "pixel_clock %lld\n",
		mode->signal_properties.pixel_clock.val);
	dev_dbg(dev, "framerate_factor %d\n",
		mode->control_properties.framerate_factor);
	dev_dbg(dev, "line_length %d\n", mode->image_properties.line_length);
	dev_dbg(dev, "vmax %d\n", vmax);

	if (vmax < IMX462_MIN_FRAME_LENGTH)
		vmax = IMX462_MIN_FRAME_LENGTH;
	else if (vmax > IMX462_MAX_FRAME_LENGTH)
		vmax = IMX462_MAX_FRAME_LENGTH;

	dev_dbg(dev, "%s: val: %llde-6 [fps], vmax: %u [lines]\n", __func__,
		val, vmax);

	imx462_get_vmax_regs(vmax_regs, vmax);
	for (i = 0; i < 3; i++) {
		err = imx462_write_reg(s_data, vmax_regs[i].addr,
				       vmax_regs[i].val);
		if (err) {
			dev_err(dev, "%s: frame_length control error\n",
				__func__);
			return err;
		}
	}

	priv->frame_length = vmax;

	/* SHS1 counts back from VMAX, recompute it for new frame length */
	if (priv->exposure_us)
		err = imx462_set_coarse_time(priv, priv->exposure_us);

	return err;
}

static int imx462_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct imx462 *priv = (struct imx462 *)tc_dev->priv;
	struct device *dev = tc_dev->dev;
	int err = 0;

	dev_dbg(dev, "%s: val: %lld\n", __func__, val);

	priv->exposure_us = val;
	err = imx462_set_coarse_time(priv, val);
	if (err)
		dev_dbg(dev, "%s: error coarse time SHS1 override\n", __func__);

	return err;
}

static struct tegracam_ctrl_ops imx462_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = imx462_set_gain,
	.set_exposure = imx462_set_exposure,
	.set_frame_rate = imx462_set_frame_rate,
	.set_group_hold = imx462_set_group_hold,
};

static int imx462_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power on\n", __func__);
	if (pdata && pdata->power_on) {
		err = pdata->power_on(pw);
		if (err)
			dev_err(dev, "%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	if (pw->reset_gpio) {
		if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
			gpio_set_value_cansleep(pw->reset_gpio, 0);
		else
			gpio_set_value(pw->reset_gpio, 0);
	}

	if (unlikely(!(pw->avdd || pw->iovdd || pw->dvdd)))
		goto skip_power_seqn;

	usleep_range(10, 20);

	if (pw->avdd) {
		err = regulator_enable(pw->avdd);
		if (err)
			goto imx462_avdd_fail;
	}

	if (pw->iovdd) {
		err = regulator_enable(pw->iovdd);
		if (err)
			goto imx462_iovdd_fail;
	}

	if (pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (err)
			goto imx462_dvdd_fail;
	}

	usleep_range(10, 20);

skip_power_seqn:
	if (pw->reset_gpio) {
		if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
			gpio_set_value_cansleep(pw->reset_gpio, 1);
		else
			gpio_set_value(pw->reset_gpio, 1);
	}

	/* Need to wait for t4 + t5 + t9 + t10 time as per the data sheet */
	/* t4 - 200us, t5 - 21.2ms, t9 - 1.2ms t10 - 270 ms */
	usleep_range(300000, 300100);

	pw->state = SWITCH_ON;

	return 0;

imx462_dvdd_fail:
	regulator_disable(pw->iovdd);

imx462_iovdd_fail:
	regulator_disable(pw->avdd);

imx462_avdd_fail:
	dev_err(dev, "%s failed.\n", __func__);

	return -ENODEV;
}

static int imx462_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	dev_dbg(dev, "%s: power off\n", __func__);

	if (pdata && pdata->power_off) {
		err = pdata->power_off(pw);
		if (err) {
			dev_err(dev, "%s failed.\n", __func__);
			return err;
		}
	} else {
		if (pw->reset_gpio) {
			if (gpiod_cansleep(gpio_to_desc(pw->reset_gpio)))
				gpio_set_value_cansleep(pw->reset_gpio, 0);
			else
				gpio_set_value(pw->reset_gpio, 0);
		}

		usleep_range(10, 10);

		if (pw->dvdd)
			regulator_disable(pw->dvdd);
		if (pw->iovdd)
			regulator_disable(pw->iovdd);
		if (pw->avdd)
			regulator_disable(pw->avdd);
	}

	pw->state = SWITCH_OFF;

	return 0;
}

static int imx462_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;

	if (likely(pw->dvdd))
		devm_regulator_put(pw->dvdd);

	if (likely(pw->avdd))
		devm_regulator_put(pw->avdd);

	if (likely(pw->iovdd))
		devm_regulator_put(pw->iovdd);

	pw->dvdd = NULL;
	pw->avdd = NULL;
	pw->iovdd = NULL;

	if (likely(pw->reset_gpio))
		gpio_free(pw->reset_gpio);

	return 0;
}

static int imx462_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct clk *parent;
	int err = 0;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}

	/* Sensor MCLK (aka. INCK) */
	if (pdata->mclk_name) {
		pw->mclk = devm_clk_get(dev, pdata->mclk_name);
		if (IS_ERR(pw->mclk)) {
			dev_err(dev, "unable to get clock %s\n",
				pdata->mclk_name);
			return PTR_ERR(pw->mclk);
		}

		if (pdata->parentclk_name) {
			parent = devm_clk_get(dev, pdata->parentclk_name);
			if (IS_ERR(parent)) {
				dev_err(dev, "unable to get parent clock %s",
					pdata->parentclk_name);
			} else
				clk_set_parent(pw->mclk, parent);
		}
	}

	/* analog 2.8v */
	if (pdata->regulators.avdd)
		err |= camera_common_regulator_get(dev, &pw->avdd,
						   pdata->regulators.avdd);
	/* IO 1.8v */
	if (pdata->regulators.iovdd)
		err |= camera_common_regulator_get(dev, &pw->iovdd,
						   pdata->regulators.iovdd);
	/* dig 1.2v */
	if (pdata->regulators.dvdd)
		err |= camera_common_regulator_get(dev, &pw->dvdd,
						   pdata->regulators.dvdd);
	if (err) {
		dev_err(dev, "%s: unable to get regulator(s)\n", __func__);
		goto done;
	}

	/* Reset or ENABLE GPIO */
	pw->reset_gpio = pdata->reset_gpio;
	err = gpio_request(pw->reset_gpio, "cam_reset_gpio");
	if (err < 0) {
		dev_err(dev, "%s: unable to request reset_gpio (%d)\n",
			__func__, err);
		goto done;
	}

done:
	pw->state = SWITCH_OFF;

	return err;
}

static struct camera_common_pdata *
imx462_parse_dt(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	struct camera_common_pdata *ret = NULL;
	int err = 0;
	int gpio;

	if (!np)
		return NULL;

	match = of_match_device(imx462_of_match, dev);
	if (!match) {
		dev_err(dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata =
		devm_kzalloc(dev, sizeof(*board_priv_pdata), GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio < 0) {
		if (gpio == -EPROBE_DEFER)
			ret = ERR_PTR(-EPROBE_DEFER);
		dev_err(dev, "reset-gpios not found\n");
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	err = of_property_read_string(np, "mclk", &board_priv_pdata->mclk_name);
	if (err)
		dev_dbg(dev,
			"mclk name not present, assume sensor driven externally\n");

	err = of_property_read_string(np, "avdd-reg",
				      &board_priv_pdata->regulators.avdd);
	err |= of_property_read_string(np, "iovdd-reg",
				       &board_priv_pdata->regulators.iovdd);
	err |= of_property_read_string(np, "dvdd-reg",
				       &board_priv_pdata->regulators.dvdd);
	if (err)
		dev_dbg(dev,
			"avdd, iovdd and/or dvdd reglrs. not present, assume sensor powered independently\n");

	board_priv_pdata->has_eeprom = of_property_read_bool(np, "has-eeprom");

	return board_priv_pdata;

error:
	devm_kfree(dev, board_priv_pdata);

	return ret;
}

static int imx462_set_mode(struct tegracam_device *tc_dev)
{
	struct imx462 *priv = (struct imx462 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	unsigned int mode_index = 0;
	int err = 0;
	const char *config;
	struct device_node *mode;
	uint offset = ARRAY_SIZE(imx462_frmfmt);

	dev_dbg(tc_dev->dev, "%s:\n", __func__);
	mode = of_get_child_by_name(tc_dev->dev->of_node, "mode0");
	err = of_property_read_string(mode, "num_lanes", &config);

	if (config[0] == '4') {
		priv->config = FOUR_LANE_CONFIG;
		dev_dbg(tc_dev->dev, "Using 4-lane configuration\n");
	} else if (config[0] == '2') {
		priv->config = TWO_LANE_CONFIG;
		dev_dbg(tc_dev->dev, "Using 2-lane configuration\n");
	} else {
		dev_err(tc_dev->dev, "Unsupported config\n");
		return -EINVAL;
	}

	err = imx462_write_table(priv, mode_table[IMX462_MODE_COMMON]);
	if (err)
		return err;

	mode_index = s_data->mode;
	if (priv->config == FOUR_LANE_CONFIG)
		err = imx462_write_table(priv, mode_table[mode_index + offset]);
	else {
		dev_dbg(tc_dev->dev, "Writing mode table %d\n", mode_index);
		err = imx462_write_table(priv, mode_table[mode_index]);
	}
	if (err)
		return err;

	err = imx462_write_reg(s_data, IMX462_FR_FDG_SEL,
			       IMX462_FRSEL_60FPS |
				       (hcg_mode ? IMX462_FDG_HCG :
						   IMX462_FDG_LCG));
	if (err)
		return err;

	dev_dbg(tc_dev->dev, "hcg_mode: %d\n", hcg_mode);

	priv->frame_length = IMX462_MIN_FRAME_LENGTH;
	priv->exposure_us = 0;

	return 0;
}

static int imx462_start_streaming(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct imx462 *priv = (struct imx462 *)tegracam_get_privdata(tc_dev);
	int err = 0;

	dev_dbg(tc_dev->dev, "%s:\n", __func__);

	if (test_mode) {
		dev_dbg(tc_dev->dev, "Test mode %d\n", test_mode);

		err = imx462_write_table(priv,
					 mode_table[IMX462_MODE_TEST_PATTERN]);
		if (err)
			return err;

		err = imx462_write_reg(s_data, IMX462_PGCTRL,
				       IMX462_PGCTRL_REGEN |
					       IMX462_PGCTRL_THRU |
					       IMX462_PGCTRL_MODE(test_mode));
		if (err)
			return err;
	}

	return imx462_write_table(priv, mode_table[IMX462_START_STREAM]);
}

static int imx462_stop_streaming(struct tegracam_device *tc_dev)
{
	int err;
	struct imx462 *priv = (struct imx462 *)tegracam_get_privdata(tc_dev);

	dev_dbg(tc_dev->dev, "%s:\n", __func__);
	err = imx462_write_table(priv, mode_table[IMX462_STOP_STREAM]);

	return err;
}

static struct camera_common_sensor_ops imx462_common_ops = {
	.numfrmfmts = ARRAY_SIZE(imx462_frmfmt),
	.frmfmt_table = imx462_frmfmt,
	.power_on = imx462_power_on,
	.power_off = imx462_power_off,
	.write_reg = imx462_write_reg,
	.read_reg = imx462_read_reg,
	.parse_dt = imx462_parse_dt,
	.power_get = imx462_power_get,
	.power_put = imx462_power_put,
	.set_mode = imx462_set_mode,
	.start_streaming = imx462_start_streaming,
	.stop_streaming = imx462_stop_streaming,
};

static int imx462_board_setup(struct imx462 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct device *dev = s_data->dev;
	u8 reg_val[2];
	int err = 0;

	/* Skip mclk enable as this camera has an internal oscillator */

	err = imx462_power_on(s_data);
	if (err) {
		dev_err(dev, "error during power on sensor (%d)\n", err);
		goto done;
	}

	/* Probe sensor model id registers */
	err = imx462_read_reg(s_data, IMX462_MODEL_ID_ADDR_MSB, &reg_val[0]);
	if (err) {
		dev_err(dev, "%s: error during i2c read probe (%d)\n", __func__,
			err);
		goto err_reg_probe;
	}
	err = imx462_read_reg(s_data, IMX462_MODEL_ID_ADDR_LSB, &reg_val[1]);
	if (err) {
		dev_err(dev, "%s: error during i2c read probe (%d)\n", __func__,
			err);
		goto err_reg_probe;
	}

	dev_dbg(dev, "%s: sensor model id: 0x%x%x\n", __func__, reg_val[0],
		reg_val[1]);

	if (!((reg_val[0] == 0x10) && reg_val[1] == 0xA0))
		dev_err(dev, "%s: invalid sensor model id: %x%x\n", __func__,
			reg_val[0], reg_val[1]);

err_reg_probe:
	imx462_power_off(s_data);

done:
	return err;
}

static int imx462_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_dbg(&client->dev, "%s:\n", __func__);

	return 0;
}

static const struct v4l2_subdev_internal_ops imx462_subdev_internal_ops = {
	.open = imx462_open,
};

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int imx462_probe(struct i2c_client *client)
#else
static int imx462_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct imx462 *priv;
	int err;

	dev_dbg(dev, "probing v4l2 sensor at addr 0x%0x\n", client->addr);

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(struct imx462), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	priv->i2c_client = tc_dev->client = client;
	tc_dev->dev = dev;
	strscpy(tc_dev->name, "imx462", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &sensor_regmap_config;
	tc_dev->sensor_ops = &imx462_common_ops;
	tc_dev->v4l2sd_internal_ops = &imx462_subdev_internal_ops;
	tc_dev->tcctrl_ops = &imx462_ctrl_ops;

	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	priv->subdev = &tc_dev->s_data->subdev;
	tegracam_set_privdata(tc_dev, (void *)priv);

	err = imx462_board_setup(priv);
	if (err) {
		dev_err(dev, "board setup failed\n");
		return err;
	}

	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "tegra camera subdev registration failed\n");
		return err;
	}

	dev_dbg(dev, "detected imx462 sensor\n");

	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int imx462_remove(struct i2c_client *client)
#else
static void imx462_remove(struct i2c_client *client)
#endif
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct imx462 *priv;

	if (!s_data) {
		dev_err(&client->dev, "camera common data is NULL\n");
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
		return -EINVAL;
#else
		return;
#endif
	}
	priv = (struct imx462 *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
	return 0;
#endif
}

static const struct i2c_device_id imx462_id[] = { { "imx462", 0 }, {} };

MODULE_DEVICE_TABLE(i2c, imx462_id);

static struct i2c_driver imx462_i2c_driver = {
	.driver = {
		   .name = "imx462",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(imx462_of_match),
		   },
	.probe = imx462_probe,
	.remove = imx462_remove,
	.id_table = imx462_id,
};

module_i2c_driver(imx462_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for Sony IMX462");
MODULE_AUTHOR("UAB Kurokesu");
MODULE_LICENSE("GPL v2");
