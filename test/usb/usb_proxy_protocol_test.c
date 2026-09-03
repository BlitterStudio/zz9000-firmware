#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_proxy.h"

#define CHECK(expr) do {     if (!(expr)) {         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);         return EXIT_FAILURE;     } } while (0)

static void put_le16_test(void *p, uint16_t value)
{
    uint8_t *bytes = p;
    bytes[0] = value & 0xffu;
    bytes[1] = value >> 8;
}

static void make_valid_control(struct ZZUSBCommand *cmd,
                               struct ZZUSBProtocolExtension *ext)
{
    memset(cmd, 0, sizeof(*cmd));
    memset(ext, 0, sizeof(*ext));
    put_be16(&cmd->cmd, ZZUSB_CMD_CONTROL_XFER);
    put_be32(&cmd->dev_addr, 1);
    put_be16(&cmd->direction, 0x80);
    put_be16(&cmd->xfer_type, ZZUSB_XFER_CONTROL);
    put_be16(&cmd->max_pkt_size, 64);
    put_be32(&cmd->data_length, 4);
    put_be16(&cmd->speed, ZZUSB_SPEED_HIGH);
    cmd->setup_bRequestType = 0x80;
    put_le16_test(&cmd->setup_wLength, 4);
    put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
    put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
    put_be32(&ext->request_id, 1);
    put_be32(&ext->controller_epoch, 7);
}

static void make_valid_bulk(struct ZZUSBCommand *cmd,
                            struct ZZUSBProtocolExtension *ext,
                            uint16_t direction)
{
    memset(cmd, 0, sizeof(*cmd));
    memset(ext, 0, sizeof(*ext));
    put_be16(&cmd->cmd, ZZUSB_CMD_BULK_XFER);
    put_be32(&cmd->dev_addr, 1);
    put_be16(&cmd->endpoint, 1);
    put_be16(&cmd->direction, direction);
    put_be16(&cmd->xfer_type, ZZUSB_XFER_BULK);
    put_be16(&cmd->max_pkt_size, 512);
    put_be32(&cmd->data_length, 512);
    put_be32(&cmd->timeout_ms, 5);
    put_be16(&cmd->speed, ZZUSB_SPEED_HIGH);
    put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
    put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
    put_be32(&ext->request_id, 2);
    put_be32(&ext->controller_epoch, 7);
}

static void make_valid_iso(struct ZZUSBCommand *cmd,
                           struct ZZUSBProtocolExtension *ext,
                           uint16_t command, uint32_t data_length)
{
    memset(cmd, 0, sizeof(*cmd));
    memset(ext, 0, sizeof(*ext));
    put_be16(&cmd->cmd, command);
    put_be32(&cmd->dev_addr, 1);
    put_be16(&cmd->endpoint, 1);
    put_be16(&cmd->direction, 0x80);
    put_be16(&cmd->xfer_type, ZZUSB_XFER_ISO);
    put_be16(&cmd->max_pkt_size, 1024);
    put_be32(&cmd->data_length, data_length);
    put_be16(&cmd->interval, 1);
    put_be16(&cmd->speed, ZZUSB_SPEED_HIGH);
    put_be16(&ext->version, ZZUSB_PROTOCOL_VERSION);
    put_be16(&ext->header_size, ZZUSB_V2_HEADER_SIZE);
    put_be32(&ext->request_id, 2);
    put_be32(&ext->controller_epoch, 7);
}

int main(void)
{
    struct ZZUSBCommand cmd;
    struct ZZUSBProtocolExtension ext;

    CHECK(ZZUSB_CMD_SIZE == 48u);
    CHECK(ZZUSB_V2_HEADER_SIZE == 64u);
    CHECK(ZZUSB_DATA_OFFSET == 64u);
    CHECK(ZZUSB_APERTURE_SIZE == 24576u);
    CHECK(ZZUSB_MAX_XFER == 24512u);
    CHECK(ZZUSB_V2_DATA_MAX == 16384u);
    CHECK(ZZUSB_DIAG_OFFSET == 20480u);
    CHECK(ZZUSB_DIAG_SIZE == 4096u);
    CHECK(sizeof(struct ZZUSBCommand) == ZZUSB_CMD_SIZE);
    CHECK(sizeof(struct ZZUSBProtocolExtension) == 16u);

    CHECK(offsetof(struct ZZUSBCommand, cmd) == 0u);
    CHECK(offsetof(struct ZZUSBCommand, status) == 2u);
    CHECK(offsetof(struct ZZUSBCommand, dev_addr) == 4u);
    CHECK(offsetof(struct ZZUSBCommand, data_length) == 16u);
    CHECK(offsetof(struct ZZUSBCommand, actual_length) == 20u);
    CHECK(offsetof(struct ZZUSBCommand, timeout_ms) == 24u);
    CHECK(offsetof(struct ZZUSBCommand, setup_bRequestType) == 32u);
    CHECK(offsetof(struct ZZUSBCommand, split_hub_addr) == 40u);
    CHECK(offsetof(struct ZZUSBCommand, flags) == 44u);
    CHECK(offsetof(struct ZZUSBCommand, reserved) == 46u);
    CHECK(offsetof(struct ZZUSBProtocolExtension, version) == 0u);
    CHECK(offsetof(struct ZZUSBProtocolExtension, header_size) == 2u);
    CHECK(offsetof(struct ZZUSBProtocolExtension, request_id) == 4u);
    CHECK(offsetof(struct ZZUSBProtocolExtension, controller_epoch) == 8u);
    CHECK(offsetof(struct ZZUSBProtocolExtension, capabilities) == 12u);

    CHECK(ZZUSB_CMD_CONTROL_XFER == 0x01u);
    CHECK(ZZUSB_CMD_BULK_XFER == 0x02u);
    CHECK(ZZUSB_CMD_INT_XFER == 0x03u);
    CHECK(ZZUSB_CMD_ISO_XFER == 0x04u);
    CHECK(ZZUSB_CMD_CHECK_PORT == 0x0cu);
    CHECK(ZZUSB_CMD_QUERY_CAPS == 0x0du);
    CHECK(ZZUSB_CMD_ISO_STOP == 0x16u);

    CHECK(ZZUSB_STATUS_OK == 0x00u);
    CHECK(ZZUSB_STATUS_PENDING == 0x01u);
    CHECK(ZZUSB_STATUS_ERROR == 0xffu);
    CHECK(ZZUSB_STATUS_BADPARAM == 0xf6u);
    CHECK(ZZUSB_STATUS_UNSUPPORTED == 0xf5u);
    CHECK(ZZUSB_STATUS_STALE == 0xf4u);
    CHECK(ZZUSB_STATUS_NOMEM == 0xf0u);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_PROTOCOL_V2) != 0);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_PRECISE_ERRORS) != 0);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_PERIODIC) != 0);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_EVENT_IRQ) != 0);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_ISO_SIMPLE) != 0);
    CHECK((ZZUSB_CAP_BASE & ZZUSB_CAP_ISO_REALTIME) != 0);

    make_valid_control(&cmd, &ext);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);
    cmd.setup_bRequestType = 0;
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_control(&cmd, &ext);

    put_be32(&cmd.dev_addr, 128);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_control(&cmd, &ext);
    put_be32(&ext.controller_epoch, 6);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_STALE);

    make_valid_control(&cmd, &ext);
    put_be32(&cmd.data_length, ZZUSB_V2_DATA_MAX + 1u);
    put_le16_test(&cmd.setup_wLength, 0);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_control(&cmd, &ext);
    put_be16(&cmd.flags, ZZUSB_FLAG_SPLIT);
    put_be16(&cmd.speed, ZZUSB_SPEED_HIGH);
    put_be16(&cmd.split_hub_addr, 2);
    put_be16(&cmd.split_hub_port, 1);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_bulk(&cmd, &ext, 0x80);
    put_be16(&cmd.flags, ZZUSB_FLAG_BULK_IN_POLL);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);
    put_be16(&cmd.direction, 0);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);
    put_be16(&cmd.flags, 0);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);

    make_valid_iso(&cmd, &ext, ZZUSB_CMD_ISO_QUEUE, 32);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);
    CHECK(zzusb_validate_command(&cmd, &ext, 0, 7) ==
          ZZUSB_STATUS_BADPARAM);
    put_be16(&cmd.max_pkt_size, 0x1400);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);
    put_be16(&cmd.max_pkt_size, 0x1401);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);
    put_be16(&cmd.max_pkt_size, 1024);


    put_be32(&cmd.data_length, 31);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_iso(&cmd, &ext, ZZUSB_CMD_ISO_REAP, ZZUSB_V2_DATA_MAX);
    put_be16(&cmd.interval, 17);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_iso(&cmd, &ext, ZZUSB_CMD_ISO_QUEUE, 32);
    put_be16(&cmd.speed, ZZUSB_SPEED_FULL);
    put_be16(&cmd.max_pkt_size, 1023);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);
    put_be16(&cmd.flags, ZZUSB_FLAG_SPLIT);
    put_be16(&cmd.split_hub_addr, 2);
    put_be16(&cmd.split_hub_port, 1);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);
    put_be16(&cmd.interval, 17);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_BADPARAM);

    make_valid_iso(&cmd, &ext, ZZUSB_CMD_ISO_STOP, 0);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);

    memset(&cmd, 0, sizeof(cmd));
    memset(&ext, 0, sizeof(ext));
    put_be16(&cmd.cmd, ZZUSB_CMD_QUERY_CAPS);
    put_be16(&ext.version, ZZUSB_PROTOCOL_VERSION);
    put_be16(&ext.header_size, ZZUSB_V2_HEADER_SIZE);
    put_be32(&ext.request_id, 9);
    CHECK(zzusb_validate_command(&cmd, &ext, 1, 7) ==
          ZZUSB_STATUS_OK);

    puts("USB proxy v1/v2 wire and validation contract satisfied");
    return EXIT_SUCCESS;
}
