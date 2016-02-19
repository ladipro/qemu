/*
 * FireWire (1394) support
 *
 * Copyright (c) 2016 Guardicore
 * Authors:
 *  Itamar Tal   <itamar@guardicore.com>
 * Originally Written by James Harper
 *
 * This is a `bare-bones' implementation of the Firewire 1394 OHCI
 * for virtual->virtual firewire connections emulation
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
 *
 */

#include "hcd-ohci.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/loader.h"
#include "sysemu/sysemu.h"
#include "sysemu/char.h"
#include "qemu/timer.h"

#define OHCI_1394_MMIO_SIZE 0x800

#define HCCONTROL_RESET 16
#define HCCONTROL_LINK_ENABLE 17
#define HCCONTROL_LPS 19

#define HCCONTROL_RESET_MASK (1 << (HCCONTROL_RESET))
#define HCCONTROL_LINK_ENABLE_MASK (1 << (HCCONTROL_LINK_ENABLE))
#define HCCONTROL_LPS_MASK (1 << (HCCONTROL_LPS))

#define HCD_STATE_UNPLUGGED    0 /* no connection */
#define HCD_STATE_MAGIC        1 /* waiting for magic */
#define HCD_STATE_DISCONNECTED 2 /* waiting for link packet */
#define HCD_STATE_ARBITRATION1 3 /* send bid */
#define HCD_STATE_ARBITRATION2 4 /* receive bid and compare */
#define HCD_STATE_CONNECTED    5 /* connected and ready to go */

#define IS_Ax_ACTIVE(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] & (1 << 10))
#define SET_Ax_ACTIVE(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] |= (1 << 10))
#define CLR_Ax_ACTIVE(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] &= ~(1 << 10))

#define IS_Ax_DEAD(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] & (1 << 11))
#define SET_Ax_DEAD(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] |= (1 << 11))

#define IS_Ax_WAKE(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] & (1 << 12))
#define CLR_Ax_WAKE(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] &= ~(1 << 12))

#define IS_Ax_RUN(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] & (1 << 15))

#define SET_Ax_EVENT_CODE(n, e) s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x000) >> 2] = ((s->mmio.regs[((0x180 + \
                                      ((n) * 0x20)) + 0x00) >> 2] & \
                                      0xFFFFFFE0) | (e))

#define Ax_COMMAND_PTR(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x00C) >> 2])
#define SET_Ax_COMMAND_PTR(n, c) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x00C) >> 2] = (c))

#define Ax_CONTEXT_CONTROL(n) (s->mmio.regs[((0x180 + ((n) * 0x20)) + \
                                      0x00) >> 2])

/* interrupt event flags */
#define INT_REQ_TX_COMPLETE           0x00000001
#define INT_RESP_TX_COMPLETE          0x00000002
#define INT_ARRQ                      0x00000004
#define INT_ARRS                      0x00000008
#define INT_RQP_KT                    0x00000010
#define INT_RSP_KT                    0x00000020
#define INT_ISOCH_TX                  0x00000040
#define INT_ISOCH_RX                  0x00000080
#define INT_POSTED_WRITE_ERR          0x00000100
#define INT_LOCK_RESP_ERR             0x00000200
#define INT_SELF_ID_COMPLETE2         0x00008000
#define INT_SELF_ID_COMPLETE          0x00010000
#define INT_BUS_RESET                 0x00020000
#define INT_REG_ACCESS_FAIL           0x00040000
#define INT_PHY                       0x00080000
#define INT_CYCLE_SUNCH               0x00100000
#define INT_CYCLE_64_SECONDS          0x00200000
#define INT_CYCLE_LOST                0x00400000
#define INT_CYCLE_INCONSISTENT        0x00800000
#define INT_UNRECOVERABLE_ERROR       0x01000000
#define INT_CYCLE_TOO_LONG            0x02000000
#define INT_PHY_REG_RCVD              0x04000000
#define INT_ACK_TARDY                 0x08000000
#define INT_SOFT_INTERRUPT            0x20000000
#define INT_VENDOR_SPECIFIC           0x40000000
#define INT_MASTER_INT_ENABLE         0x80000000

/* node ID layout */
#define NODEID_NODE_NUMBER            0x0000003F
#define NODEID_BUS_NUMBER             0x0000FFC0
#define NODEID_CPS                    0x08000000
#define NODEID_ROOT                   0x40000000
#define NODEID_ID_VALID               0x80000000

/* PhyControl and its flag layout */
#define PHY_CONTROL_WR_FLAGS_REG_ADDR 0x0F
#define PHY_CONTROL_WR_FLAGS_WR_REG   0x40
#define PHY_CONTROL_WR_FLAGS_RD_REG   0x80
#define PHY_CONTROL_RD_FLAGS_RD_ADDR  0x0F
#define PHY_CONTROL_RD_FLAGS_RD_DONE  0x80

typedef union {
    uint32_t value;
    struct {
        uint8_t wr_data;
        uint8_t wr_flags;
        uint8_t rd_data;
        uint8_t rd_flags;
    };
} PhyControl;

typedef union {
    uint32_t value;
    struct {
        uint16_t rom_crc_value;
        uint8_t crc_length;
        uint8_t info_length;
    };
} ConfigROMHdr;

/* SelfIDCount and its field layout */
#define SELF_ID_COUNT_LO_WORD_SIZE 0x07FC
#define SELF_ID_COUNT_FLAGS_ERROR  0x80

typedef union {
    uint32_t value;
    struct {
        uint16_t lo_word;
        uint8_t generation;
        uint8_t flags;
    };
} SelfIDCount;

typedef struct {
    union {
       struct {
           uint32_t context_control;
           uint32_t context_control_alt;
       };
       struct {
           uint32_t context_control_set;
           uint32_t context_control_clear;
       };
    };
    uint32_t reserved_08;
    uint32_t command_ptr;
    uint32_t reserved_10;
    uint32_t reserved_14;
    uint32_t reserved_18;
    uint32_t reserved_1c;
} AsyncContext;

typedef union {
    struct {
        uint32_t version;
        uint32_t guid_rom;
        uint32_t at_retries;
        union {
            uint32_t csr_read_data;  /* 00c */
            uint32_t csr_write_data; /* 00c */
        };
        uint32_t csr_compare_data;
        uint32_t csr_control;
        ConfigROMHdr config_rom_hdr;
        uint32_t bus_id;
        uint32_t bus_options;
        uint32_t guid_hi;
        uint32_t guid_lo;
        uint32_t reserved_002c;
        uint32_t reserved_0030;
        uint32_t config_rom_map;
        uint32_t posted_write_address_lo;
        uint32_t posted_write_address_hi;
        uint32_t vendor_id;
        uint32_t reserved_0044;
        uint32_t reserved_0048;
        uint32_t reserved_004c;
        union {
            struct {
                /* read */
                uint32_t hc_control;
                uint32_t hc_control_alt;
            };
            struct {
                /* write */
                uint32_t hc_control_set;
                uint32_t hc_control_clear;
            };
        };
        uint32_t reserved_0058;
        uint32_t reserved_005c;
        uint32_t reserved_0060;
        uint32_t self_id_buffer;
        SelfIDCount self_id_count;
        uint32_t reserved_006c;
        union {
            struct {
                /* read */
                uint32_t ir_multi_chan_mask_hi;
                uint32_t ir_multi_chan_mask_hi_alt;
            };
            struct {
                /* write */
                uint32_t ir_multi_chan_mask_hi_set;
                uint32_t ir_multi_chan_mask_hi_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t ir_multi_chan_mask_lo;
                uint32_t ir_multi_chan_mask_lo_alt;
            };
            struct {
                /* write */
                uint32_t ir_multi_chan_mask_lo_set;
                uint32_t ir_multi_chan_mask_lo_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t int_event;            /* 0080 */
                uint32_t int_event_masked;     /* 0084 */
            };
            struct {
                /* write */
                uint32_t int_event_set;        /* 0080 */
                uint32_t int_event_clear;      /* 0084 */
            };
        };
        union {
            struct {
                /* read */
                uint32_t int_mask;
                uint32_t int_mask_alt;
            };
            struct {
                /* write */
                uint32_t int_mask_set;
                uint32_t int_mask_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t iso_xmit_int_event;
                uint32_t iso_xmit_int_event_masked;
            };
            struct {
                /* write */
                uint32_t iso_xmit_int_event_set;
                uint32_t iso_xmit_int_event_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t iso_xmit_int_mask;
                uint32_t iso_xmit_int_mask_alt;
            };
            struct {
                /* write */
                uint32_t iso_xmit_int_mask_set;
                uint32_t iso_xmit_int_mask_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t iso_recv_int_event;
                uint32_t iso_recv_int_event_masked;
            };
            struct {
                /* write */
                uint32_t iso_recv_int_event_set;
                uint32_t iso_recv_int_event_clear;
            };
        };
        union {
            struct {
                /* read */
                uint32_t iso_recv_int_mask;
                uint32_t iso_recv_int_mask_alt;
            };
            struct {
                /* write */
                uint32_t iso_recv_int_mask_set;
                uint32_t iso_recv_int_mask_clear;
            };
        };
        uint32_t initial_bandwidth_available;   /* 00B0 */
        uint32_t initial_channels_available_hi; /* 00B4 */
        uint32_t initial_channels_available_lo; /* 00B8 */
        uint32_t reserved_00bc;
        uint32_t reserved_00c0;
        uint32_t reserved_00c4;
        uint32_t reserved_00c8;
        uint32_t reserved_00dc;
        uint32_t reserved_00d0;
        uint32_t reserved_00d4;
        uint32_t reserved_00d8;
        uint32_t fairness_control; /* 00dc */
        union {
            struct {
                /* read */
                uint32_t link_control;       /* 0xe0 */
                uint32_t link_control_alt;   /* 0xe4 */
            };
            struct {
                /* write */
                uint32_t link_control_set;   /* 0xe0 */
                uint32_t link_control_clear; /* 0xe4 */
            };
        };
        uint32_t node_id;                                  /* 00e8 */
        PhyControl phy_control;                            /* 00ec */
        uint32_t isochronous_cycle_timer;                  /* 00f0 */
        uint32_t reserved_00f4;
        uint32_t reserved_00f8;
        uint32_t reserved_00fc;
        union {
            struct {
                /* read */
                uint32_t asynchronous_request_filter_hi;     /* 0100 */
                uint32_t asynchronous_request_filter_hi_alt; /* 0104 */
            };
            struct {
                /* write */
                uint32_t asynchronous_request_filter_hi_set;   /* 0100 */
                uint32_t asynchronous_request_filter_hi_clear; /* 0104 */
            };
        };
        union {
            struct {
                /* read */
                uint32_t asynchronous_request_filter_lo;      /* 0108 */
                uint32_t asynchronous_request_filter_lo_alt;  /* 010c */
            };
            struct {
                /* write */
                uint32_t asynchronous_request_filter_lo_set;   /* 0108 */
                uint32_t asynchronous_request_filter_lo_clear; /* 010c */
            };
        };
        union {
            struct {
                /* read */
                uint32_t physical_request_filter_hi;          /* 0110 */
                uint32_t physical_request_filter_hi_alt;      /* 0114 */
            };
            struct {
                /* write */
                uint32_t physical_request_filter_hi_set;      /* 0110 */
                uint32_t physical_request_filter_hi_clear;    /* 0114 */
            };
        };
        union {
            struct {
                /* read */
                uint32_t physical_request_filter_lo;          /* 0118 */
                uint32_t physical_request_filter_lo_alt;      /* 011c */
            };
            struct {
                /* write */
                uint32_t physical_request_filter_lo_set;      /* 0118 */
                uint32_t physical_request_filter_lo_clear;    /* 011c */
            };
        };
        uint32_t physical_upper_bound; /* 0120 */
        uint32_t reserved_0124;
        uint32_t reserved_0128;
        uint32_t reserved_012c;
        uint32_t reserved_0130;
        uint32_t reserved_0134;
        uint32_t reserved_0138;
        uint32_t reserved_013c;
        uint32_t reserved_0140;
        uint32_t reserved_0144;
        uint32_t reserved_0148;
        uint32_t reserved_014c;
        uint32_t reserved_0150;
        uint32_t reserved_0154;
        uint32_t reserved_0158;
        uint32_t reserved_015c;
        uint32_t reserved_0160;
        uint32_t reserved_0164;
        uint32_t reserved_0168;
        uint32_t reserved_016c;
        uint32_t reserved_0170;
        uint32_t reserved_0174;
        uint32_t reserved_0178;
        uint32_t reserved_017c;
        AsyncContext async_request_transmit;
        AsyncContext async_response_transmit;
        AsyncContext async_request_receive;
        AsyncContext async_response_receive;
        /* Isoch stuff */
    };
    uint32_t regs[OHCI_1394_MMIO_SIZE >> 2];
} OHCIDeviceRegs;

/* OHCIPhyState and its register layout */
#define PHY_REG0_CPS         0x01
#define PHY_REG0_ROOT        0x02
#define PHY_REG0_PHYSICAL_ID 0xFC
#define PHY_REG1_GAP_COUNT   0x3F
#define PHY_REG1_IBR         0x40
#define PHY_REG1_RHB         0x80
#define PHY_REG2_NUM_PORTS   0x0F
#define PHY_REG2_EXTENDED    0xE0
#define PHY_REG3_DELAY       0x0F
#define PHY_REG3_PHY_SPEED   0xE0
#define PHY_REG4_PWR_CLASS   0x07
#define PHY_REG4_JITTER      0x38
#define PHY_REG4_C           0x40
#define PHY_REG4_L           0x80
#define PHY_REG5_EMC         0x01
#define PHY_REG5_EAA         0x02
#define PHY_REG5_PEI         0x04
#define PHY_REG5_STOI        0x08
#define PHY_REG5_CPSI        0x10
#define PHY_REG5_CTOI        0x20
#define PHY_REG5_ISBR        0x40
#define PHY_REG5_RPIE        0x80
#define PHY_REG7_PORT_SELECT 0x0F
#define PHY_REG7_PAGE_SELECT 0xE0

typedef union {
    uint8_t bytes[16];
    struct {
        uint8_t reg0;
        uint8_t reg1;
        uint8_t reg2;
        uint8_t reg3;
        uint8_t reg4;
        uint8_t reg5;
        uint8_t reg6;
        uint8_t reg7;
    };
} OHCIPhyState;

/* OHCISelfID and its register layout */
#define SELF_ID_REG0_M         0x01 /* = 00 */
#define SELF_ID_REG0_INITIATED 0x02 /* = 02 for root node */
#define SELF_ID_REG0_P2        0x0C /* = 00 */
#define SELF_ID_REG0_P1        0x30 /* = 00 */
#define SELF_ID_REG0_P0        0xC0 /* = C0, maybe 80 when "child" to root */
#define SELF_ID_REG1_PWR       0x07 /* = 00 */
#define SELF_ID_REG1_C         0x08 /* = 08 when root */
#define SELF_ID_REG1_DEL       0x30 /* = 00? */
#define SELF_ID_REG1_SP        0xC0 /* = 00? */
#define SELF_ID_REG2_GAP_CNT   0x3F /* = 00? */
#define SELF_ID_REG2_L         0x40 /* = 40? maybe just when connected */
#define SELF_ID_REG3_NODE_ID   0x3F
#define SELF_ID_REG3_TYPE      0xC0 /* = 80 */

typedef union {
    uint32_t val;
    struct {
        uint8_t reg0;
        uint8_t reg1;
        uint8_t reg2;
        uint8_t reg3;
    };
} OHCISelfID;

struct OHCI1394State;
typedef struct OHCI1394State OHCI1394State;

/* HCDAtDB and its flag layout */
#define HCD_AT_DB_FLAGS_BRANCH    0x000C
#define HCD_AT_DB_FLAGS_INTERRUPT 0x0030
#define HCD_AT_DB_FLAGS_PING      0x0080
#define HCD_AT_DB_FLAGS_KEY       0x0700
#define HCD_AT_DB_FLAGS_CMD       0xF000

typedef struct {
    uint16_t req_count;
    uint16_t flags;
    uint32_t data_address;
    uint32_t branch_address;
    uint16_t timestamp;
    uint16_t transfer_status;
} HCDAtDB;

/* HCDArDB and its flag layout */
#define HCD_AR_DB_FLAGS_BRANCH    0x000C
#define HCD_AR_DB_FLAGS_INTERRUPT 0x0030
#define HCD_AR_DB_FLAGS_KEY       0x0700
#define HCD_AR_DB_FLAGS_STATUS    0x0800
#define HCD_AR_DB_FLAGS_CMD       0xF000

typedef struct {
    uint16_t req_count;
    uint16_t flags;
    uint32_t data_address;
    uint32_t branch_address;
    uint16_t res_count;
    uint16_t transfer_status;
} HCDArDB;

typedef struct {
    OHCI1394State *s;
    QEMUTimer *timer;
    uint32_t num; /* base register is 0x180 + num * 0x20 */
    uint32_t address; /* current address */
    uint32_t response;
} HCDTimerState;

struct OHCI1394State {
    PCIDevice pci_dev;
    MemoryRegion mmio_bar;
    OHCIDeviceRegs mmio;
    HCDTimerState at_req_timer;
    HCDTimerState at_rsp_timer;
    OHCIPhyState phy;
    uint8_t phy_pages[8][8];
    qemu_irq irq;
    uint32_t irq_asserted;
    /* properties from init */
    CharBackend chr;
    int state;
    int other_link;
    uint16_t bid;
    int root;
    int bufpos;
    uint8_t buf[16 + 65536]; /* maximum request size + maximum data size */
};

#define REG_OFFSET(field)  offsetof(OHCIDeviceRegs, field)

static void hcd_bus_reset(OHCI1394State *s);
static void hcd_chr_event(void *opaque, int event);

static void
hcd_check_irq(OHCI1394State *s) {
    if ((s->mmio.int_mask & 0x80000000) &&
        (s->mmio.int_event & s->mmio.int_mask)) {
        if (!s->irq_asserted) {
            qemu_set_irq(s->irq, 1);
            s->irq_asserted = 1;
        }
    } else {
        if (s->irq_asserted) {
            qemu_set_irq(s->irq, 0);
            s->irq_asserted = 0;
        }
    }
}

static void
hcd_soft_reset(OHCI1394State *s) {
    s->mmio.bus_options = 0x00008002; /* 5.11 */
    s->mmio.hc_control &= 0x00C00000; /* 5.7.2 */
}

static void
hcd_hard_reset(OHCI1394State *s) {
    memset(&s->mmio, 0, sizeof(s->mmio));
    s->mmio.version = 0x00010010; /* Release 1.1 of OHCI spec */
    s->mmio.bus_id = 0x31333934; /* 1394 */
    s->mmio.bus_options = 0x00008002; /* 5.11 */
    s->mmio.guid_hi = 0x89abcdef;
    s->mmio.guid_lo = 0x01234567;
    memset(&s->phy, 0, sizeof(s->phy));
    s->phy.reg2 = 1 | (s->phy.reg2 & ~PHY_REG2_NUM_PORTS);
    s->phy.reg4 |= PHY_REG4_L;
    s->phy.reg4 |= PHY_REG4_C;
    s->phy_pages[0][0] = 0x08; /* 0xFE; */
    hcd_soft_reset(s);
}

static void
hcd_complete_self_id(OHCI1394State *s) {
    s->mmio.node_id = (s->root) ? 0 : 1; /* 5.11 */
    s->mmio.node_id |= (0x3ff << 6);     /* busNumber */
    if (s->state == HCD_STATE_CONNECTED) {
        s->mmio.node_id |= NODEID_CPS;
    }
    if (s->root) {
        s->mmio.node_id |= NODEID_ROOT;
    }
    s->mmio.node_id |= NODEID_ID_VALID;
    s->mmio.self_id_count.lo_word &= ~SELF_ID_COUNT_LO_WORD_SIZE;
    s->mmio.self_id_count.flags &= ~SELF_ID_COUNT_FLAGS_ERROR;
    if (s->mmio.link_control & 0x00000200) { /* if RcvSelfID */
        uint32_t tmp = 0;
        OHCISelfID sid;

        sid.val = 0;
        sid.reg0 |= SELF_ID_REG0_INITIATED;
        sid.reg0 |= (0x80 & SELF_ID_REG0_P0);
        sid.reg1 |= SELF_ID_REG1_C;
        sid.reg2 |= SELF_ID_REG2_L;
        sid.reg3 |= (0x80 & SELF_ID_REG3_TYPE);
        dma_memory_write(&address_space_memory,
                         s->mmio.self_id_buffer + 4,
                         &sid.val, 4);
        sid.val = ~sid.val;
        dma_memory_write(&address_space_memory,
                         s->mmio.self_id_buffer + 8,
                         &sid.val, 4);
        s->mmio.self_id_count.lo_word += 8;

        if (s->state == HCD_STATE_CONNECTED) {
            sid.val = 0;
            sid.reg0 |= SELF_ID_REG0_P0;
            sid.reg2 |= SELF_ID_REG2_L;
            sid.reg3 = 1; /* node id */
            sid.reg3 |= (0x80 & SELF_ID_REG3_TYPE);
            dma_memory_write(&address_space_memory,
                             s->mmio.self_id_buffer + 12,
                             &sid.val, 4);
            sid.val = ~sid.val;
            dma_memory_write(&address_space_memory,
                             s->mmio.self_id_buffer + 16,
                             &sid.val, 4);
            s->mmio.self_id_count.lo_word += 8;
        }

        tmp = (s->mmio.self_id_count.generation << 16) | 1;
        dma_memory_write(&address_space_memory,
                         s->mmio.self_id_buffer,
                         &tmp, 4);
        s->mmio.self_id_count.lo_word += 4;
    }
    s->mmio.int_event |= 0x00018000; /* selfIDcomplete | selfIDcomplete2 */
    hcd_check_irq(s);
}

static void
hcd_async_rx_rsp_packet(OHCI1394State *s, uint8_t *buf, uint32_t size,
                        uint8_t response) {
    int num = 3;
    HCDArDB db;
    uint32_t data_address = 0;
    uint32_t status = 0;
    int state = 0;

    if (size == 0) {
        return;
    }
    SET_Ax_EVENT_CODE(num, response);
    dma_memory_read(&address_space_memory,
                    Ax_COMMAND_PTR(num) & 0xFFFFFFF0,
                    &db, sizeof(db));
    data_address = db.data_address + db.req_count - db.res_count;
    while (state != 3) {
        int write_size;

        db.transfer_status =
                        s->mmio.async_response_receive.context_control & 0xFFFF;
        if (db.res_count == 0) {
            dma_memory_write(&address_space_memory,
                             Ax_COMMAND_PTR(num) & 0xFFFFFFF0,
                             &db, sizeof(db));
            if (db.branch_address == 0) {
                CLR_Ax_ACTIVE(num);
                /* TODO: need to roll back if this happens */
            }
            SET_Ax_COMMAND_PTR(num, db.branch_address);
            dma_memory_read(&address_space_memory,
                            Ax_COMMAND_PTR(num) & 0xFFFFFFF0,
                            &db, sizeof(db));
            data_address = db.data_address + db.req_count - db.res_count;
        }
        switch (state) {
        case 0:
            if (db.res_count > size) {
                write_size = size;
            } else {
                write_size = db.res_count;
            }
            dma_memory_write(&address_space_memory,
                             data_address,
                             buf, write_size);
            db.res_count -= write_size;
            data_address += write_size;
            size -= write_size;
            buf += write_size;
            if (size == 0) {
                state = 1;
            }
            break;
        case 1:
            status = s->mmio.async_response_receive.context_control << 16;
            db.transfer_status =
                        s->mmio.async_response_receive.context_control & 0xFFFF;
            dma_memory_write(&address_space_memory, data_address, &status, 4);
            db.res_count -= 4;
            data_address += 4;
            dma_memory_write(&address_space_memory,
                             Ax_COMMAND_PTR(num) & 0xFFFFFFF0,
                             &db, sizeof(db));
            state = 2;
            break;
        case 2:
            /* this state exists to go around the loop again and update the db
            if required */
            state = 3;
            break;
        }
    }
    s->mmio.int_event |= (1 << 5);
    hcd_check_irq(s);
}

static void
hcd_async_rx_run(OHCI1394State *s, uint32_t addr) {
    int num;

    num = (addr & 0x0180) >> 7;
    SET_Ax_ACTIVE(num);
}

static void
hcd_async_rx_stop(OHCI1394State *s, uint32_t addr) {
    int num;

    num = (addr & 0x0180) >> 7;
    CLR_Ax_ACTIVE(num);
}

static void
hcd_async_rx_wake(OHCI1394State *s, uint32_t addr) {
    uint32_t address;
    HCDArDB db;
    int num;

    num = (addr & 0x0180) >> 7;
    if (IS_Ax_ACTIVE(num)) {
        return;
    }
    address = s->mmio.regs[(addr >> 2) + 0x00c];
    dma_memory_read(&address_space_memory,
                    address & 0xFFFFFFF0,
                    &db, sizeof(db));
    if ((db.branch_address & 0x0000000f) != 0) {
        SET_Ax_ACTIVE(num);
        SET_Ax_COMMAND_PTR(num, db.branch_address);
    }
}

static void
hcd_at_run(HCDTimerState *t) {
    OHCI1394State *s = t->s;
    t->address = Ax_COMMAND_PTR(t->num) & 0xfffffff0;
    t->response = EVT_TCODE_ERR;
    SET_Ax_ACTIVE(t->num);
}

static void
hcd_at_timer(void *o) {
    HCDTimerState *t = (HCDTimerState *)o;
    OHCI1394State *s = t->s;
    OHCIPacketHeader packet_header;
    HCDAtDB db;

    if (IS_Ax_DEAD(t->num) || !IS_Ax_RUN(t->num)) {
        CLR_Ax_WAKE(t->num);
        CLR_Ax_ACTIVE(t->num);
        return;
    }
    if (!IS_Ax_ACTIVE(t->num)) {
        if (!IS_Ax_WAKE(t->num)) {
            return;
        }
        CLR_Ax_WAKE(t->num);
        dma_memory_read(&address_space_memory,
                        t->address, &db,
                        sizeof(db));
        if (!(db.branch_address & 0x0000000f)) {
            return;
        }
        SET_Ax_COMMAND_PTR(t->num, db.branch_address);
        hcd_at_run(t); /* also sets active */
    }
    CLR_Ax_WAKE(t->num);
    dma_memory_read(&address_space_memory,
                    t->address, &db,
                    sizeof(db));
    if ((db.flags & HCD_AT_DB_FLAGS_CMD) == 0 &&
        (db.flags & HCD_AT_DB_FLAGS_KEY) == 0) {
        /* Do nothing */
    } else if ((db.flags & HCD_AT_DB_FLAGS_CMD) == 0 &&
        (db.flags & HCD_AT_DB_FLAGS_KEY) == 0x0200) {
        /* OUTPUT_MORE_Immediate */
    } else if ((db.flags & HCD_AT_DB_FLAGS_CMD) == 0x1000 &&
        (db.flags & HCD_AT_DB_FLAGS_KEY) == 0) {
        /* OUTPUT_LAST */
    } else if ((db.flags & HCD_AT_DB_FLAGS_CMD) == 0x1000 &&
        (db.flags & HCD_AT_DB_FLAGS_KEY) == 0x0200) {
        /* OUTPUT_LAST_Immediate */
    } else {
        /* UNKNOWN COMMAND */
        return ;
    }

    switch (db.flags & HCD_AT_DB_FLAGS_KEY) {
    case 0: { /* non-Immediate */
        uint8_t buf[65536];
        dma_memory_read(&address_space_memory,
                        db.data_address, buf,
                        db.req_count);
        qemu_chr_fe_write(&s->chr, buf, db.req_count);
        break;
    }
    case 0x0200: { /* Immediate */
        uint32_t data[4];
        dma_memory_read(&address_space_memory,
                        t->address + sizeof(db),
                        data, db.req_count);

        packet_header = *(OHCIPacketHeader *)data;
        switch (packet_header.flags & OHCI_PACKET_FLAGS_T_CODE) {
        case 0x00: { /* quadlet write - quadlet format */
            OHCIReqQuadletPacket at_packet = *(OHCIReqQuadletPacket *)data;
            qemu_chr_fe_write(&s->chr, (uint8_t *)data, sizeof(at_packet));
            t->response = ACK_PENDING;
            break;
        }
        case 0x10: { /* block write - block write format */
            qemu_chr_fe_write(&s->chr, (uint8_t *)data, db.req_count);
            t->response = ACK_PENDING;
            break;
        }
        case 0x40: { /* quadlet read - nodata format */
            qemu_chr_fe_write(&s->chr, (uint8_t *)data, db.req_count);
            t->response = ACK_PENDING;
            break;
        }
        case 0x50: { /* read bytes from target */
            qemu_chr_fe_write(&s->chr, (uint8_t *)data, db.req_count);
            t->response = ACK_PENDING;
            break;
        }
        case 0xe0: { /* PHY packet */
            /* probably just configuring the gap count... */
            t->response = ACK_COMPLETE;
            /* reset because PHY packet */
            hcd_bus_reset(s); /* not all PHY packets require reset... */
            break;
        }
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    if ((db.flags & HCD_AT_DB_FLAGS_CMD) == 0) { /* more */
        if ((db.flags & HCD_AT_DB_FLAGS_KEY) == 0x0200) {
            t->address += sizeof(db) + sizeof(int32_t) * 4;
        } else {
            t->address += sizeof(db);
        }
    } else { /* last */
        if ((db.flags & HCD_AT_DB_FLAGS_INTERRUPT) == 0x0030) {
            s->mmio.int_event |= (1 << t->num);
        }
        SET_Ax_EVENT_CODE(t->num, t->response);
        db.transfer_status = (uint16_t)Ax_CONTEXT_CONTROL(t->num);
        dma_memory_write(&address_space_memory, t->address, &db, sizeof(db));
        if ((db.branch_address & 0x0000000f) == 0) {
            CLR_Ax_ACTIVE(t->num);
            return;
        }
        SET_Ax_COMMAND_PTR(t->num, db.branch_address);
        hcd_at_run(t);
    }
    timer_mod(t->timer, 0);
    /* maybe
    t_now + get_ticks_per_sec() / 100000);
    100/sec isn't going to be right */
}

static void
hcd_bus_reset(OHCI1394State *s) {
    uint32_t bus_reset_packet[3] = {0x000000e0, 0x00000000, 0x00000000};
    s->mmio.node_id = (s->mmio.node_id & ~NODEID_BUS_NUMBER) | (0x3ff << 6);
    s->mmio.node_id &= ~NODEID_CPS;
    s->mmio.node_id &= ~NODEID_ROOT;
    s->mmio.node_id &= ~NODEID_ID_VALID;
    s->mmio.self_id_count.generation++;
    s->mmio.int_event |= 0x00020000; /* bus reset complete */
    if (s->state != HCD_STATE_CONNECTED) {
        s->root = 1;
    }
    s->mmio.async_request_transmit.context_control &= 0xFFFFFBFF;
    s->mmio.async_response_transmit.context_control &= 0xFFFFFBFF;
    if (s->mmio.async_response_receive.context_control & 0x00008000) {
        bus_reset_packet[2] |= s->mmio.self_id_count.generation << 16;
        hcd_async_rx_rsp_packet(s, (uint8_t *)bus_reset_packet,
                                sizeof(bus_reset_packet), EVT_BUS_RESET);
    }
    hcd_complete_self_id(s);
}

static uint8_t
hcd_phy_read(OHCI1394State *s, uint8_t reg) {
    if (reg < 8) {
        return s->phy.bytes[reg];
    } else {
        int page = (s->phy.reg7 & PHY_REG7_PAGE_SELECT) >> 5;
        return s->phy_pages[page][reg & 7];
    }
}

static void
hcd_phy_write(OHCI1394State *s, uint8_t reg, uint8_t data) {
    if (reg < 8) {
        switch (reg) {
        case 0: /* not allowed? */
            break;
        case 1:
            s->phy.bytes[reg] = data & 0xBF;
            if (data & 0x40) {
                hcd_bus_reset(s);
            }
            break;
        case 5:
            s->phy.bytes[reg] = data & 0xBF;
            if (data & 0x40) {
                hcd_bus_reset(s);
            }
            break;
        default:
            s->phy.bytes[reg] = data;
            break;
        }
    } else {
        int page = (s->phy.reg7 & PHY_REG7_PAGE_SELECT) >> 5;
        s->phy_pages[page][reg & 7] = data;
    }
}

static uint64_t hcd_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t ret;
    OHCI1394State *s = (OHCI1394State *)opaque;
    switch (addr) {
    case REG_OFFSET(int_event_masked): /* 0084 */
        ret = s->mmio.int_event & s->mmio.int_mask;
         break;
    default:
        ret = s->mmio.regs[addr >> 2];
        break;
    }
    if (addr != 0x0080) {
        /* what to do? */
    }
    return ret;
}

static void hcd_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    OHCI1394State *s = (OHCI1394State *)opaque;
    PhyControl phy_control;
    uint8_t reg_addr;

    if ((addr & 0xFFE0) != 0x0100) {
        /* what to do? */
    }
    switch (addr) {
    case REG_OFFSET(csr_control):     /* 014 */
        switch (data & 0x00000003) {
        case 0: /* BUS_MANAGER_ID */
            /* TODO: set bus manager somewhere... */
            s->mmio.csr_read_data = s->mmio.csr_compare_data;
            s->mmio.csr_control = 0x80000000 | (data & 0x00000003);
            break;
        case 1: /* BANDWIDTH_AVAILABLE */
            /* not actioned */
            s->mmio.csr_control = 0x80000000 | (data & 0x00000003);
            break;
        case 2: /* CHANNELS_AVAILABLE_HI */
            /* not actioned */
            s->mmio.csr_control = 0x80000000 | (data & 0x00000003);
            break;
        case 3: /* CHANNELS_AVAILABLE_LO */
            /* not actioned */
            s->mmio.csr_control = 0x80000000 | (data & 0x00000003);
            break;
        }
        break;
    case REG_OFFSET(hc_control_set): /* 0050 */
        data &= 0xE0CF0000;
        s->mmio.hc_control |= data;
        if (data & HCCONTROL_RESET_MASK) {
            /* do a reset */
            hcd_soft_reset(s);
        }
        if (data & HCCONTROL_LINK_ENABLE_MASK) {
            if ((s->state == HCD_STATE_DISCONNECTED) ||
                (s->state == HCD_STATE_ARBITRATION1)) {
                uint32_t buf = 0xFFFFFFFF;
                qemu_chr_fe_write(&s->chr, (uint8_t *)&buf, 4);
            }
        }
        break;
    case REG_OFFSET(hc_control_clear): /* 0054 */
        data &= 0xE0CE0000;
        s->mmio.hc_control &= ~data;
        break;
    case REG_OFFSET(ir_multi_chan_mask_hi_set): /* 0070 */
    case REG_OFFSET(ir_multi_chan_mask_lo_set): /* 0078 */
        s->mmio.regs[(addr >> 2) & 0xFFFE] |= data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        break;
    case REG_OFFSET(ir_multi_chan_mask_hi_clear): /* 0074 */
    case REG_OFFSET(ir_multi_chan_mask_lo_clear): /* 007c */
        s->mmio.regs[(addr >> 2) & 0xFFFE] &= ~data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        break;
    case REG_OFFSET(int_event_set): /* 0080 */
    case REG_OFFSET(int_mask_set): /* 0088 */
    case REG_OFFSET(iso_xmit_int_mask_set):
    case REG_OFFSET(iso_recv_int_mask_set):
    case REG_OFFSET(link_control_set):
        s->mmio.regs[(addr >> 2) & 0xFFFE] |= data;
        break;
    case REG_OFFSET(int_event_clear): /* 0084 */
    case REG_OFFSET(int_mask_clear): /* 008c */
    case REG_OFFSET(iso_xmit_int_mask_clear):
    case REG_OFFSET(iso_recv_int_mask_clear):
    case REG_OFFSET(link_control_clear):
        s->mmio.regs[(addr >> 2) & 0xFFFE] &= ~data;
        break;
    case REG_OFFSET(node_id): /* 00E8 */
        s->mmio.node_id =
            (s->mmio.node_id & ~NODEID_BUS_NUMBER) | (data & NODEID_BUS_NUMBER);
        break;
    case REG_OFFSET(phy_control): /* 00ec */
        *(uint32_t *)&phy_control = data;
        reg_addr = phy_control.wr_flags & PHY_CONTROL_WR_FLAGS_REG_ADDR;
        s->mmio.phy_control.wr_flags = reg_addr |
            (s->mmio.phy_control.wr_flags & ~PHY_CONTROL_WR_FLAGS_REG_ADDR);
        if (phy_control.wr_flags & PHY_CONTROL_WR_FLAGS_RD_REG) {
            s->mmio.phy_control.rd_flags = reg_addr |
                (s->mmio.phy_control.rd_flags & ~PHY_CONTROL_RD_FLAGS_RD_ADDR);
            s->mmio.phy_control.rd_data = hcd_phy_read(s, reg_addr);
            s->mmio.phy_control.rd_flags |= PHY_CONTROL_RD_FLAGS_RD_DONE;
            s->mmio.int_event |= INT_PHY_REG_RCVD;
        }
        if (phy_control.wr_flags & PHY_CONTROL_WR_FLAGS_WR_REG) {
            hcd_phy_write(s, reg_addr, phy_control.wr_data);
            s->mmio.phy_control.wr_data = phy_control.wr_data;
            s->mmio.phy_control.rd_flags &= ~PHY_CONTROL_RD_FLAGS_RD_DONE;
        }
        break;
    case REG_OFFSET(asynchronous_request_filter_hi_set): /* 0x100 */
    case REG_OFFSET(asynchronous_request_filter_lo_set): /* 0x108 */
    case REG_OFFSET(physical_request_filter_hi_set): /* 0x110 */
    case REG_OFFSET(physical_request_filter_lo_set): /* 0x118 */
        s->mmio.regs[(addr >> 2) & 0xFFFE] |= data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        break;

    case REG_OFFSET(asynchronous_request_filter_hi_clear): /* 0x104 */
    case REG_OFFSET(asynchronous_request_filter_lo_clear): /* 0x10c */
    case REG_OFFSET(physical_request_filter_hi_clear): /* 0x114 */
    case REG_OFFSET(physical_request_filter_lo_clear): /* 0x11c */
        s->mmio.regs[(addr >> 2) & 0xFFFE] &= ~data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        break;
    case REG_OFFSET(async_request_transmit.context_control_set):
    case REG_OFFSET(async_response_transmit.context_control_set):
        data &= 0x00009000;
        s->mmio.regs[(addr >> 2) & 0xFFFE] |= data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        if (data & 0x00009000) {
            HCDTimerState *t = NULL;
            if (addr ==
                REG_OFFSET(async_request_transmit.context_control_set)) {
                t = &s->at_req_timer;
            } else {
                t = &s->at_rsp_timer;
            }
            if (data & 0x00008000) {
                hcd_at_run(t);
            }
            timer_mod(t->timer, 0);
        }
        break;
    case REG_OFFSET(async_request_receive.context_control_set):
    case REG_OFFSET(async_response_receive.context_control_set):
        data &= 0x00009000;
        s->mmio.regs[(addr >> 2) & 0xFFFE] |= data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        if (data & 0x00008000) {
            hcd_async_rx_run(s, addr & 0xFFE0);
        }
        if (data & 0x00001000) {
            hcd_async_rx_wake(s, addr & 0xFFE0);
        }
        break;
    case REG_OFFSET(async_request_transmit.context_control_clear):
    case REG_OFFSET(async_response_transmit.context_control_clear):
        s->mmio.regs[(addr >> 2) & 0xFFFE] &= ~data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        break;
    case REG_OFFSET(async_request_receive.context_control_clear):
    case REG_OFFSET(async_response_receive.context_control_clear):
        data &= 0x00008000;
        s->mmio.regs[(addr >> 2) & 0xFFFE] &= ~data;
        s->mmio.regs[((addr >> 2) & 0xFFFE) + 1] =
                                          s->mmio.regs[((addr >> 2) & 0xFFFE)];
        if (data & 0x00008000) {
            hcd_async_rx_stop(s, addr & 0xFFE0);
        }
        break;
    default:
        s->mmio.regs[addr >> 2] = data;
        break;
    }
    hcd_check_irq(s);
}

static const MemoryRegionOps hcd_mmio_ops = {
    .read = hcd_mmio_read,
    .write = hcd_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int hcd_chr_can_receive(void *opaque)
{
    if (runstate_check(RUN_STATE_INMIGRATE)) {
        /* this seems to race with the restore RUN_STATE_INMIGRATE */
        return 0;
    } else {
        return 8192;
    }
}

static void hcd_fill_buffer(OHCI1394State *s, const uint8_t **buf, int *len,
                            int required)
{
    int to_copy;

    if (s->bufpos >= required) {
        return;
    }
    if (required - s->bufpos > *len) {
        to_copy = *len;
    } else {
        to_copy = required - s->bufpos;
    }

    memcpy(s->buf + s->bufpos, *buf, to_copy);
    *buf += to_copy;
    *len -= to_copy;
    s->bufpos += to_copy;
}

static bool hcd_chr_request_quadlet_write(OHCI1394State *s,
                                          const uint8_t **buf, int *len)
{
    OHCIReqQuadletPacket *req_packet;
    OHCIRspNoDataPacket rsp_packet;

    hcd_fill_buffer(s, buf, len, sizeof(*req_packet));
    if (s->bufpos < sizeof(*req_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 00 */
        return false;
    }
    if (s->bufpos > sizeof(*req_packet)) {
        /* overflow HCD_STATE_CONNECTED 00 */
        return false;
    }
    req_packet = (OHCIReqQuadletPacket *)s->buf;
    dma_memory_write(&address_space_memory,
                     req_packet->destination_offset_low,
                     &req_packet->data, sizeof(uint32_t));
    /* forward to ar or handle here?? */
    rsp_packet.flags = 0x20; /* t_code */
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_RT);
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_T_LABEL);
    rsp_packet.r_code = RESP_COMPLETE;
    rsp_packet.destination_id = req_packet->destination_id ^ 1;
    rsp_packet.source_id = req_packet->destination_id;
    qemu_chr_fe_write(&s->chr, (uint8_t *)&rsp_packet, sizeof(rsp_packet));
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_request_block_write(OHCI1394State *s,
                                        const uint8_t **buf, int *len)
{
    OHCIReqBlockPacket *req_packet;
    OHCIRspNoDataPacket rsp_packet;

    hcd_fill_buffer(s, buf, len, sizeof(*req_packet));
    if (s->bufpos < sizeof(*req_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 01 */
        return false;
    }
    req_packet = (OHCIReqBlockPacket *)s->buf;
    hcd_fill_buffer(s, buf, len,
                    sizeof(*req_packet) + req_packet->data_length);
    if (s->bufpos < (sizeof(*req_packet) +
                      req_packet->data_length)) {
        /* not enough data yet HCD_STATE_CONNECTED 01 */
        return false;
    }
    if (s->bufpos > (sizeof(*req_packet) +
                      req_packet->data_length)) {
        /* overflow HCD_STATE_CONNECTED 01 */
        return false;
    }
    dma_memory_write(&address_space_memory,
                     req_packet->destination_offset_low,
                     s->buf + sizeof(*req_packet),
                     req_packet->data_length);
    /* forward to ar or handle here?? */
    rsp_packet.flags = 0x20; /* t_code */
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_RT);
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_T_LABEL);
    rsp_packet.r_code = RESP_COMPLETE;
    rsp_packet.destination_id = req_packet->destination_id ^ 1;
    rsp_packet.source_id = req_packet->destination_id;
    qemu_chr_fe_write(&s->chr, (uint8_t *)&rsp_packet, sizeof(rsp_packet));
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_response_quadlet_write(OHCI1394State *s,
                                           const uint8_t **buf, int *len)
{
    OHCIRspNoDataPacket *rsp_packet;

    hcd_fill_buffer(s, buf, len, sizeof(*rsp_packet));
    if (s->bufpos < sizeof(*rsp_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 02 */
        return false;
    }
    if (s->bufpos > sizeof(*rsp_packet)) {
        /* overflow HCD_STATE_CONNECTED 02 */
        return false;
    }
    rsp_packet = (OHCIRspNoDataPacket *)s->buf;
    hcd_async_rx_rsp_packet(s, (uint8_t *)rsp_packet,
                            sizeof(*rsp_packet), ACK_COMPLETE);
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_request_quadlet_read(OHCI1394State *s,
                                         const uint8_t **buf, int *len)
{
    OHCIReqNoDataPacket *req_nodata_packet;
    OHCIRspQuadletPacket rsp_quadlet_packet;

    hcd_fill_buffer(s, buf, len, 12);
    if (s->bufpos < 12) {
        /* not enough data yet HCD_STATE_CONNECTED 04 */
        return false;
    }
    if (s->bufpos > 12) {
        /* overflow HCD_STATE_CONNECTED 04 */
        return false;
    }
    req_nodata_packet = (OHCIReqNoDataPacket *)s->buf;
    /* forward to ar or handle here?? */
    rsp_quadlet_packet.flags = 0x60; /* t_code */
    rsp_quadlet_packet.flags |=
        (req_nodata_packet->flags & OHCI_PACKET_FLAGS_RT);
    rsp_quadlet_packet.flags |=
        (req_nodata_packet->flags & OHCI_PACKET_FLAGS_T_LABEL);
    rsp_quadlet_packet.destination_id = req_nodata_packet->destination_id ^ 1;
    rsp_quadlet_packet.source_id = req_nodata_packet->destination_id;
    if (req_nodata_packet->destination_offset_high == 0xFFFF) {
        if (0xF0000400 ==
            (req_nodata_packet->destination_offset_low & 0xFFFFFC00)) {
            uint32_t tmp_addr = s->mmio.config_rom_map +
               (req_nodata_packet->destination_offset_low & 0x3ff);

            dma_memory_read(&address_space_memory,
                            tmp_addr,
                            &rsp_quadlet_packet.data,
                            sizeof(uint32_t));
            rsp_quadlet_packet.r_code = RESP_COMPLETE;
        } else {
            /* Unknown address */
            rsp_quadlet_packet.r_code = RESP_ADDRESS_ERROR;
        }
    } else if ((req_nodata_packet->destination_offset_high) == 0x0000) {
        if (dma_memory_read(&address_space_memory,
                            req_nodata_packet->destination_offset_low,
                            &rsp_quadlet_packet.data, sizeof(uint32_t))) {
            rsp_quadlet_packet.r_code = RESP_ADDRESS_ERROR;
        } else {
            rsp_quadlet_packet.r_code = RESP_COMPLETE;
        }
    } else {
        /* Unknown address */
        rsp_quadlet_packet.r_code = RESP_ADDRESS_ERROR;
    }

    qemu_chr_fe_write(&s->chr, (uint8_t *)&rsp_quadlet_packet,
                      sizeof(rsp_quadlet_packet));
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_request_block_read(OHCI1394State *s,
                                       const uint8_t **buf, int *len)
{
    OHCIReqBlockPacket *req_packet;
    OHCIRspBlockPacket rsp_packet;
    void *bounce_buffer;

    hcd_fill_buffer(s, buf, len, sizeof(*req_packet));
    if (s->bufpos < sizeof(*req_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 05 */
        return false;
    }
    if (s->bufpos > sizeof(*req_packet)) {
        /* overflow HCD_STATE_CONNECTED 05 */
        return false;
    }
    req_packet = (OHCIReqBlockPacket *)s->buf;
    /* forward to ar or handle here?? */
    rsp_packet.flags = 0x70; /* t_code */
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_RT);
    rsp_packet.flags |= (req_packet->flags & OHCI_PACKET_FLAGS_T_LABEL);
    rsp_packet.destination_id = req_packet->destination_id ^ 1;
    rsp_packet.source_id = req_packet->destination_id;
    rsp_packet.data_length = req_packet->data_length;

    bounce_buffer = g_malloc(rsp_packet.data_length);
    if (req_packet->destination_offset_high == 0xFFFF) {
        if ((req_packet->destination_offset_low & 0xFFFFFC00) == 0xF0000400) {
            uint32_t tmp_addr = s->mmio.config_rom_map +
                      (req_packet->destination_offset_low & 0x3ff);
            dma_memory_read(&address_space_memory,
                            tmp_addr,
                            bounce_buffer,
                            rsp_packet.data_length);
            rsp_packet.r_code = RESP_COMPLETE;
        } else {
            /* Unknown address */
            rsp_packet.r_code = RESP_ADDRESS_ERROR;
        }
    } else if ((req_packet->destination_offset_high) == 0x0000) {
        if (dma_memory_read(&address_space_memory,
                            req_packet->destination_offset_low,
                            bounce_buffer,
                            rsp_packet.data_length)) {
            /* address error */
            rsp_packet.r_code = RESP_ADDRESS_ERROR;
        } else {
            rsp_packet.r_code = RESP_COMPLETE;
        }
    } else {
        /* Unknown address */
        rsp_packet.r_code = RESP_ADDRESS_ERROR;
    }
    qemu_chr_fe_write(&s->chr, (uint8_t *)&rsp_packet, sizeof(rsp_packet));
    if (rsp_packet.r_code == RESP_COMPLETE) {
        qemu_chr_fe_write(&s->chr, bounce_buffer, rsp_packet.data_length);
    }
    g_free(bounce_buffer);
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_response_quadlet_read(OHCI1394State *s,
                                          const uint8_t **buf, int *len)
{
    OHCIRspQuadletPacket *rsp_packet;

    hcd_fill_buffer(s, buf, len, sizeof(*rsp_packet));
    if (s->bufpos < sizeof(*rsp_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 4 */
        return false;
    }
    if (s->bufpos > sizeof(*rsp_packet)) {
        /* overflow HCD_STATE_CONNECTED 4 */
        return false;
    }
    rsp_packet = (OHCIRspQuadletPacket *)s->buf;

    hcd_async_rx_rsp_packet(s, (uint8_t *)rsp_packet,
                            sizeof(*rsp_packet), ACK_COMPLETE);
    s->bufpos = 0;
    return true;
}

static bool hcd_chr_response_block_read(OHCI1394State *s,
                                        const uint8_t **buf, int *len)
{
    OHCIRspBlockPacket *rsp_packet;

    hcd_fill_buffer(s, buf, len, sizeof(*rsp_packet));
    if (s->bufpos < sizeof(*rsp_packet)) {
        /* not enough data yet HCD_STATE_CONNECTED 07 */
        return false;
    }
    rsp_packet = (OHCIRspBlockPacket *)s->buf;
    hcd_fill_buffer(s, buf, len,
                    sizeof(*rsp_packet) + rsp_packet->data_length);
    if (s->bufpos < (sizeof(*rsp_packet) + rsp_packet->data_length)) {
        /* not enough data yet HCD_STATE_CONNECTED 07 */
        return false;
    }
    if (s->bufpos > (sizeof(*rsp_packet) + rsp_packet->data_length)) {
        /* overflow HCD_STATE_CONNECTED 07 */
        return false;
    }
    hcd_async_rx_rsp_packet(s, (uint8_t *)rsp_packet,
                            sizeof(*rsp_packet) + rsp_packet->data_length,
                            ACK_COMPLETE);
    s->bufpos = 0;
    return true;
}

static void hcd_chr_receive(void *opaque, const uint8_t *buf, int len)
{
    OHCI1394State *s = (OHCI1394State *)opaque;
    uint16_t received_bid;
    struct timeval tv;
    OHCIPacketHeader *packet_header;

    while (len) {
        switch (s->state) {
        case HCD_STATE_UNPLUGGED:
            /* restore races with chr event, just fake it here */
            hcd_chr_event(s, CHR_EVENT_OPENED);
            break;
        case HCD_STATE_MAGIC: /* waiting for magic */
            hcd_fill_buffer(s, &buf, &len, 4);
            if (s->bufpos < 4) {
                /* not enough data yet HCD_MAGIC */
                break;
            }
            if (s->bufpos > 4) {
                /* overflow HCD_MAGIC */
                break;
            }
            if (memcmp(s->buf, "1394", 4) != 0) {
                /* TODO: what do we do here? drop the connection I suppose */
                break;
            } else {
                s->state = HCD_STATE_DISCONNECTED;
            }
            s->bufpos = 0;
            break;
        case HCD_STATE_DISCONNECTED:
            hcd_fill_buffer(s, &buf, &len, 4);
            if (s->bufpos < 4) {
                /* not enough data yet HCD_STATE_DISCONNECTED */
                return;
            }
            s->bufpos = 0;
            if (*(uint32_t *)s->buf != 0xFFFFFFFF) {
                /* unknown data */
                break;
            }
            s->other_link = 1;
            /* link change - connected */
            s->state = HCD_STATE_ARBITRATION1;
            if (!(s->mmio.hc_control & HCCONTROL_LINK_ENABLE_MASK)) {
                /* we will progress when our link comes up and the other end
                sends a bid */
                break;
            }
            /* fall through as we won't go around again because len == 0 */
        case HCD_STATE_ARBITRATION1:
            gettimeofday(&tv, NULL);
            s->bid = 0;
            s->bid ^= (tv.tv_sec >> 0) & 0xFFFF;
            s->bid ^= (tv.tv_sec >> 16) & 0xFFFF;
            s->bid ^= (tv.tv_sec >> 32) & 0xFFFF;
            s->bid ^= (tv.tv_sec >> 48) & 0xFFFF;
            s->bid ^= (tv.tv_usec >> 0) & 0xFFFF;
            s->bid ^= (tv.tv_usec >> 16) & 0xFFFF;
            s->bid ^= (tv.tv_usec >> 32) & 0xFFFF;
            s->bid ^= (tv.tv_usec >> 48) & 0xFFFF;
            s->bid &= 0x7FFF;
            /* TODO: set high bit based on preference to become root */
            qemu_chr_fe_write(&s->chr, (uint8_t *)&s->bid, 2);
            s->state = HCD_STATE_ARBITRATION2;
            break;
        case HCD_STATE_ARBITRATION2:
            hcd_fill_buffer(s, &buf, &len, 2);
            if (s->bufpos < 2) {
                /* not enough data yet HCD_STATE_ARBITRATION2 */
                break;
            }
            received_bid = *(uint16_t *)s->buf;
            s->bufpos = 0;
            if (received_bid == s->bid) {
                s->state = HCD_STATE_ARBITRATION1;
                break;
            } else if (received_bid < s->bid) {
                s->root = 1;
                s->state = HCD_STATE_CONNECTED;
            } else {
                s->root = 0;
                s->state = HCD_STATE_CONNECTED;
            }
            hcd_bus_reset(s);
            break;
        case HCD_STATE_CONNECTED:
            if (!(s->mmio.hc_control & HCCONTROL_LINK_ENABLE_MASK)) {
                return;
            }
            hcd_fill_buffer(s, &buf, &len, 4);
            if (s->bufpos < 4) {
                /* not enough data yet HCD_STATE_CONNECTED */
                return;
            }
            if (*(uint32_t *)s->buf == 0xFFFFFFFE) {
                /* Reset because link change */
                s->bufpos = 0;
                s->state = HCD_STATE_DISCONNECTED;
                hcd_bus_reset(s);
                break;
            }
            packet_header = (OHCIPacketHeader *)s->buf;

            switch (packet_header->flags & OHCI_PACKET_FLAGS_T_CODE) {
            case 0x00: /* request - quadlet write */
                if (!hcd_chr_request_quadlet_write(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x10: /* request - block write */
                if (!hcd_chr_request_block_write(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x20: /* response - quadlet write */
                if (!hcd_chr_response_quadlet_write(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x40: /* request - quadlet read */
                if (!hcd_chr_request_quadlet_read(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x50: /* request - block read */
                if (!hcd_chr_request_block_read(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x60: /* response - quadlet read */
                if (!hcd_chr_response_quadlet_read(s, &buf, &len)) {
                    return;
                }
                break;
            case 0x70: /* response - block read */
                if (!hcd_chr_response_block_read(s, &buf, &len)) {
                    return;
                }
                break;
            default:
                /* unknown t_code */
                break;
            }
            return;
        }
    }
}

static void hcd_chr_event(void *opaque, int event)
{
    OHCI1394State *s = (OHCI1394State *)opaque;

    if (runstate_check(RUN_STATE_INMIGRATE)) {
        return;
    }

    switch (event) {
    case CHR_EVENT_OPENED:
        s->state = HCD_STATE_MAGIC;
        qemu_chr_fe_write(&s->chr, (uint8_t *)"1394", 4);
        if (s->mmio.hc_control & HCCONTROL_LINK_ENABLE_MASK) {
            uint32_t buf = 0xFFFFFFFF;
            qemu_chr_fe_write(&s->chr, (uint8_t *)&buf, 4);
            if (s->other_link) {
                hcd_bus_reset(s);
            }
        }
        break;
    case CHR_EVENT_CLOSED:
        s->state = HCD_STATE_UNPLUGGED;
        s->phy_pages[0][0] = 0x08; /* 0xFE ? */
        /* TODO: interrupt? */
        s->phy.reg5 |= PHY_REG5_PEI;
        s->mmio.int_event |= (1 << 19);
        hcd_bus_reset(s);
        break;
    }
}

#define TYPE_PCI_1394 "ohci-1394"
#define PCI_1394(obj) OBJECT_CHECK(OHCI1394State, (obj), TYPE_PCI_1394)

static int
hcd_pci_init(PCIDevice *pci_dev)
{
    OHCI1394State *s = PCI_1394(pci_dev);
    uint8_t *pci_conf = pci_dev->config;

    pci_set_byte(pci_conf + PCI_CLASS_PROG, 0x10); /* OHCI */
    pci_set_word(pci_conf + PCI_STATUS,
                 PCI_STATUS_DEVSEL_MEDIUM | PCI_STATUS_FAST_BACK);
    pci_set_byte(pci_conf + PCI_INTERRUPT_PIN, 1);
    pci_set_byte(pci_conf + PCI_MIN_GNT, 0x08);

    memory_region_init_io(&s->mmio_bar, OBJECT(s), &hcd_mmio_ops, s,
                          "ohci-1394-mmio", OHCI_1394_MMIO_SIZE);
    pci_register_bar(&s->pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->mmio_bar);
    s->irq = pci_allocate_irq(&s->pci_dev);
    s->at_req_timer.s = s;
    s->at_req_timer.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hcd_at_timer,
                                         &s->at_req_timer);
    s->at_req_timer.num = 0;
    s->at_rsp_timer.s = s;
    s->at_rsp_timer.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hcd_at_timer,
                                         &s->at_rsp_timer);
    s->at_rsp_timer.num = 1;
    qemu_chr_fe_set_handlers(&s->chr, hcd_chr_can_receive, hcd_chr_receive,
                          hcd_chr_event, s, NULL, true);
    hcd_hard_reset(s);
    return 0;
}

static void
hcd_pci_exit(PCIDevice *pci_dev)
{
    OHCI1394State *s = PCI_1394(pci_dev);

    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL, NULL, true);

    timer_del(s->at_rsp_timer.timer);
    timer_free(s->at_rsp_timer.timer);

    timer_del(s->at_req_timer.timer);
    timer_free(s->at_req_timer.timer);

    qemu_free_irq(s->irq);
}

static Property hcd_properties[] = {
    DEFINE_PROP_CHR("chardev", OHCI1394State, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_pci_hcd = {
    .name = TYPE_PCI_1394,
    .version_id = 3,
    .minimum_version_id = 3,
    .minimum_version_id_old = 0,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci_dev, OHCI1394State),
        VMSTATE_UINT32_ARRAY(mmio.regs, OHCI1394State,
                             OHCI_1394_MMIO_SIZE >> 2),
        VMSTATE_UINT8_ARRAY(phy.bytes, OHCI1394State, 16),
        VMSTATE_UINT8_2DARRAY(phy_pages, OHCI1394State, 8, 8),
        VMSTATE_END_OF_LIST(),
    }
};

static void
hcd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = hcd_pci_init;
    k->exit = hcd_pci_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_1394_OHCI;
    k->class_id = PCI_CLASS_SERIAL_1394;
    dc->desc = "IEEE1394 OpenHCI Host Controller";
    dc->props = hcd_properties;
    dc->vmsd = &vmstate_pci_hcd;
}

static const TypeInfo hcd_info = {
    .name          = TYPE_PCI_1394,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(OHCI1394State),
    .class_init    = hcd_class_init,
};

static void ohci_1394_register_types(void)
{
    type_register_static(&hcd_info);
}

type_init(ohci_1394_register_types)
