// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (C) 2019, Theobroma Systems Design und Consulting GmbH
 * Copyright (c) 2024, Rockchip, Inc. All rights reserved.
 */

#include <assert.h>
#include <common.h>
#include <drivers/rockchip_otp.h>
#include <io.h>
#include <kernel/panic.h>
#include <kernel/mutex.h>
#include <kernel/tee_common_otp.h>
#include <mm/core_memprot.h>
#include <platform.h>
#include <platform_config.h>
#include <rng_support.h>
#include <stdlib_ext.h>
#include <string.h>
#include <string_ext.h>
#include <utee_defines.h>

#define FIREWALL_DDR_RGN(i)		((i) * 0x4)
#define FIREWALL_DDR_CON		0xf0

#define RG_MAP_SECURE(top, base)	\
	(((((top) - 1) & 0x7fff) << 16) | ((base) & 0x7fff))

register_phys_mem_pgdir(MEM_AREA_IO_SEC, FIREWALL_DDR_BASE, FIREWALL_DDR_SIZE);
register_phys_mem_pgdir(MEM_AREA_IO_SEC, TRNG_S_BASE, TRNG_S_SIZE);
register_phys_mem_pgdir(MEM_AREA_IO_SEC, SYS_SGRF_BASE,  SYS_SGRF_SIZE);
register_phys_mem_pgdir(MEM_AREA_IO_SEC, PERICRU_BASE,  PERICRU_SIZE);

int platform_secure_ddr_region(int rgn, paddr_t st, size_t sz)
{
	vaddr_t fw_ddr_base = (vaddr_t)phys_to_virt_io(FIREWALL_DDR_BASE,
						       FIREWALL_DDR_SIZE);

	paddr_t ed = st + sz;
	uint32_t st_mb = st / SIZE_M(1);
	uint32_t ed_mb = ed / SIZE_M(1);

	if (!fw_ddr_base)
		panic();

	assert(rgn <= 16);
	assert(st < ed);

	/* Check aligned 1MB */
	assert(st % SIZE_M(1) == 0);
	assert(ed % SIZE_M(1) == 0);

	IMSG("protecting region %d: 0x%"PRIxPA"-0x%"PRIxPA"", rgn, st, ed);

	/* Map secure region in DDR */
	io_write32(fw_ddr_base + FIREWALL_DDR_RGN(rgn),
		   RG_MAP_SECURE(ed_mb, st_mb));

	/* Enable secure region for DDR */
	io_setbits32(fw_ddr_base + FIREWALL_DDR_CON, BIT(rgn));

	return 0;
}

int platform_secure_init(void)
{
	vaddr_t pericru_base = (vaddr_t)phys_to_virt_io(PERICRU_BASE, PERICRU_SIZE);
	vaddr_t sys_sgrf_base = (vaddr_t)phys_to_virt_io(SYS_SGRF_BASE, SYS_SGRF_SIZE);

	if (!pericru_base || !sys_sgrf_base) {
		EMSG("MMU Mapping failed");
		return TEE_ERROR_GENERIC;
	}

	/* Open the Secure Configuration Gate */
	dsb();
	io_write32(sys_sgrf_base + 0x34, 0x00080008);
	dsb();

	/* Assert OTP Reset via CRU */
	// Bit 4 is reset, Bit 20 is the mask
	io_write32(pericru_base + 0x438, 0x00100010);
	udelay(2);
	dsb();

	/* Release OTP Reset */
	io_write32(pericru_base + 0x438, 0x00100000);
	udelay(1);
	dsb();

	return 0;
}
