#ifndef HCD_OHCI_H
#define HCD_OHCI_H

/*
 * QEMU IEEE 1394 API
 *
 * Copyright (c) 2016 Guardicore
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#define EVT_NO_STATUS  0x00
#define EVT_UNDERRUN   0x04
#define EVT_OVERRUN    0x05
#define EVT_DATA_READ  0x07
#define EVT_DATA_WRITE 0x08
#define EVT_BUS_RESET  0x09
#define EVT_TCODE_ERR  0x0B
#define EVT_UNKNOWN    0x0E
#define EVT_FLUSHED    0x0F
#define ACK_COMPLETE   0x11
#define ACK_PENDING    0x12

#define RESP_COMPLETE       0x00
#define RESP_CONFLICT_ERROR 0x40
#define RESP_DATA_ERROR     0x50
#define RESP_TYPE_ERROR     0x60
#define RESP_ADDRESS_ERROR  0x70

/* Layout of the flags field in packet structures below */
#define OHCI_PACKET_FLAGS_T_CODE     0x000000F0
#define OHCI_PACKET_FLAGS_RT         0x00000300
#define OHCI_PACKET_FLAGS_T_LABEL    0x0000FC00
#define OHCI_PACKET_FLAGS_SPD        0x00070000
#define OHCI_PACKET_FLAGS_SRC_BUS_ID 0x00800000

typedef union {
    uint32_t qdata[3];
    uint32_t flags;
} OHCIPacketHeader;

typedef union {
    uint32_t qdata[3];
    struct {
        uint32_t flags;
        uint16_t destination_offset_high;
        uint16_t destination_id;
        uint32_t destination_offset_low;
    };
} OHCIReqNoDataPacket;

typedef union {
    uint32_t qdata[3];
    struct {
        uint32_t flags;
        uint16_t destination_offset_high;
        uint16_t destination_id;
        uint32_t destination_offset_low;
        uint32_t data;
    };
} OHCIReqQuadletPacket;

typedef union {
    uint32_t qdata[4];
    struct {
        uint32_t flags;
        uint16_t destination_offset_high;
        uint16_t destination_id;
        uint32_t destination_offset_low;
        uint16_t padding;
        uint16_t data_length;
    };
} OHCIReqBlockPacket;

typedef union {
    uint32_t qdata[3];
    struct {
        uint16_t flags;
        uint16_t destination_id;
        uint8_t padding;
        uint8_t r_code;
        uint16_t source_id;
    };
} OHCIRspNoDataPacket;

typedef union {
    uint32_t qdata[4];
    struct {
        uint16_t flags;
        uint16_t destination_id;
        uint8_t padding1;
        uint8_t r_code;
        uint16_t source_id;
        uint32_t padding2;
        uint32_t data;
    };
} OHCIRspQuadletPacket;

typedef union {
    uint32_t qdata[4];
    struct {
        uint16_t flags;
        uint16_t destination_id;
        uint8_t padding1;
        uint8_t r_code;
        uint16_t source_id;
        uint32_t padding2;
        uint16_t padding3;
        uint16_t data_length;
    };
} OHCIRspBlockPacket;

#endif
