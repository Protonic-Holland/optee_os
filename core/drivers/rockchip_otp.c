// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (C) 2019, Theobroma Systems Design und Consulting GmbH
 * Copyright (c) 2024, Rockchip, Inc. All rights reserved.
 * Copyright (C) 2025, Pengutronix, Michael Tretter <m.tretter@pengutronix.de>
 */

#include <common.h>
#include <drivers/rockchip_otp.h>
#include <io.h>
#include <kernel/panic.h>
#include <kernel/tee_common_otp.h>
#include <mm/core_memprot.h>
#include <utee_defines.h>

#define OTP_S_AUTO_CTRL			0x0004
#define OTP_S_AUTO_EN			0x0008
#define OTP_S_PROG_DATA			0x0010
#define OTP_S_DOUT			0x0020
#define OTP_S_INT_ST			0x0084

#define OTP_S_LOCK_CTRL			0x0050
#define OTP_S_USER_CTRL			0x0100
#define OTP_S_USER_ADDR			0x0104
#define OTP_S_USER_ENABLE		0x0108
#define OTP_S_USER_Q			0x0124
#define OTP_S_INT_STATUS		0x0304

#define OTP_S_USER_ADDR_MASK		GENMASK_32(31, 16)
#define OTP_S_USE_USER			BIT(0)
#define OTP_S_USE_USER_MASK		GENMASK_32(16, 16)
#define OTP_S_USER_FSM_ENABLE		BIT(0)
#define OTP_S_USER_FSM_ENABLE_MASK	GENMASK_32(16, 16)
#define OTP_S_LOCK			BIT(0)
#define OTP_S_LOCK_MASK			GENMASK_32(16, 16)
#define OTP_S_USER_DONE			BIT(2)
#define OTP_S_INT_STATUS_MASK		GENMASK_32(31, 16)

#define ADDR_SHIFT	16
#define BURST_SHIFT	8
#define CMD_READ	0
#define CMD_WRITE	2
#define EN_ENABLE	1
#define EN_DISABLE	0

#define BURST_SIZE	8
#define OTP_WORD	1

#define OTP_S_ERROR_BIT		BIT32(4)
#define OTP_S_WR_DONE_BIT	BIT32(3)
#define OTP_S_VERIFY_BIT	BIT32(2)
#define OTP_S_RD_DONE_BIT	BIT32(1)


register_phys_mem_pgdir(MEM_AREA_IO_SEC, OTP_S_BASE, OTP_S_SIZE);

#if defined(PLATFORM_FLAVOR_rk3562)

#define OTP_SBPI_CTRL_CFG          0xff000200
#define OTP_SBPI_CTRL_PROG         0xff003a00

#define OTP_SBPI_STATUS            0x002c
#define OTP_SBPI_EXEC              0x10001

#define OTP_SBPI_VALID1            0xffff0001
#define OTP_SBPI_VALID2            0xffff0002
#define OTP_SBPI_VALID14           0xffff000e

#define OTP_TIMEOUT_US             10000
#define OTP_FLAG_TIMEOUT_US        20000
#define OTP_CHECK_TIMEOUT_US       100000

#define SBPI_CTRL_CFG          0xff000200
#define SBPI_CTRL_PROG         0xff003a00

#define SBPI_VALID_1           0xffff0001
#define SBPI_VALID_2           0xffff0002
#define SBPI_VALID_14          0xffff000e

#define SBPI_CMD_ENTER_PROG    0xfb
#define SBPI_CMD_LOAD_DATA     0xc0
#define SBPI_CMD_ARM_PROG      0xff

#define OTP_S_SBPI_CTRL             0x0020
#define OTP_S_SBPI_CMD_VALID        0x0024
#define OTP_S_SBPI_STATUS           0x0028
#define OTP_S_SBPI_FLAG             0x002c

#define OTP_S_SBPI_CMD(n)           (0x1000 + ((n) * 4))


struct otp_sbpi {
	uint32_t ctrl;
	uint32_t valid;

	uint32_t cmd[15];
	unsigned int ncmd;
};

static inline void otp_write(vaddr_t base, uint32_t reg, uint32_t val)
{
	dsb();
	io_write32(base + reg, val);
}

static inline uint32_t otp_read(vaddr_t base, uint32_t reg)
{
	uint32_t val;

	val = io_read32(base + reg);
	dsb();

	return val;
}

static TEE_Result otp_wait_status(vaddr_t base, uint32_t flag)
{
	uint32_t timeout = OTP_TIMEOUT_US;

	while (timeout--) {
		if (otp_read(base, OTP_S_INT_STATUS) & flag) {
			otp_write(base, OTP_S_INT_STATUS,
				  OTP_S_INT_STATUS_MASK | flag);
			return TEE_SUCCESS;
		}

		udelay(1);
	}

	return TEE_ERROR_BUSY;
}

static TEE_Result otp_check_flag(vaddr_t base)
{
	uint32_t timeout = OTP_CHECK_TIMEOUT_US;

	while (timeout--) {
		uint32_t val = otp_read(base, OTP_SBPI_STATUS);

		if (!(val & BIT(4)))
			return (val & BIT(5)) ? TEE_ERROR_GENERIC
					      : TEE_SUCCESS;

		udelay(1);
	}

	return TEE_ERROR_BUSY;
}

static TEE_Result otp_wait_flag(vaddr_t base)
{
	uint32_t timeout = OTP_FLAG_TIMEOUT_US;

	while (timeout--) {
		if (otp_read(base, OTP_SBPI_STATUS) & BIT(4))
			return TEE_SUCCESS;

		udelay(1);
	}

	return TEE_ERROR_BUSY;
}

static TEE_Result otp_sbpi_exec(vaddr_t base,
				uint32_t ctrl,
				uint32_t valid,
				const uint32_t *cmd,
				size_t ncmd)
{
	size_t i;

	otp_write(base, OTP_S_SBPI_CTRL, ctrl);
	otp_write(base, OTP_S_SBPI_CMD_VALID, valid);

	for (i = 0; i < ncmd; i++)
		otp_write(base, OTP_S_SBPI_CMD(i), cmd[i]);

	otp_write(base, OTP_S_SBPI_CTRL, OTP_SBPI_EXEC);

	return otp_wait_status(base, BIT(1));
}

static TEE_Result otp_ecc_enable(vaddr_t base, bool enable)
{
	uint32_t cmd[2];

	cmd[0] = 0xfa;
	cmd[1] = enable ? 0 : 9;

	return otp_sbpi_exec(base, OTP_SBPI_CTRL_CFG, OTP_SBPI_VALID1,
			     cmd, ARRAY_SIZE(cmd));
}

static bool otp_word_index_valid(uint32_t index)
{
	/* Secure boot lock bits */
	if (index >= 0x10 && index <= 0x11)
		return true;

	/* Lower non-ecc area */
	if (index >= 0xc0 && index <= 0xdf)
		return true;

	/* Protected + Non-protected OEM zone */
	if (index >= 0x120 && index <= 0x16f)
		return true;

	/* Upper non-ecc area */
	if (index >= 0x1a0 && index <= 0x1bf)
		return true;

	return false;
}

static bool otp_addr_use_ecc(uint32_t word_index)
{
	/*
	 * Matches the original driver:
	 *
	 * ECC disabled:
	 *   0x10..0x1f
	 *   0x1a0..0x1bf
	 *
	 * ECC enabled everywhere else.
	 */
	if (word_index >= 0x10 && word_index <= 0x1f)
		return false;

	if (word_index >= 0x1a0 && word_index <= 0x1bf)
		return false;

	return true;
}

TEE_Result rockchip_otp_read_word(uint32_t word_index, uint16_t *value)
{
	vaddr_t base;
	TEE_Result res;
	TEE_Result restore_res;
	bool use_ecc;

	if (!value)
		return TEE_ERROR_BAD_PARAMETERS;

	if (!otp_word_index_valid(word_index))
		return TEE_ERROR_BAD_PARAMETERS;

	base = (vaddr_t)phys_to_virt(OTP_S_BASE,
				     MEM_AREA_IO_SEC,
				     OTP_S_SIZE);

	use_ecc = otp_addr_use_ecc(word_index);

	otp_write(base, OTP_S_USER_CTRL,
		OTP_S_USE_USER | OTP_S_USE_USER_MASK);
	udelay(50);

	res = otp_ecc_enable(base, use_ecc);
	if (res)
		goto out_user;

	otp_write(base, OTP_S_INT_STATUS,
		  OTP_S_INT_STATUS_MASK | OTP_S_USER_DONE);

	otp_write(base, OTP_S_USER_ADDR,
		   OTP_S_USER_ADDR_MASK | word_index);

	otp_write(base, OTP_S_USER_ENABLE,
		   OTP_S_USER_FSM_ENABLE |
		   OTP_S_USER_FSM_ENABLE_MASK);

	res = otp_wait_status(base, OTP_S_USER_DONE);
	if (res)
		goto out;

	*value = io_read16(base + OTP_S_USER_Q);
	dsb();

out:
	restore_res = otp_ecc_enable(base, false);
	if (!res)
		res = restore_res;

out_user:
	otp_write(base, OTP_S_USER_CTRL, OTP_S_USE_USER_MASK);

	return res;
}

TEE_Result rockchip_otp_read_secure(uint32_t *value,
				    uint32_t index,
				    uint32_t count)
{
	TEE_Result res;
	uint32_t i;

	if (!value || !count)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < count; i++) {
		uint32_t word = (index + i) * 2;
		uint16_t lo;
		uint16_t hi;

		res = rockchip_otp_read_word(word, &lo);
		if (res)
			goto out;

		res = rockchip_otp_read_word(word + 1, &hi);
		if (res)
			goto out;

		value[i] = ((uint32_t)hi << 16) | lo;
	}

	res = TEE_SUCCESS;

out:
	return res;
}

static TEE_Result otp_prepare_program(vaddr_t base,
				      uint32_t word_index)
{
	static const uint32_t addr_prefix[] = {
		0xf0, 1, 0x7a, 0x25,
		0, 0, 0,
		0x1f, 0x0b, 8,
		0, 0, 0,
	};

	static const uint32_t prog_seq[] = {
		0xf0, 1, 0x7a, 0x15,
		0xdc, 0x92, 0x79, 0x81,
		0x7e, 0x21, 0x11, 0x9d,
		2, 0, 0,
	};

	uint32_t cmd[15];
	TEE_Result res;

	memcpy(cmd, addr_prefix, sizeof(addr_prefix));
	cmd[13] = word_index;
	cmd[14] = word_index >> 8;

	res = otp_sbpi_exec(base, SBPI_CTRL_CFG,
			    SBPI_VALID_14,
			    cmd, ARRAY_SIZE(cmd));
	if (res)
		return res;

	return otp_sbpi_exec(base, SBPI_CTRL_PROG,
			     SBPI_VALID_14,
			     prog_seq,
			     ARRAY_SIZE(prog_seq));
}

static TEE_Result otp_load_data(vaddr_t base,
				uint16_t value)
{
	const uint32_t cmd[] = {
		SBPI_CMD_LOAD_DATA,
		value & 0xff,
		value >> 8,
	};

	return otp_sbpi_exec(base,
			     SBPI_CTRL_CFG,
			     SBPI_VALID_2,
			     cmd,
			     ARRAY_SIZE(cmd));
}

static TEE_Result otp_execute_program(vaddr_t base)
{
	TEE_Result res;
	static const uint32_t enter_prog[] = {
		SBPI_CMD_ENTER_PROG,
		0,
	};

	static const uint32_t arm_prog[] = {
		SBPI_CMD_ARM_PROG,
		10,
	};

	static const uint32_t execute[] = {
		1,
		0xbf,
		0,
	};

	static const uint32_t finish[] = {
		2,
		0xbf,
	};

	res = otp_sbpi_exec(base, SBPI_CTRL_CFG,
			    SBPI_VALID_1,
			    enter_prog,
			    ARRAY_SIZE(enter_prog));

	if (res)
		return res;

	res = otp_sbpi_exec(base, SBPI_CTRL_PROG,
			    SBPI_VALID_1,
			    arm_prog,
			    ARRAY_SIZE(arm_prog));
	if (res)
		return res;

	res = otp_sbpi_exec(base, SBPI_CTRL_PROG,
			    SBPI_VALID_2,
			    execute,
			    ARRAY_SIZE(execute));
	if (res)
		return res;

	res = otp_check_flag(base);
	if (res)
		return res;

	res = otp_sbpi_exec(base, SBPI_CTRL_PROG,
			    SBPI_VALID_1,
			    finish,
			    ARRAY_SIZE(finish));
	if (res)
		return res;

	res = otp_wait_flag(base);
	return res;
}


TEE_Result rockchip_otp_program_word(uint32_t word_index, uint16_t value)
{
	vaddr_t base;
	TEE_Result res;
	TEE_Result restore_res;
	bool use_ecc;

	if (!value)
		return TEE_SUCCESS;

	base = (vaddr_t)phys_to_virt(OTP_S_BASE,
				     MEM_AREA_IO_SEC,
				     OTP_S_SIZE);

	use_ecc = otp_addr_use_ecc(word_index);

	res = otp_ecc_enable(base, use_ecc);
	if (res)
		return res;

	/* Leave USER mode before using the SBPI interface. */
	otp_write(base, OTP_S_USER_CTRL,
		   OTP_S_USE_USER_MASK);

	otp_write(base, OTP_S_SBPI_CTRL, 0x40004);
	otp_write(base, OTP_S_SBPI_CMD_VALID, 0xffff0000);

	res = otp_prepare_program(base, word_index);
	if (res)
		goto out;

	res = otp_load_data(base, value);
	if (res)
		goto out;

	res = otp_execute_program(base);

out:
	restore_res = otp_ecc_enable(base, false);
	if (!res)
		res = restore_res;

	return res;
}

TEE_Result rockchip_otp_write_secure(const uint32_t *value,
				     uint32_t index,
				     uint32_t count)
{
	TEE_Result res = TEE_SUCCESS;
	uint32_t i;

	if (!value || !count)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < count; i++) {
		uint32_t hw = (index + i) * 2;

		/* Lower 16 bits */
		res = rockchip_otp_program_word(hw, value[i] & 0xffff);
		if (res)
			break;

		/* Upper 16 bits */
		res = rockchip_otp_program_word(hw + 1, value[i] >> 16);
		if (res)
			break;
	}

	return res;
}

#elif defined(PLATFORM_FLAVOR_rk3588)

#define OTP_POLL_PERIOD_US	0
#define OTP_POLL_TIMEOUT_US	1000

#define MAX_INDEX	0x300

TEE_Result rockchip_otp_read_secure(uint32_t *value, uint32_t index,
				    uint32_t count)
{
	vaddr_t base = (vaddr_t)phys_to_virt(OTP_S_BASE, MEM_AREA_IO_SEC,
					     OTP_S_SIZE);
	uint32_t int_status = 0;
	uint32_t i = 0;
	uint32_t val = 0;
	uint32_t auto_ctrl_val = 0;
	TEE_Result res = TEE_SUCCESS;

	if (!base)
		panic("OTP_S base not mapped");

	/* Check for invalid parameters or exceeding hardware burst limit */
	if (!value || !count || count > BURST_SIZE ||
	    (index + count > MAX_INDEX))
		return TEE_ERROR_BAD_PARAMETERS;

	/* Setup read: index, count, command = READ */
	auto_ctrl_val = SHIFT_U32(index, ADDR_SHIFT) |
			SHIFT_U32(count, BURST_SHIFT) |
			CMD_READ;

	/* Clear any pending interrupts by reading & writing back INT_ST */
	io_write32(base + OTP_S_INT_ST, io_read32(base + OTP_S_INT_ST));

	/* Set read command */
	io_write32(base + OTP_S_AUTO_CTRL, auto_ctrl_val);

	/* Enable read */
	io_write32(base + OTP_S_AUTO_EN, EN_ENABLE);

	/* Wait for RD_DONE or ERROR bits */
	res = IO_READ32_POLL_TIMEOUT(base + OTP_S_INT_ST,
				     int_status,
				     (int_status & OTP_S_RD_DONE_BIT) ||
				     (int_status & OTP_S_ERROR_BIT),
				     OTP_POLL_PERIOD_US,
				     OTP_POLL_TIMEOUT_US);

	/* Clear the interrupt again */
	io_write32(base + OTP_S_INT_ST, io_read32(base + OTP_S_INT_ST));

	if (int_status & OTP_S_ERROR_BIT) {
		EMSG("OTP_S Error");
		return TEE_ERROR_GENERIC;
	}
	if (res) {
		EMSG("OTP_S Timeout");
		return TEE_ERROR_BUSY;
	}

	/* Read out the data */
	for (i = 0; i < count; i++) {
		val = io_read32(base + OTP_S_DOUT +
				(i * sizeof(uint32_t)));
		value[i] = val;
	}

	return TEE_SUCCESS;
}

TEE_Result rockchip_otp_write_secure(const uint32_t *value, uint32_t index,
				     uint32_t count)
{
	vaddr_t base = (vaddr_t)phys_to_virt(OTP_S_BASE, MEM_AREA_IO_SEC,
					     OTP_S_SIZE);
	uint32_t int_status = 0;
	uint32_t i = 0;

	if (!base)
		panic("OTP_S base not mapped");

	/* Check for invalid parameters or exceeding hardware limits */
	if (!value || !count || count > BURST_SIZE ||
	    (index + count > MAX_INDEX))
		return TEE_ERROR_BAD_PARAMETERS;

	/* Program OTP words */
	for (i = 0; i < count; i++) {
		uint32_t old_val = 0;
		uint32_t new_val = 0;
		uint32_t curr_idx = index + i;
		TEE_Result res = TEE_SUCCESS;

		/* Setup write: curr_idx, command = WRITE */
		uint32_t auto_ctrl_val = SHIFT_U32(curr_idx, ADDR_SHIFT) |
						   CMD_WRITE;

		/* Read existing OTP word to see which bits can be set */
		res = rockchip_otp_read_secure(&old_val, curr_idx, OTP_WORD);
		if (res != TEE_SUCCESS)
			return res;

		/* Check if bits in value conflict with old_val */
		if (~*value & old_val) {
			EMSG("OTP_S Program fail");
			return TEE_ERROR_GENERIC;
		}

		/* Only program bits that are currently 0 (0->1) */
		new_val = *value & ~old_val;
		value++;
		if (!new_val)
			continue;

		/* Clear any pending interrupts */
		io_write32(base + OTP_S_INT_ST, io_read32(base + OTP_S_INT_ST));

		/* Set write command */
		io_write32(base + OTP_S_AUTO_CTRL, auto_ctrl_val);

		/* Write the new bits into PROG_DATA register */
		io_write32(base + OTP_S_PROG_DATA, new_val);

		/* Enable the write */
		io_write32(base + OTP_S_AUTO_EN, EN_ENABLE);

		/* Poll for WR_DONE or verify/error bits */
		res = IO_READ32_POLL_TIMEOUT(base + OTP_S_INT_ST,
					     int_status,
					     (int_status & OTP_S_WR_DONE_BIT) ||
					     (int_status & OTP_S_VERIFY_BIT) ||
					     (int_status & OTP_S_ERROR_BIT),
					     OTP_POLL_PERIOD_US,
					     OTP_POLL_TIMEOUT_US);

		/* Clear INT status bits */
		io_write32(base + OTP_S_INT_ST, int_status);

		/* Check for VERIFY_FAIL, ERROR or timeout */
		if (int_status & OTP_S_VERIFY_BIT) {
			EMSG("OTP_S Verification fail");
			return TEE_ERROR_GENERIC;
		}
		if (int_status & OTP_S_ERROR_BIT) {
			EMSG("OTP_S Error");
			return TEE_ERROR_GENERIC;
		}
		if (res) {
			EMSG("OTP_S Timeout");
			return TEE_ERROR_BUSY;
		}
	}

	return TEE_SUCCESS;
}
#endif
