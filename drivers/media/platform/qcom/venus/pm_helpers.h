/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019 Linaro Ltd. */
#ifndef __VENUS_PM_HELPERS_H__
#define __VENUS_PM_HELPERS_H__

struct device;

#define POWER_ON	1
#define POWER_OFF	0

struct venus_pm_ops {
	int (*core_get_pm)(struct device *dev);
	void (*core_put_pm)(struct device *dev);
	int (*core_power)(struct device *dev, int on);

	int (*vdec_get_pm)(struct device *dev);
	int (*vdec_power)(struct device *dev, int on);

	int (*venc_get_pm)(struct device *dev);
	int (*venc_power)(struct device *dev, int on);
};

const struct venus_pm_ops *venus_get_pm_ops(enum hfi_version version);

#endif
