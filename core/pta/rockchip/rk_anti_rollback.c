/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2026, Protonic Holland,
 *  Robin van der Gracht <robin.van.der.gracht@protonic.nl>
 */

#include <drivers/rockchip_otp.h>
#include <kernel/pseudo_ta.h>
#include <tee_api_types.h>
#include <trace.h>
#include <pta_rk_anti_rollback.h>

#define RK3562_SECURE_ROLLBACK_FIRST_WORD       304U
#define RK3562_SECURE_ROLLBACK_WORD_COUNT       24U

#define RK3562_NONSECURE_ROLLBACK_FIRST_WORD   	328U
#define RK3562_NONSECURE_ROLLBACK_WORD_COUNT    24U

#define RK3562_ROLLBACK_SLOT_MARKER             0xC35AU

static TEE_Result hw_read_otp_counter(uint32_t first_hw,
				      uint32_t slot_count,
				      uint32_t *counter)
{
	uint32_t i;
	TEE_Result res;

	if (!counter || !slot_count)
		return TEE_ERROR_BAD_PARAMETERS;

	for (i = 0; i < slot_count; i++) {
		uint16_t raw;

		res = rockchip_otp_read_word(first_hw + i, &raw);
		if (res)
			return res;

		if (raw == 0x0000U) {
			*counter = i;
			return TEE_SUCCESS;
		}

		if (raw != RK3562_ROLLBACK_SLOT_MARKER) {
			EMSG("Invalid rollback slot hw=%"PRIu32" raw=%#04"PRIx16,
			     first_hw + i, raw);
			return TEE_ERROR_SECURITY;
		}
	}

	*counter = slot_count;
	return TEE_SUCCESS;
}

static TEE_Result hw_set_otp_counter(uint32_t first_hw,
				     uint32_t slot_count,
				     uint32_t target_counter)
{
	uint32_t current_counter;
	uint32_t i;
	TEE_Result res;

	if (target_counter > slot_count)
		return TEE_ERROR_BAD_PARAMETERS;

	res = hw_read_otp_counter(first_hw, slot_count, &current_counter);
	if (res)
		return res;

	if (target_counter <= current_counter) {
		IMSG("OTP anti-rollback counter (%u) already satisfies "
		     "target (%u)", current_counter, target_counter);
		return TEE_SUCCESS;
	}

	for (i = current_counter; i < target_counter; i++) {
		res = rockchip_otp_program_word(first_hw + i,
					   RK3562_ROLLBACK_SLOT_MARKER);
		if (res)
			return res;

		/* Verify the newly programmed slot only. */
		{
			uint16_t raw;

			res = rockchip_otp_read_word(first_hw + i, &raw);
			if (res)
				return res;

			if (raw != RK3562_ROLLBACK_SLOT_MARKER) {
				EMSG("Rollback slot verify failed hw=%"PRIu32
				     " raw=%#04"PRIx16,
				     first_hw + i, raw);
				return TEE_ERROR_SECURITY;
			}
		}
	}

	IMSG("OTP anti-rollback counter incremented from %u to %u",
	     current_counter, target_counter);

	return TEE_SUCCESS;
}

static TEE_Result invoke_command(void *sess_ctx __unused, uint32_t cmd_id,
				 uint32_t param_types,
				 TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_param_types =
		TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INOUT,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE,
				TEE_PARAM_TYPE_NONE);
	uint32_t counter = 0;
	TEE_Result res;

	if (param_types != exp_param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	switch (cmd_id) {
	case PTA_RK_ANTI_ROLLBACKD_GET_SECURE_COUNTER:
		res = hw_read_otp_counter(RK3562_SECURE_ROLLBACK_FIRST_WORD,
					  RK3562_SECURE_ROLLBACK_WORD_COUNT,
					  &counter);
		if (res == TEE_SUCCESS)
			params[0].value.a = counter;
		return res;

	case PTA_RK_ANTI_ROLLBACKD_SET_SECURE_COUNTER:
		return hw_set_otp_counter(RK3562_SECURE_ROLLBACK_FIRST_WORD,
					  RK3562_SECURE_ROLLBACK_WORD_COUNT,
					  params[0].value.a);

	case PTA_RK_ANTI_ROLLBACKD_GET_NONSECURE_COUNTER:
		res = hw_read_otp_counter(RK3562_NONSECURE_ROLLBACK_FIRST_WORD,
					  RK3562_NONSECURE_ROLLBACK_WORD_COUNT,
					  &counter);
		if (res == TEE_SUCCESS)
			params[0].value.a = counter;
		return res;

	case PTA_RK_ANTI_ROLLBACKD_SET_NONSECURE_COUNTER:
		return hw_set_otp_counter(RK3562_NONSECURE_ROLLBACK_FIRST_WORD,
					  RK3562_NONSECURE_ROLLBACK_WORD_COUNT,
					  params[0].value.a);

	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}

pseudo_ta_register(.uuid = PTA_ANTI_ROLLBACK_UUID,
		   .name = "rk_anti_rollback.pta",
		   .flags = PTA_DEFAULT_FLAGS,
		   .invoke_command_entry_point = invoke_command);
