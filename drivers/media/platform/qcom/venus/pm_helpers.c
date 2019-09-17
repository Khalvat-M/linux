// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Linaro Ltd.
 *
 * Author: Stanimir Varbanov <stanimir.varbanov@linaro.org>
 */
#include <linux/clk.h>
#include <linux/types.h>
#include <linux/iopoll.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "core.h"
#include "hfi_venus_io.h"
#include "pm_helpers.h"

static int core_clks_get(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	struct device *dev = core->dev;
	unsigned int i;

	for (i = 0; i < res->clks_num; i++) {
		core->clks[i] = devm_clk_get(dev, res->clks[i]);
		if (IS_ERR(core->clks[i]))
			return PTR_ERR(core->clks[i]);
	}

	return 0;
}

static int core_clks_enable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i;
	int ret;

	for (i = 0; i < res->clks_num; i++) {
		ret = clk_prepare_enable(core->clks[i]);
		if (ret)
			goto err;
	}

	return 0;
err:
	while (i--)
		clk_disable_unprepare(core->clks[i]);

	return ret;
}

static void core_clks_disable(struct venus_core *core)
{
	const struct venus_resources *res = core->res;
	unsigned int i = res->clks_num;

	while (i--)
		clk_disable_unprepare(core->clks[i]);
}

static void
vcodec_power_control_v3(struct venus_core *core, u32 session_type, bool enable)
{
	void __iomem *ctrl;

	if (session_type == VIDC_SESSION_TYPE_DEC)
		ctrl = core->base + WRAPPER_VDEC_VCODEC_POWER_CONTROL;
	else
		ctrl = core->base + WRAPPER_VENC_VCODEC_POWER_CONTROL;

	if (enable)
		writel(0, ctrl);
	else
		writel(1, ctrl);
}

static int
vcodec_power_control_v4(struct venus_core *core, u32 coreid, bool enable)
{
	void __iomem *ctrl, *stat;
	u32 val;
	int ret;

	if (coreid == VIDC_CORE_ID_1) {
		ctrl = core->base + WRAPPER_VCODEC0_MMCC_POWER_CONTROL;
		stat = core->base + WRAPPER_VCODEC0_MMCC_POWER_STATUS;
	} else {
		ctrl = core->base + WRAPPER_VCODEC1_MMCC_POWER_CONTROL;
		stat = core->base + WRAPPER_VCODEC1_MMCC_POWER_STATUS;
	}

	if (enable) {
		writel(0, ctrl);

		ret = readl_poll_timeout(stat, val, val & BIT(1), 1, 100);
		if (ret)
			return ret;
	} else {
		writel(1, ctrl);

		ret = readl_poll_timeout(stat, val, !(val & BIT(1)), 1, 100);
		if (ret)
			return ret;
	}

	return 0;
}

static int poweroff_by_core_id(struct venus_core *core, struct device *dev,
			       unsigned int coreid_mask)
{
	int ret;

	if (coreid_mask & VIDC_CORE_ID_1) {
		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_1, true);
		if (ret)
			return ret;

		clk_disable_unprepare(core->core0_bus_clk);
		clk_disable_unprepare(core->core0_clk);

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_1, false);
		if (ret)
			dev_err(dev, "%s: power off vcodec0 failed %d\n",
				__func__, ret);

		ret = pm_runtime_put_sync(core->pd_core0);
		if (ret < 0)
			return ret;
	}

	if (coreid_mask & VIDC_CORE_ID_2) {
		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_2, true);
		if (ret)
			return ret;

		clk_disable_unprepare(core->core1_bus_clk);
		clk_disable_unprepare(core->core1_clk);

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_2, false);
		if (ret)
			dev_err(dev, "%s: power off vcodec1 failed %d\n",
				__func__, ret);

		ret = pm_runtime_put_sync(core->pd_core1);
	}

	return ret;
}

static int poweron_by_core_id(struct venus_core *core, struct device *dev,
			      unsigned int coreid_mask)
{
	int ret;

	if (coreid_mask & VIDC_CORE_ID_1) {
		ret = pm_runtime_get_sync(core->pd_core0);
		if (ret < 0)
			return ret;

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_1, true);
		if (ret)
			return ret;

		ret = clk_prepare_enable(core->core0_clk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(core->core0_bus_clk);
		if (ret)
			return ret;

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_1, false);
		if (ret < 0)
			return ret;
	}

	if (coreid_mask & VIDC_CORE_ID_2) {
		ret = pm_runtime_get_sync(core->pd_core1);
		if (ret < 0)
			return ret;

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_2, true);
		if (ret)
			return ret;

		ret = clk_prepare_enable(core->core1_clk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(core->core1_bus_clk);
		if (ret)
			return ret;

		ret = vcodec_power_control_v4(core, VIDC_CORE_ID_2, false);
	}

	return ret;
}

static int core_get_pm_v1(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);

	return core_clks_get(core);
}

static int core_power_v1(struct device *dev, int on)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret = 0;

	if (on == POWER_ON)
		ret = core_clks_enable(core);
	else
		core_clks_disable(core);

	return ret;
}

static const struct venus_pm_ops venus_pm_ops_v1 = {
	.core_get_pm = core_get_pm_v1,
	.core_power = core_power_v1,
};

static int vdec_get_pm_v3(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);

	core->core0_clk = devm_clk_get(dev, "core");
	if (IS_ERR(core->core0_clk))
		return PTR_ERR(core->core0_clk);

	return 0;
}

static int vdec_power_v3(struct device *dev, int on)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret = 0;

	if (on == POWER_ON) {
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_DEC, true);
		ret = clk_prepare_enable(core->core0_clk);
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_DEC, false);
	} else {
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_DEC, true);
		clk_disable_unprepare(core->core0_clk);
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_DEC, false);
	}

	return ret;
}

static int venc_get_pm_v3(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);

	core->core1_clk = devm_clk_get(dev, "core");
	if (IS_ERR(core->core1_clk))
		return PTR_ERR(core->core1_clk);

	return 0;
}

static int venc_power_v3(struct device *dev, int on)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret = 0;

	if (on == POWER_ON) {
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_ENC, true);
		ret = clk_prepare_enable(core->core1_clk);
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_ENC, false);
	} else {
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_ENC, true);
		clk_disable_unprepare(core->core1_clk);
		vcodec_power_control_v3(core, VIDC_SESSION_TYPE_ENC, false);
	}

	return ret;
}

static const struct venus_pm_ops venus_pm_ops_v3 = {
	.core_get_pm = core_get_pm_v1,
	.core_power = core_power_v1,
	.vdec_get_pm = vdec_get_pm_v3,
	.vdec_power = vdec_power_v3,
	.venc_get_pm = venc_get_pm_v3,
	.venc_power = venc_power_v3,
};

static int core_get_pm_v4(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	ret = core_clks_get(core);
	if (ret)
		return ret;

	core->core0_clk = devm_clk_get(dev, "vcodec0_core");
	if (IS_ERR(core->core0_clk))
		return PTR_ERR(core->core0_clk);

	core->core0_bus_clk = devm_clk_get(dev, "vcodec0_bus");
	if (IS_ERR(core->core0_bus_clk))
		return PTR_ERR(core->core0_bus_clk);

	core->core1_clk = devm_clk_get(dev, "vcodec1_core");
	if (IS_ERR(core->core1_clk))
		return PTR_ERR(core->core1_clk);

	core->core1_bus_clk = devm_clk_get(dev, "vcodec1_bus");
	if (IS_ERR(core->core1_bus_clk))
		return PTR_ERR(core->core1_bus_clk);

	core->pd_core = dev_pm_domain_attach_by_name(dev, "venus");
	if (IS_ERR(core->pd_core))
		return PTR_ERR(core->pd_core);

	core->pd_core0 = dev_pm_domain_attach_by_name(dev, "vcodec0");
	if (IS_ERR(core->pd_core0))
		return PTR_ERR(core->pd_core0);

	core->pd_core1 = dev_pm_domain_attach_by_name(dev, "vcodec1");
	if (IS_ERR(core->pd_core1))
		return PTR_ERR(core->pd_core1);

	core->pd_dl_venus = device_link_add(dev, core->pd_core,
					    DL_FLAG_PM_RUNTIME |
					    DL_FLAG_STATELESS |
					    DL_FLAG_RPM_ACTIVE);
	if (!core->pd_dl_venus) {
		dev_err(dev, "adding venus device link failed!\n");
		return -ENODEV;
	}

	return 0;
}

static void core_put_pm_v4(struct device *dev)
{
	struct venus_core *core = dev_get_drvdata(dev);

	if (core->pd_dl_venus)
		device_link_del(core->pd_dl_venus);

	if (!IS_ERR_OR_NULL(core->pd_core))
		dev_pm_domain_detach(core->pd_core, true);

	if (!IS_ERR_OR_NULL(core->pd_core0))
		dev_pm_domain_detach(core->pd_core0, true);

	if (!IS_ERR_OR_NULL(core->pd_core1))
		dev_pm_domain_detach(core->pd_core1, true);
}

static int core_power_v4(struct device *dev, int on)
{
	struct venus_core *core = dev_get_drvdata(dev);
	int ret;

	if (on == POWER_ON) {
		ret = core_clks_enable(core);
		if (ret) {
			dev_err(dev, "core clocks enable failed (%d)\n", ret);
			return ret;
		}

		ret = poweron_by_core_id(core, dev, VIDC_CORE_ID_1 |
						    VIDC_CORE_ID_2);
	} else {
		ret = poweroff_by_core_id(core, dev, VIDC_CORE_ID_1 |
						     VIDC_CORE_ID_2);
		if (ret)
			dev_err(dev, "poweroff by core failed (%d)\n", ret);

		core_clks_disable(core);
	}

	return ret;
}

static const struct venus_pm_ops venus_pm_ops_v4 = {
	.core_get_pm = core_get_pm_v4,
	.core_put_pm = core_put_pm_v4,
	.core_power = core_power_v4,
};

const struct venus_pm_ops *venus_get_pm_ops(enum hfi_version version)
{
	switch (version) {
	case HFI_VERSION_1XX:
	default:
		return &venus_pm_ops_v1;
	case HFI_VERSION_3XX:
		return &venus_pm_ops_v3;
	case HFI_VERSION_4XX:
		return &venus_pm_ops_v4;
	}

	return NULL;
}
