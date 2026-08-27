/*
 * Candis-S31 watch demo - domain suite registration aggregator.
 *
 * test_register_all() is called once by svc_test_start() right after
 * test_registry_init(). Suites register in domain-enum order; the
 * registry keeps per-domain registration order after its stable sort.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_registry.h"

/* Domain suite entry points (one shell file per domain, spec E.2). */
void test_display_register(void);
void test_touch_register(void);
void test_camera_register(void);
void test_audio_register(void);
void test_storage_register(void);
void test_net_register(void);
void test_system_register(void);
void test_memory_register(void);
void test_accel_register(void);
void test_power_register(void);

void test_register_all(void)
{
    test_display_register();
    test_touch_register();
    test_camera_register();
    test_audio_register();
    test_storage_register();
    test_net_register();
    test_system_register();
    test_memory_register();
    test_accel_register();
    test_power_register();
}
