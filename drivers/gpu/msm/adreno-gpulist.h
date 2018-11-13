/* Copyright (c) 2002,2007-2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define ANY_ID (~0)

static const struct adreno_gpu_core adreno_gpulist[] = {
	{
		.gpurev = ADRENO_REV_A630,
		.core = 6,
		.major = 3,
		.minor = 0,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_IFPC |
			ADRENO_GPMU | ADRENO_CONTENT_PROTECTION |
			ADRENO_IOCOHERENT | ADRENO_PREEMPTION,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a630_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_1M,
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a630_gmu.bin",
		.gpmu_major = 0x1,
		.gpmu_minor = 0x003,
		.gpmu_tsens = 0x000C000D,
		.max_power = 5448,
	},
	{
		.gpurev = ADRENO_REV_A615,
		.core = 6,
		.major = 1,
		.minor = 5,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_PREEMPTION |
			ADRENO_GPMU | ADRENO_CONTENT_PROTECTION | ADRENO_IFPC |
			ADRENO_IOCOHERENT,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a615_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_512K,
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a630_gmu.bin",
		.gpmu_major = 0x1,
		.gpmu_minor = 0x003,
	},
	{
		.gpurev = ADRENO_REV_A618,
		.core = 6,
		.major = 1,
		.minor = 8,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_PREEMPTION |
			ADRENO_GPMU | ADRENO_CONTENT_PROTECTION | ADRENO_IFPC |
			ADRENO_IOCOHERENT,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a615_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_512K,
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a618_gmu.bin",
		.gpmu_major = 0x1,
		.gpmu_minor = 0x008,
	},
	{
		.gpurev = ADRENO_REV_A640,
		.core = 6,
		.major = 4,
		.minor = 0,
		.patchid = 0,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_GPMU |
			ADRENO_CONTENT_PROTECTION | ADRENO_IOCOHERENT |
			ADRENO_IFPC,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a640_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_1M, //Verified 1MB
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a640_gmu.bin",
		.gpmu_major = 0x2,
		.gpmu_minor = 0x000,
		.gpmu_tsens = 0x000C000D,
		.max_power = 5448,
		.va_padding = SZ_64K,
	},
	{
		.gpurev = ADRENO_REV_A640,
		.core = 6,
		.major = 4,
		.minor = 0,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_GPMU |
			ADRENO_CONTENT_PROTECTION | ADRENO_IOCOHERENT |
			ADRENO_IFPC | ADRENO_PREEMPTION | ADRENO_ACD |
			ADRENO_LM,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a640_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_1M, //Verified 1MB
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a640_gmu.bin",
		.gpmu_major = 0x2,
		.gpmu_minor = 0x000,
		.gpmu_tsens = 0x000C000D,
		.max_power = 5448,
	},
	{
		.gpurev = ADRENO_REV_A680,
		.core = 6,
		.major = 8,
		.minor = 0,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_GPMU |
			ADRENO_CONTENT_PROTECTION | ADRENO_IOCOHERENT |
			ADRENO_IFPC | ADRENO_PREEMPTION,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a640_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_2M,
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a640_gmu.bin",
		.gpmu_major = 0x2,
		.gpmu_minor = 0x000,
		.gpmu_tsens = 0x000C000D,
		.max_power = 5448,
	},
	{
		.gpurev = ADRENO_REV_A612,
		.core = 6,
		.major = 1,
		.minor = 2,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_CONTENT_PROTECTION |
			ADRENO_IOCOHERENT | ADRENO_PREEMPTION | ADRENO_GPMU |
			ADRENO_IFPC | ADRENO_PERFCTRL_RETAIN,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a612_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = (SZ_128K + SZ_4K),
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a612_rgmu.bin",
		.cx_ipeak_gpu_freq = 745000000,
	},
	{
		.gpurev = ADRENO_REV_A616,
		.core = 6,
		.major = 1,
		.minor = 6,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_RPMH | ADRENO_PREEMPTION |
			ADRENO_GPMU | ADRENO_CONTENT_PROTECTION | ADRENO_IFPC |
			ADRENO_IOCOHERENT,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a615_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = SZ_512K,
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.gpmufw_name = "a630_gmu.bin",
		.gpmu_major = 0x1,
		.gpmu_minor = 0x003,
	},
	{
		.gpurev = ADRENO_REV_A610,
		.core = 6,
		.major = 1,
		.minor = 0,
		.patchid = ANY_ID,
		.features = ADRENO_64BIT | ADRENO_PREEMPTION |
			ADRENO_CONTENT_PROTECTION,
		.sqefw_name = "a630_sqe.fw",
		.zap_name = "a610_zap",
		.gpudev = &adreno_a6xx_gpudev,
		.gmem_size = (SZ_128K + SZ_4K),
		.num_protected_regs = 0x20,
		.busy_mask = 0xFFFFFFFE,
		.cx_ipeak_gpu_freq = 900000000,
	},
};
