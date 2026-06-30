/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2026, Protonic Holland,
 *  Robin van der Gracht <robin.van.der.gracht@protonic.nl>
 */

#ifndef __PTA_RK_ANTI_ROLLBACK_H__
#define __PTA_RK_ANTI_ROLLBACK_H__

#define PTA_ANTI_ROLLBACK_UUID { 0x76c73d1f, 0x614a, 0x4f6f, \
		{ 0x91, 0x00, 0x9b, 0xbc, 0xaa, 0xca, 0x1f, 0xee } }

/**
 * @brief Retrieves the nv-counter for secure firmware from OTP.
 */
#define PTA_RK_ANTI_ROLLBACKD_GET_SECURE_COUNTER        0

/**
 * @brief Increments nv-counter for secure firmware in OTP.
 */
#define PTA_RK_ANTI_ROLLBACKD_SET_SECURE_COUNTER        1

/**
 * @brief Retrieves the nv-counter for non-secure firmware from OTP.
 */
#define PTA_RK_ANTI_ROLLBACKD_GET_NONSECURE_COUNTER     2

/**
 * @brief Increments nv-counter for non-secure firmware in OTP.
 */
#define PTA_RK_ANTI_ROLLBACKD_SET_NONSECURE_COUNTER     3

#endif /* __PTA_RK_ANTI_ROLLBACK_H__ */
