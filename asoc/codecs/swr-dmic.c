// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/debugfs.h>
#include <soc/soundwire.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <asoc/msm-cdc-pinctrl.h>
#include <asoc/msm-cdc-supply.h>
#include <dt-bindings/sound/audio-codec-port-types.h>
#include "wcd938x/wcd938x.h"
#include "swr-dmic.h"

static int swr_master_channel_map[] = {
	ZERO,
	SWRM_TX1_CH1,
	SWRM_TX1_CH2,
	SWRM_TX1_CH3,
	SWRM_TX1_CH4,
	SWRM_TX2_CH1,
	SWRM_TX2_CH2,
	SWRM_TX2_CH3,
	SWRM_TX2_CH4,
	SWRM_TX3_CH1,
	SWRM_TX3_CH2,
	SWRM_TX3_CH3,
	SWRM_TX3_CH4,
	SWRM_PCM_IN,
};

/*
 * Private data Structure for swr-dmic. All parameters related to
 * external mic codec needs to be defined here.
 */
struct swr_dmic_priv {
	struct device *dev;
	struct swr_device *swr_slave;
	struct snd_soc_component *component;
	struct snd_soc_component_driver *driver;
	struct snd_soc_component *supply_component;
	u32 micb_num;
	struct device_node *wcd_handle;
	bool is_wcd_supply;
	int is_en_supply;
	int port_type;
	u8 tx_master_port_map[SWR_DMIC_MAX_PORTS];
};

const char *codec_name_list[] = {
	"swr-dmic-01",
	"swr-dmic-02",
	"swr-dmic-03",
	"swr-dmic-04",
};

const char *dai_name_list[] = {
	"swr_dmic_tx0",
	"swr_dmic_tx1",
	"swr_dmic_tx2",
	"swr_dmic_tx3",
};

const char *aif_name_list[] = {
	"SWR_DMIC_AIF0 Playback",
	"SWR_DMIC_AIF1 Playback",
	"SWR_DMIC_AIF2 Playback",
	"SWR_DMIC_AIF3 Playback",
};

static int swr_dmic_reset(struct swr_device *pdev);
static int swr_dmic_up(struct swr_device *pdev);
static int swr_dmic_down(struct swr_device *pdev);

static inline int swr_dmic_tx_get_slave_port_type_idx(const char *wname,
				      unsigned int *port_idx)
{
	u8 port_type;

	if (strnstr(wname, "HIFI", strlen(wname)))
		port_type = SWR_DMIC_HIFI_PORT;
	else if (strnstr(wname, "LP", strlen(wname)))
		port_type = SWR_DMIC_LP_PORT;
	else
		return -EINVAL;

	*port_idx = port_type;
	return 0;
}

static inline int swr_dmic_get_master_port_val(int port)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(swr_master_channel_map); i++)
		if (port == swr_master_channel_map[i])
			return i;
	return 0;
}

static int swr_dmic_tx_master_port_get(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
				snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic = snd_soc_component_get_drvdata(component);
	int ret = 0;
	int slave_port_idx;

	ret = swr_dmic_tx_get_slave_port_type_idx(kcontrol->id.name,
							&slave_port_idx);
	if (ret) {
		dev_dbg(component->dev, "%s: invalid port string\n", __func__);
		return ret;
	}
	swr_dmic->port_type = slave_port_idx;

	ucontrol->value.integer.value[0] =
			swr_dmic_get_master_port_val(
				swr_dmic->tx_master_port_map[slave_port_idx]);

	dev_dbg(component->dev, "%s: ucontrol->value.integer.value[0] = %ld\n",
			__func__, ucontrol->value.integer.value[0]);

	return 0;
}

static int swr_dmic_tx_master_port_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
				snd_soc_kcontrol_component(kcontrol);
	struct swr_dmic_priv *swr_dmic = snd_soc_component_get_drvdata(component);
	int ret = 0;
	int slave_port_idx;

	ret  = swr_dmic_tx_get_slave_port_type_idx(kcontrol->id.name,
							&slave_port_idx);
	if (ret) {
		dev_dbg(component->dev, "%s: invalid port string\n", __func__);
		return ret;
	}
	swr_dmic->port_type = slave_port_idx;

	swr_dmic->tx_master_port_map[slave_port_idx] =
		swr_master_channel_map[ucontrol->value.enumerated.item[0]];
	dev_dbg(component->dev, "%s: slv port id: %d, master_port_type: %d\n",
		__func__, slave_port_idx,
		swr_dmic->tx_master_port_map[slave_port_idx]);

	return 0;
}

static int swr_dmic_port_enable(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	int ret = 0;
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	u8 ch_mask = 0x01; // only DpnChannelEN1 register is available
	u8 num_port = 1;
	u8 port_id = swr_dmic->port_type;
	u8 port_type = swr_dmic->tx_master_port_map[port_id];

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = swr_slvdev_datapath_control(swr_dmic->swr_slave,
			swr_dmic->swr_slave->dev_num, true);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		ret = swr_disconnect_port(swr_dmic->swr_slave,
				&port_id, num_port, &ch_mask, &port_type);
		break;
	};

	return ret;
}

static int dmic_swr_ctrl(struct snd_soc_dapm_widget *w,
			 struct snd_kcontrol *kcontrol, int event)
{
	int ret = 0;
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	u8 num_ch = 1;
	u8 ch_mask = 0x01; // only DpnChannelEN1 register is available
	u32 ch_rate = SWR_CLK_RATE_4P8MHZ;
	u8 num_port = 1;
	u8 port_type = 0;
	u8 port_id = swr_dmic->port_type;

	/*
	 * Port 1 is high quality / 2.4 or 3.072 Mbps
	 * Port 2 is listen low power / 0.6 or 0.768 Mbps
	 */
	if(swr_dmic->port_type == SWR_DMIC_HIFI_PORT)
		ch_rate = SWR_CLK_RATE_2P4MHZ;
	else
		ch_rate = SWR_CLK_RATE_0P6MHZ;

	port_type = swr_dmic->tx_master_port_map[port_id];

	dev_dbg(component->dev, "%s port_type: %d event: %d\n", __func__,
		port_type, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = swr_connect_port(swr_dmic->swr_slave, &port_id,
					num_port, &ch_mask, &ch_rate,
					&num_ch, &port_type);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = swr_slvdev_datapath_control(swr_dmic->swr_slave,
			swr_dmic->swr_slave->dev_num, false);
		break;
	};

	return ret;
}

static int swr_dmic_enable_supply(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol,
			       int event)
{
	struct snd_soc_component *component =
			snd_soc_dapm_to_component(w->dapm);
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);
	int ret = 0;

	dev_dbg(component->dev, "%s wname: %s event: %d\n", __func__,
		w->name, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = swr_dmic_up(swr_dmic->swr_slave);
		break;
	case SND_SOC_DAPM_POST_PMU:
		ret = swr_dmic_reset(swr_dmic->swr_slave);
		break;
	case SND_SOC_DAPM_POST_PMD:
		ret = swr_dmic_down(swr_dmic->swr_slave);
		break;
	}

	if (ret)
		dev_dbg(component->dev, "%s wname: %s event: %d ret : %d\n",
			__func__, w->name, event, ret);

	return ret;
}

static const char * const tx_master_port_text[] = {
	"ZERO", "SWRM_TX1_CH1", "SWRM_TX1_CH2", "SWRM_TX1_CH3", "SWRM_TX1_CH4",
	"SWRM_TX2_CH1", "SWRM_TX2_CH2", "SWRM_TX2_CH3", "SWRM_TX2_CH4",
	"SWRM_TX3_CH1", "SWRM_TX3_CH2", "SWRM_TX3_CH3", "SWRM_TX3_CH4",
	"SWRM_PCM_IN",
};

static const struct soc_enum tx_master_port_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tx_master_port_text),
				tx_master_port_text);

static const struct snd_kcontrol_new swr_dmic_snd_controls[] = {
	SOC_ENUM_EXT("HIFI PortMap", tx_master_port_enum,
		swr_dmic_tx_master_port_get, swr_dmic_tx_master_port_put),
	SOC_ENUM_EXT("LP PortMap", tx_master_port_enum,
		swr_dmic_tx_master_port_get, swr_dmic_tx_master_port_put),
};

static const struct snd_kcontrol_new dmic_switch[] = {
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_soc_dapm_widget swr_dmic_dapm_widgets[] = {
	SND_SOC_DAPM_MIXER_E("SWR_DMIC_MIXER", SND_SOC_NOPM, 0, 0,
			dmic_switch, ARRAY_SIZE(dmic_switch), dmic_swr_ctrl,
			SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_INPUT("SWR_DMIC"),

	SND_SOC_DAPM_SUPPLY_S("SMIC_SUPPLY", 1, SND_SOC_NOPM, 0, 0,
				swr_dmic_enable_supply,
				SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
				SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("SMIC_PORT_EN", SND_SOC_NOPM, 0, 0, NULL, 0,
				swr_dmic_port_enable,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("SWR_DMIC_OUTPUT"),
};

static const struct snd_soc_dapm_route swr_dmic_audio_map[] = {
	{"SWR_DMIC", NULL, "SMIC_SUPPLY"},
	{"SWR_DMIC_MIXER", "Switch", "SWR_DMIC"},
	{"SMIC_PORT_EN", NULL, "SWR_DMIC_MIXER"},
	{"SWR_DMIC_OUTPUT", NULL, "SMIC_PORT_EN"},
};

static int swr_dmic_codec_probe(struct snd_soc_component *component)
{
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	if (!swr_dmic)
		return -EINVAL;

	swr_dmic->component = component;
	return 0;
}

static void swr_dmic_codec_remove(struct snd_soc_component *component)
{
	struct swr_dmic_priv *swr_dmic =
			snd_soc_component_get_drvdata(component);

	swr_dmic->component = NULL;
	return;
}

static const struct snd_soc_component_driver soc_codec_dev_swr_dmic = {
	.name = NULL,
	.probe = swr_dmic_codec_probe,
	.remove = swr_dmic_codec_remove,
	.controls = swr_dmic_snd_controls,
	.num_controls = ARRAY_SIZE(swr_dmic_snd_controls),
	.dapm_widgets = swr_dmic_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(swr_dmic_dapm_widgets),
	.dapm_routes = swr_dmic_audio_map,
	.num_dapm_routes = ARRAY_SIZE(swr_dmic_audio_map),
};

static int enable_wcd_codec_supply(struct swr_dmic_priv *swr_dmic, bool enable)
{
	int rc = 0;
	int micb_num = swr_dmic->micb_num;
	struct snd_soc_component *component = swr_dmic->supply_component;

	if (!component) {
		pr_err("%s: component is NULL\n", __func__);
		return -EINVAL;
	}

	if (enable)
		rc = wcd938x_codec_force_enable_micbias_v2(component,
					SND_SOC_DAPM_PRE_PMU, micb_num);
	else
		rc = wcd938x_codec_force_enable_micbias_v2(component,
					SND_SOC_DAPM_POST_PMD, micb_num);

	return rc;
}

static int swr_dmic_parse_supply(struct device_node *np,
				struct swr_dmic_priv *swr_dmic)
{
	struct platform_device *pdev = NULL;

	if (!np || !swr_dmic)
		return -EINVAL;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return -EINVAL;

	swr_dmic->supply_component = snd_soc_lookup_component(&pdev->dev, NULL);

	return 0;
}

static int swr_dmic_probe(struct swr_device *pdev)
{
	int ret = 0;
	int i = 0;
	u8 swr_devnum = 0;
	int dev_index = -1;
	char* prefix_name = NULL;
	struct swr_dmic_priv *swr_dmic = NULL;
	const char *swr_dmic_name_prefix_of = NULL;
	const char *swr_dmic_codec_name_of = NULL;
	struct snd_soc_component *component = NULL;

	swr_dmic = devm_kzalloc(&pdev->dev, sizeof(struct swr_dmic_priv),
			    GFP_KERNEL);
	if (!swr_dmic)
		return -ENOMEM;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,swr-dmic-supply",
				&swr_dmic->micb_num);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: Looking up %s property in node %s failed\n",
		__func__, "qcom,swr-dmic-supply",
		pdev->dev.of_node->full_name);
		goto err;
	}
	swr_dmic->wcd_handle = of_parse_phandle(pdev->dev.of_node,
						"qcom,wcd-handle", 0);
	if (!swr_dmic->wcd_handle) {
		dev_dbg(&pdev->dev, "%s: no wcd handle listed\n",
			__func__);
		swr_dmic->is_wcd_supply = false;
	} else {
		swr_dmic_parse_supply(swr_dmic->wcd_handle, swr_dmic);
		swr_dmic->is_wcd_supply = true;
	}

	if (swr_dmic->is_wcd_supply) {
		ret = enable_wcd_codec_supply(swr_dmic, true);
		if (ret) {
			ret = -EPROBE_DEFER;
			goto err;
		}
		++swr_dmic->is_en_supply;
	}

	swr_set_dev_data(pdev, swr_dmic);

	swr_dmic->swr_slave = pdev;

	ret = of_property_read_string(pdev->dev.of_node, "qcom,swr-dmic-prefix",
				&swr_dmic_name_prefix_of);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: Looking up %s property in node %s failed\n",
		__func__, "qcom,swr-dmic-prefix",
		pdev->dev.of_node->full_name);
		goto dev_err;
	}

	ret = of_property_read_string(pdev->dev.of_node, "qcom,codec-name",
				&swr_dmic_codec_name_of);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: Looking up %s property in node %s failed\n",
		__func__, "qcom,codec-name",
		pdev->dev.of_node->full_name);
		goto dev_err;
	}

	/*
	 * Add 5msec delay to provide sufficient time for
	 * soundwire auto enumeration of slave devices as
	 * as per HW requirement.
	 */
	usleep_range(5000, 5010);
	ret = swr_get_logical_dev_num(pdev, pdev->addr, &swr_devnum);
	if (ret) {
		dev_dbg(&pdev->dev,
			"%s get devnum %d for dev addr %lx failed\n",
			__func__, swr_devnum, pdev->addr);
		ret = -EPROBE_DEFER;
		goto err;
	}
	pdev->dev_num = swr_devnum;


	swr_dmic->driver = devm_kzalloc(&pdev->dev,
			sizeof(struct snd_soc_component_driver), GFP_KERNEL);
	if (!swr_dmic->driver) {
		ret = -ENOMEM;
		goto dev_err;
	}

	memcpy(swr_dmic->driver, &soc_codec_dev_swr_dmic,
			sizeof(struct snd_soc_component_driver));

	for (i = 0; i < ARRAY_SIZE(codec_name_list); i++) {
		if (!strcmp(swr_dmic_codec_name_of, codec_name_list[i])) {
			dev_index = i;
			break;
		}
	}

	if (dev_index < 0) {
		ret = -EINVAL;
		goto dev_err;
	}

	swr_dmic->driver->name = dai_name_list[dev_index];

	ret = snd_soc_register_component(&pdev->dev, swr_dmic->driver,
				NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "%s: Codec registration failed\n",
			__func__);
		goto dev_err;
	}

	component = snd_soc_lookup_component(&pdev->dev,
						swr_dmic->driver->name);
	swr_dmic->component = component;
	prefix_name = devm_kzalloc(&pdev->dev,
					strlen(swr_dmic_name_prefix_of),
					GFP_KERNEL);
	if (!prefix_name) {
		ret = -ENOMEM;
		goto dev_err;
	}
	strlcpy(prefix_name, swr_dmic_name_prefix_of,
			strlen(swr_dmic_name_prefix_of));
	component->name_prefix = prefix_name;

	if (swr_dmic->is_en_supply == 1) {
		enable_wcd_codec_supply(swr_dmic, false);
		--swr_dmic->is_en_supply;
	}

	return 0;

dev_err:
	if (swr_dmic->is_en_supply == 1) {
		enable_wcd_codec_supply(swr_dmic, false);
		--swr_dmic->is_en_supply;
	}
	swr_dmic->is_wcd_supply = false;
	swr_dmic->wcd_handle = NULL;
	swr_remove_device(pdev);
err:
	return ret;
}

static int swr_dmic_remove(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	snd_soc_unregister_component(&pdev->dev);
	swr_set_dev_data(pdev, NULL);
	return 0;
}

static int swr_dmic_up(struct swr_device *pdev)
{
	int ret = 0;
	struct swr_dmic_priv *swr_dmic;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	++swr_dmic->is_en_supply;
	if (swr_dmic->is_en_supply == 1)
		ret = enable_wcd_codec_supply(swr_dmic, true);

	return ret;
}

static int swr_dmic_down(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;
	int ret = 0;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	--swr_dmic->is_en_supply;
	if (swr_dmic->is_en_supply < 0) {
		dev_warn(&pdev->dev, "%s: mismatch in supply count %d\n",
			__func__, swr_dmic->is_en_supply);
		swr_dmic->is_en_supply = 0;
		goto done;
	}
	if (!swr_dmic->is_en_supply)
		enable_wcd_codec_supply(swr_dmic, false);

done:
	return ret;
}

static int swr_dmic_reset(struct swr_device *pdev)
{
	struct swr_dmic_priv *swr_dmic;
	u8 retry = 5;
	u8 devnum = 0;

	swr_dmic = swr_get_dev_data(pdev);
	if (!swr_dmic) {
		dev_err(&pdev->dev, "%s: swr_dmic is NULL\n", __func__);
		return -EINVAL;
	}

	while (swr_get_logical_dev_num(pdev, pdev->addr, &devnum) && retry--) {
		/* Retry after 1 msec delay */
		usleep_range(1000, 1100);
	}
	pdev->dev_num = devnum;
	dev_dbg(&pdev->dev, "%s: devnum: %d\n", __func__, devnum);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int swr_dmic_suspend(struct device *dev)
{
	dev_dbg(dev, "%s: system suspend\n", __func__);
	return 0;
}

static int swr_dmic_resume(struct device *dev)
{
	struct swr_dmic_priv *swr_dmic = swr_get_dev_data(to_swr_device(dev));

	if (!swr_dmic) {
		dev_err(dev, "%s: swr_dmic private data is NULL\n", __func__);
		return -EINVAL;
	}
	dev_dbg(dev, "%s: system resume\n", __func__);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops swr_dmic_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(swr_dmic_suspend, swr_dmic_resume)
};

static const struct swr_device_id swr_dmic_id[] = {
	{"swr-dmic", 0},
	{}
};

static const struct of_device_id swr_dmic_dt_match[] = {
	{
		.compatible = "qcom,swr-dmic",
	},
	{}
};

static struct swr_driver swr_dmic_driver = {
	.driver = {
		.name = "swr-dmic",
		.owner = THIS_MODULE,
		.pm = &swr_dmic_pm_ops,
		.of_match_table = swr_dmic_dt_match,
	},
	.probe = swr_dmic_probe,
	.remove = swr_dmic_remove,
	.id_table = swr_dmic_id,
	.device_up = swr_dmic_up,
	.device_down = swr_dmic_down,
	.reset_device = swr_dmic_reset,
};

static int __init swr_dmic_init(void)
{
	return swr_driver_register(&swr_dmic_driver);
}

static void __exit swr_dmic_exit(void)
{
	swr_driver_unregister(&swr_dmic_driver);
}

module_init(swr_dmic_init);
module_exit(swr_dmic_exit);

MODULE_DESCRIPTION("SWR DMIC driver");
MODULE_LICENSE("GPL v2");