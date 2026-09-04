/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief LP core housekeeper for the Candis-S31 low-power prototype.
 *
 * Runs on the rv32imac LP core and does two things:
 *
 * 1. Polls the shared active-low interrupt line (GPIO2, TG28 IRQ wire-ANDed
 *    with RX8130CE /IRQ) at a low rate and reports edges to the HP core.
 * 2. Emits a heartbeat so the HP core can light-sleep and be woken by the
 *    mailbox instead of spinning on its own timer.
 *
 * Pad ownership: while this firmware runs, GPIO2 belongs to the LP core (RTC
 * function). The HP application must not arm its own digital GPIO2 interrupt
 * during that window; see the ownership table in the project README.
 *
 * Mailbox discipline (ESP-IDF v6.1-rc1 and current IDF master):
 * sending asynchronously and then synchronously makes the tx slot index land
 * on the parity the HP core never scans, which live-locks HP receive and hangs
 * the LP core. This file therefore uses lp_core_mailbox_send() exclusively -
 * never lp_core_mailbox_send_async() - and the very first frame after
 * initialization is a synchronous send.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_mailbox.h"
#include "ulp_lp_core_utils.h"

#include "lp_shared.h"

/** Shared IRQ line: GPIO2 is RTC-capable (GPIO0-7 on ESP32-S31). */
#define LP_SHARED_IRQ_IO       LP_IO_NUM_2
/** The line is active low: both sources are open-drain, wire-ANDed. */
#define LP_SHARED_IRQ_ACTIVE   0

/** Poll period. 50 ms keeps the LP core mostly idle while still catching a
 *  power-key press well inside a human reaction time. */
#define LP_POLL_PERIOD_US      50000U

/** Heartbeat cadence expressed in poll iterations (20 * 50 ms = 1 s). */
#define LP_HEARTBEAT_PERIOD    20U

/**
 * Synchronous send timeout in LP CPU cycles.
 *
 * lp_core_mailbox_send() counts the timeout in CPU cycles on the LP side. A
 * bounded value is used instead of -1 so a HP core that stopped receiving
 * cannot wedge the housekeeper; the frame is simply dropped and the next poll
 * iteration continues.
 */
#define LP_SEND_TIMEOUT_CYCLES 2000000

static lp_mailbox_t s_mailbox;

/** Send one event synchronously, ignoring a timeout (frame is dropped). */
static void lp_report(uint8_t event, uint32_t payload)
{
    /* Always the synchronous variant: see the mailbox discipline note above. */
    (void)lp_core_mailbox_send(s_mailbox, candis_lp_msg_pack(event, payload),
                               LP_SEND_TIMEOUT_CYCLES);
}

int main(void)
{
    /* Take the shared IRQ pad as an RTC input with its pull-up: the line is
     * released high and pulled low by either source. */
    ulp_lp_core_gpio_init(LP_SHARED_IRQ_IO);
    ulp_lp_core_gpio_input_enable(LP_SHARED_IRQ_IO);
    ulp_lp_core_gpio_pullup_enable(LP_SHARED_IRQ_IO);
    ulp_lp_core_gpio_pulldown_disable(LP_SHARED_IRQ_IO);

    /* The software mailbox requires the LP core to initialize first; the HP
     * side waits for this before calling lp_core_mailbox_init(). */
    lp_core_mailbox_init(&s_mailbox, NULL);

    /* First frame after init is a synchronous send, never an async one. */
    lp_report(CANDIS_LP_EVENT_BOOT, CANDIS_LP_PROTOCOL_VERSION);

    bool line_asserted =
        ulp_lp_core_gpio_get_level(LP_SHARED_IRQ_IO) == LP_SHARED_IRQ_ACTIVE;
    uint32_t iteration = 0;
    uint32_t since_heartbeat = 0;

    while (1) {
        ulp_lp_core_delay_us(LP_POLL_PERIOD_US);
        iteration++;

        const bool now_asserted =
            ulp_lp_core_gpio_get_level(LP_SHARED_IRQ_IO) == LP_SHARED_IRQ_ACTIVE;
        if (now_asserted != line_asserted) {
            line_asserted = now_asserted;
            /* Wake the HP core explicitly: an IRQ on the shared line is the
             * event the application actually has to service over I2C, and the
             * HP core may be in light sleep with the pad owned by us. */
            ulp_lp_core_wakeup_main_processor();
            lp_report(now_asserted ? CANDIS_LP_EVENT_IRQ_ASSERTED
                      : CANDIS_LP_EVENT_IRQ_RELEASED,
                      iteration & CANDIS_LP_PAYLOAD_MASK);
            since_heartbeat = 0;
            continue;
        }

        if (++since_heartbeat >= LP_HEARTBEAT_PERIOD) {
            since_heartbeat = 0;
            lp_report(CANDIS_LP_EVENT_HEARTBEAT,
                      iteration & CANDIS_LP_PAYLOAD_MASK);
        }
    }

    return 0;
}
