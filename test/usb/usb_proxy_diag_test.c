#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_proxy_diag.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return EXIT_FAILURE; \
    } \
} while (0)

static uint8_t page[ZZUSB_DIAG_PAGE_SIZE];
static uint8_t snapshot[ZZUSB_DIAG_PAGE_SIZE];
static uint8_t mailbox[128];
static int inject_during_publish;

static uint16_t read_be16(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t read_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           src[3];
}

static void publish_flush(volatile void *address, size_t length)
{
    (void)address;
    (void)length;
    if (inject_during_publish) {
        inject_during_publish = 0;
        zzusb_diag_record(ZZUSB_DIAG_EVENT_LATE_COMPLETION, 0xf4,
                          999u, 7u, 3u, 2u, 1u, 0u, 0u,
                          0xfeedu, 123u);
    }
}

int main(void)
{
    zzusb_diag_reset();
    memset(page, 0xa5, sizeof(page));
    memset(mailbox, 0x01, sizeof(mailbox));

    for (uint32_t i = 1; i <= 200u; i++) {
        zzusb_diag_count(ZZUSB_DIAG_COUNT_REQUEST);
        zzusb_diag_record((uint16_t)((i % 12u) + 1u),
                          (uint16_t)(i & 0xffu), i, 7u,
                          (uint16_t)(i & 0x7fu),
                          (uint8_t)(i & 0x0fu), (uint8_t)(i & 1u),
                          (uint16_t)(i >> 4), (uint16_t)(i & 7u),
                          i * 3u, i * 5u);
    }
    zzusb_diag_high_water(4u);
    zzusb_diag_high_water(2u);
    zzusb_diag_high_water(9u);

    inject_during_publish = 1;
    zzusb_diag_publish(page, sizeof(page), 0x1fu, 7u, 200u,
                       4u, 0x55u, publish_flush);

    CHECK(read_be32(page + ZZUSB_DIAG_OFF_MAGIC) == ZZUSB_DIAG_MAGIC);
    CHECK((read_be32(page + ZZUSB_DIAG_OFF_GENERATION) & 1u) == 0u);
    CHECK(read_be16(page + ZZUSB_DIAG_OFF_VERSION) == ZZUSB_DIAG_VERSION);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENT_NEXT) == 201u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENT_COUNT) == 64u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_LOST_EVENTS) == 137u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_COUNTERS +
                    ZZUSB_DIAG_COUNT_REQUEST * 4u) == 200u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_COUNTERS +
                    ZZUSB_DIAG_COUNT_QUEUE_HIGH_WATER * 4u) == 9u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENTS +
                    ZZUSB_DIAG_EVT_OFF_SEQUENCE) == 138u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENTS +
                    63u * ZZUSB_DIAG_EVENT_SIZE +
                    ZZUSB_DIAG_EVT_OFF_SEQUENCE) == 201u);

    CHECK(zzusb_diag_read_coherent(page, snapshot,
                                   sizeof(snapshot), 4u));
    CHECK(memcmp(page, snapshot, sizeof(page)) == 0);
    CHECK(mailbox[0] == 0x01u && mailbox[sizeof(mailbox) - 1u] == 0x01u);

    page[ZZUSB_DIAG_OFF_GENERATION + 3u] |= 1u;
    CHECK(!zzusb_diag_read_coherent(page, snapshot,
                                    sizeof(snapshot), 2u));

    zzusb_diag_publish(page, sizeof(page), 0x1fu, 7u, 201u,
                       0u, 0u, NULL);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENT_NEXT) == 201u);
    CHECK(read_be32(page + ZZUSB_DIAG_OFF_EVENTS +
                    63u * ZZUSB_DIAG_EVENT_SIZE +
                    ZZUSB_DIAG_EVT_OFF_SEQUENCE) == 201u);

    puts("USB diagnostic ring and seqlock snapshot contract satisfied");
    return EXIT_SUCCESS;
}
