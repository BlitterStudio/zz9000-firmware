/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "usb/ehci-iso.h"

static void test_frame_wrap(void)
{
    assert(ehci_iso_frame_distance(2, 2046) == 4);
    assert(ehci_iso_frame_distance(2046, 2) == 2044);
    assert(ehci_iso_frame_distance(0, 0) == 0);
    assert(ehci_iso_frame_schedulable(2, 2046));
    assert(!ehci_iso_frame_schedulable(1, 2046));
    assert(!ehci_iso_frame_schedulable(1024, 0));
}

static void test_itd_completion_length(void)
{
    assert(ehci_iso_itd_actual(1024U << 16, 1024) == 1024);
    assert(ehci_iso_itd_actual(64U << 16, 1024) == 64);
    assert(ehci_iso_itd_actual(1025U << 16, 1024) == 0);
}

static void test_split_masks(void)
{
    uint8_t smask;
    uint8_t cmask;

    assert(ehci_iso_split_masks(188, 1, 0, 0, 0, &smask, &cmask));
    assert(smask == 0x01);
    assert(cmask == 0x1c);

    assert(ehci_iso_split_masks(376, 0, 0, 1, 1, &smask, &cmask));
    assert(smask == 0x06);
    assert(cmask == 0);

    assert(ehci_iso_split_masks(1023, 1, 0, 0, 0, &smask, &cmask));
    assert(smask == 0x01);
    assert(cmask == 0xfc);

    assert(!ehci_iso_split_masks(1024, 0, 0, 0, 0, &smask, &cmask));
    assert(!ehci_iso_split_masks(1, 1, 4, 0, 0, &smask, &cmask));
}

static void test_itd_builder(void)
{
    struct ehci_iso_config config;
    struct ehci_iso_itd itd;

    memset(&config, 0, sizeof(config));
    config.address = 5;
    config.endpoint = 3;
    config.direction_in = 1;
    config.max_packet = 0x13ff;

    assert(ehci_iso_build_itd(&itd, &config, 0x12345f80,
                              3069, 7, 0xdead0002));
    assert(itd.next == 0xdead0002);
    for (unsigned index = 0; index < 7; index++)
        assert(itd.transaction[index] == 0);
    assert(itd.transaction[7] ==
           (EHCI_ITD_ACTIVE | EHCI_ITD_IOC | (3069U << 16) | 0x0f80U));
    assert(itd.buffer[0] == 0x12345305);
    assert(itd.buffer[1] == 0x123463ff);
    assert(itd.buffer[2] == 0x12347803);
    assert(itd.buffer[6] == 0x1234b000);

    assert(!ehci_iso_build_itd(&itd, &config, 0x12345f80,
                               3070, 0, EHCI_ISO_LINK_TERMINATE));
    assert(!ehci_iso_build_itd(&itd, &config, 0x12345f80,
                               1, 8, EHCI_ISO_LINK_TERMINATE));

    config.max_packet = 64;
    assert(ehci_iso_build_itd(&itd, &config, 0x10000000,
                              0, 0, EHCI_ISO_LINK_TERMINATE));
    assert(itd.transaction[0] == (EHCI_ITD_ACTIVE | EHCI_ITD_IOC));
}

static void test_sitd_builder(void)
{
    struct ehci_iso_config config;
    struct ehci_iso_sitd sitd;

    memset(&config, 0, sizeof(config));
    config.address = 9;
    config.endpoint = 4;
    config.direction_in = 1;
    config.hub_address = 2;
    config.hub_port = 3;

    assert(ehci_iso_build_sitd(&sitd, &config, 0x20000ff0,
                               1000, 0x01, 0x1c, 0x12340002));
    assert(sitd.next == 0x12340002);
    assert(sitd.endpoint == 0x83020409);
    assert(sitd.uframe == 0x00001c01);
    assert(sitd.results ==
           (EHCI_SITD_IOC | EHCI_SITD_ACTIVE | (1000U << 16)));
    assert(sitd.buffer[0] == 0x20000ff0);
    assert(sitd.buffer[1] == 0x20001000);
    assert(sitd.back == EHCI_ISO_LINK_TERMINATE);

    config.direction_in = 0;
    assert(ehci_iso_build_sitd(&sitd, &config, 0x20000ff0,
                               376, 0x03, 0, EHCI_ISO_LINK_TERMINATE));
    assert(sitd.endpoint == 0x03020409);
    assert(sitd.buffer[1] == 0x2000100a);

    assert(!ehci_iso_build_sitd(&sitd, &config, 0x20000ff0,
                                1024, 1, 0, EHCI_ISO_LINK_TERMINATE));
    assert(!ehci_iso_build_sitd(&sitd, &config, 0x20000ff0,
                                1, 0, 0, EHCI_ISO_LINK_TERMINATE));
}

int main(void)
{
    test_frame_wrap();
    test_itd_completion_length();
    test_split_masks();
    test_itd_builder();
    test_sitd_builder();
    puts("ehci_iso_test: ok");
    return 0;
}
