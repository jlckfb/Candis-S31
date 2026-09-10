/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Message contract shared by the HP application and the LP housekeeper.
 *
 * This header is compiled by both cores, so it must stay free of any IDF
 * dependency beyond <stdint.h>: the LP core builds with its own toolchain and
 * does not see the HP driver headers.
 *
 * A mailbox message is an lp_message_t (intptr_t, 32-bit on this target)
 * carrying an 8-bit event code in the high byte and a 24-bit payload in the
 * low bytes.
 */

#pragma once

#include <stdint.h>

/** Event codes reported by the LP housekeeper to the HP core. */
typedef enum {
    /** LP firmware finished initializing. Payload: protocol version. */
    CANDIS_LP_EVENT_BOOT = 0x01,
    /** Periodic heartbeat. Payload: poll iteration counter (24-bit wrap). */
    CANDIS_LP_EVENT_HEARTBEAT = 0x02,
    /** Shared IRQ line (GPIO2) went from released to asserted (low). */
    CANDIS_LP_EVENT_IRQ_ASSERTED = 0x03,
    /** Shared IRQ line returned to released (high) without HP involvement. */
    CANDIS_LP_EVENT_IRQ_RELEASED = 0x04,
    /** Optional LP I2C read succeeded. Payload: register value in bits 7:0. */
    CANDIS_LP_EVENT_I2C_SAMPLE = 0x05,
    /** Optional LP I2C read failed. Payload: truncated esp_err_t. */
    CANDIS_LP_EVENT_I2C_ERROR = 0x06,
} candis_lp_event_t;

/** Protocol version carried by CANDIS_LP_EVENT_BOOT. */
#define CANDIS_LP_PROTOCOL_VERSION 1

/** Payload mask: the low 24 bits of a message. */
#define CANDIS_LP_PAYLOAD_MASK 0x00FFFFFFU

/** Pack an event code and payload into a mailbox message. */
static inline int32_t candis_lp_msg_pack(uint8_t event, uint32_t payload)
{
    return (int32_t)(((uint32_t)event << 24) |
                     (payload & CANDIS_LP_PAYLOAD_MASK));
}

/** Extract the event code from a mailbox message. */
static inline uint8_t candis_lp_msg_event(int32_t msg)
{
    return (uint8_t)(((uint32_t)msg >> 24) & 0xFFU);
}

/** Extract the payload from a mailbox message. */
static inline uint32_t candis_lp_msg_payload(int32_t msg)
{
    return (uint32_t)msg & CANDIS_LP_PAYLOAD_MASK;
}
