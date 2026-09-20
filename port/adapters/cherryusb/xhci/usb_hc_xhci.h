/*
 * @file   usb_hc_xhci.h
 * @brief  xHCI register/TRB/context definitions for the DWC3 xHCI driver.
 *
 * Transplanted whole-file from the lab line (rk3568_lab/os/standalone,
 * commit cc3acfd6, Apache-2.0, same author) per decision D20. MMIO layout
 * matches the xHCI 1.1 spec and FreeBSD xhcireg.h (BSD-2-Clause); the DWC3
 * globals follow the Synopsys DWC3 reference. The context-field layout is
 * the u-boot/Linux one - this DWC3 rejects the xHCI 1.1 §6.2 alternative
 * (measured 2026-08-12, see the comment at the context block).
 *
 * @author zhugengyu
 * @date   19.09.2026
 */
#ifndef USB_HC_XHCI_H
#define USB_HC_XHCI_H

#include <stdint.h>

/* ====================== xHCI 能力寄存器 (capbase) ====================== */
#define XHCI_CAPLENGTH          0x00U
#define XHCI_HCIVERSION         0x02U
#define XHCI_HCSPARAMS1         0x04U
#define XHCI_HCSPARAMS2         0x08U
#define XHCI_HCSPARAMS3         0x0CU
#define XHCI_HCCPARAMS1         0x10U
#define XHCI_DBOFF              0x14U
#define XHCI_RTSOFF             0x18U
#define XHCI_HCCPARAMS2         0x1CU

/* HCSPARAMS1 字段布局(xHCI spec §5.3.3, NetBSD xhcireg.h 同款):
 * MaxSlots[7:0] / MaxIntrs[18:8] / MaxPorts[31:24] */
#define XHCI_HCS1_DEVSLOT_MAX(x)   ((x) & 0xFFU)
#define XHCI_HCS1_MAX_INTRS(x)     (((x) >> 8) & 0x7FFU)
#define XHCI_HCS1_N_PORTS(x)       (((x) >> 24) & 0xFFU)
#define XHCI_HCS2_ERST_MAX(x)      (((x) >> 4) & 0xFU)
#define XHCI_HCS2_SPB_MAX(x)       (((((x) >> 16) & 0x3E0U)) | (((x) >> 27) & 0x1FU))
#define XHCI_HCCPARAMS1_AC64(x)    ((x) & 0x1U)
#define XHCI_HCCPARAMS1_CSZ(x)     (((x) >> 2) & 0x1U)
#define XHCI_HCCPARAMS1_XECP(x)    (((x) >> 16) & 0xFFFFU)

/* ====================== xHCI 操作寄存器 (operational) ====================== */
#define XHCI_USBCMD          0x00U
#define XHCI_CMD_RS          0x00000001U
#define XHCI_CMD_HCRST       0x00000002U
#define XHCI_CMD_INTE        0x00000004U
#define XHCI_CMD_HSEE        0x00000008U
#define XHCI_CMD_EWE         0x00000400U

#define XHCI_USBSTS          0x04U
#define XHCI_PAGESIZE        0x08U
#define XHCI_DNCTRL          0x14U
#define XHCI_STS_HCH         0x00000001U
#define XHCI_STS_HSE         0x00000004U
#define XHCI_STS_EINT        0x00000008U
#define XHCI_STS_PCD         0x00000010U
#define XHCI_STS_CNR         0x00000800U
#define XHCI_STS_HCE         0x00001000U
#define XHCI_STS_RSVDP0      0xFFFFE000U /* RW1C 写回时必须剥离的保留位 */

#define XHCI_CRCR_LO         0x18U
#define XHCI_CRCR_HI         0x1CU
#define XHCI_CRCR_LO_RCS     0x00000001U
#define XHCI_CRCR_LO_CS      0x00000002U
#define XHCI_CRCR_LO_CA      0x00000004U
#define XHCI_CRCR_LO_CRR     0x00000008U

#define XHCI_DCBAAP_LO       0x30U
#define XHCI_DCBAAP_HI       0x34U

#define XHCI_CONFIG          0x38U
#define XHCI_CONFIG_SLOTS_MASK 0x000000FFU

#define XHCI_PORTSC(n)       (0x3F0U + (0x10U * (n)))
#define XHCI_PS_CCS          0x00000001U
#define XHCI_PS_PED          0x00000002U
#define XHCI_PS_OCA          0x00000008U
#define XHCI_PS_PR           0x00000010U
#define XHCI_PS_PLS_GET(x)   (((x) >> 5) & 0xFU)
#define XHCI_PS_PLS_SET(x)   (((x) & 0xFU) << 5)
#define XHCI_PS_PP           0x00000200U
#define XHCI_PS_SPEED_GET(x) (((x) >> 10) & 0xFU)
#define XHCI_PS_SPEED_FULL   0x1U
#define XHCI_PS_SPEED_LOW    0x2U
#define XHCI_PS_SPEED_HIGH   0x3U
#define XHCI_PS_SPEED_SS     0x4U
#define XHCI_PS_LWS          0x00010000U
#define XHCI_PS_CSC          0x00020000U
#define XHCI_PS_PEC          0x00040000U
#define XHCI_PS_WRC          0x00080000U
#define XHCI_PS_OCC          0x00100000U
#define XHCI_PS_PRC          0x00200000U
#define XHCI_PS_PLC          0x00400000U
#define XHCI_PS_CEC          0x00800000U
#define XHCI_PS_DR           0x40000000U
#define XHCI_PS_CLEAR        0x80FF01FFU

/* ====================== xHCI 运行时寄存器 (rts) ====================== */
#define XHCI_MFINDEX         0x0000U
#define XHCI_IMAN(n)         (0x0020U + (0x20U * (n)))
#define XHCI_IMAN_INTR_PEND  0x00000001U
#define XHCI_IMAN_INTR_ENA   0x00000002U
#define XHCI_IMOD(n)         (0x0024U + (0x20U * (n)))
#define XHCI_ERSTSZ(n)       (0x0028U + (0x20U * (n)))
#define XHCI_ERSTS_SET(x)    ((x) & 0xFFFFU)
#define XHCI_ERSTBA_LO(n)    (0x0030U + (0x20U * (n)))
#define XHCI_ERSTBA_HI(n)    (0x0034U + (0x20U * (n)))
#define XHCI_ERDP_LO(n)      (0x0038U + (0x20U * (n)))
#define XHCI_ERDP_HI(n)      (0x003CU + (0x20U * (n)))
#define XHCI_ERDP_BUSY       0x00000008U /* EHB: 事件处理中(NetBSD 同款用法) */

/* ====================== xHCI Doorbell ====================== */
#define XHCI_DOORBELL(n)     (0x0000U + (4U * (n)))
#define XHCI_DB_TARGET(x)    ((x) & 0xFFU)

/* ====================== TRB ====================== */
/* 命名与字段位置按 xHCI spec §6.4 (NetBSD xhcireg.h 同款): TRB 的 dw0/dw1
 * = parameter, dw2 = status, dw3 = control。 */
#define TRB_SIZE             16U

/* --- TRB dw3 (control) --- */
#define TRB3_CYCLE        (1U << 0)
#define TRB3_ENT          (1U << 1) /* Evaluate Next TRB; LINK TRB 上即 TC 位 */
#define TRB3_ISP          (1U << 2) /* Interrupt on Short Packet (=ED 位) */
#define TRB3_ED           TRB3_ISP  /* Event Data(事件 TRB dw3 同位) */
#define TRB3_CHAIN        (1U << 4)
#define TRB3_IOC          (1U << 5)
#define TRB3_IDT          (1U << 6) /* Immediate Data */
#define TRB3_BSR          (1U << 9) /* Address Device: Block Set Address Request */
#define TRB3_TYPE_SET(x)  (((x) & 0x3FU) << 10)
#define TRB3_TYPE_GET(x)  (((x) >> 10) & 0x3FU)
#define TRB3_TRT_NONE     (0U << 16)
#define TRB3_TRT_RESERVED (1U << 16)
#define TRB3_TRT_OUT      (2U << 16)
#define TRB3_TRT_IN       (3U << 16)
#define TRB3_DIR_IN       (1U << 16)
#define TRB3_SLOT_ID(x)   (((x) & 0xFFU) << 24)
#define TRB3_SLOT_ID_GET(x) (((x) >> 24) & 0xFFU)
#define TRB3_EP_ID(x)     (((x) & 0x1FU) << 16)
#define TRB3_EP_ID_GET(x)   (((x) >> 16) & 0x1FU)

/* --- TRB dw2 (status) --- */
#define TRB2_IRQ(x)       (((x) & 0x3FFU) << 22)
#define TRB2_TDSZ(x)      (((x) & 0x1FU) << 17)
#define TRB2_LEN(x)       ((x) & 0x1FFFFU)
#define TRB2_REM_GET(x)   ((x) & 0xFFFFFFU)
#define TRB2_CODE_GET(x)  (((x) >> 24) & 0xFFU)

/* --- TRB 类型 (spec §6.4.1 Table 131; 传输段 TRB 旧值整体偏 1, 2026-09-20
 * 板验实测: SETUP 写成 1 会被当 NORMAL 执行, 控制传输永远不完成) --- */
#define TRB_TYPE_NORMAL      1U
#define TRB_TYPE_SETUP       2U
#define TRB_TYPE_DATA        3U
#define TRB_TYPE_STATUS      4U
#define TRB_TYPE_ISOCH       5U
#define TRB_TYPE_LINK        6U
#define TRB_TYPE_EVENT_DATA  7U
#define TRB_TYPE_NOOP        8U
#define TRB_TYPE_ENABLE_SLOT 9U
#define TRB_TYPE_DISABLE_SLOT 10U
#define TRB_TYPE_ADDRESS_DEV 11U
#define TRB_TYPE_CONFIGURE_EP 12U
#define TRB_TYPE_EVALUATE_CTX 13U
#define TRB_TYPE_RESET_EP    14U
#define TRB_TYPE_STOP_EP     15U
#define TRB_TYPE_SET_TR_DEQUEUE 16U
#define TRB_TYPE_RESET_DEVICE 17U
#define TRB_TYPE_TRANSFER     32U
#define TRB_TYPE_CMD_COMPLETE 33U
#define TRB_TYPE_PORT_STATUS  34U
#define TRB_TYPE_BANDWIDTH_REQ 35U
#define TRB_TYPE_DOORBELL     36U
#define TRB_TYPE_HC_EVENT     37U

/* --- 事件完成码 (§6.4.5; 数值曾在本头文件错位, 2026-09-20 按 spec/NetBSD
 * 校正: Success=1, Short Packet=13, Parameter=19, Cmd Ring Stopped=24) --- */
#define TRB_CODE_SUCCESS         1U
#define TRB_CODE_DATA_BUFFER_ERR 2U
#define TRB_CODE_BABBLE          3U
#define TRB_CODE_TRANSACTION_ERR 4U
#define TRB_CODE_TRB_ERR         5U
#define TRB_CODE_STALL           6U
#define TRB_CODE_SHORT_PACKET    13U
#define TRB_CODE_RING_UNDERRUN   14U
#define TRB_CODE_RING_OVERRUN    15U
#define TRB_CODE_PARAMETER_ERR   19U
#define TRB_CODE_CONTEXT_STATE   21U
#define TRB_CODE_CMD_RING_STOPPED 24U
#define TRB_CODE_CMD_ABORTED     25U
#define TRB_CODE_STOPPED         26U
#define TRB_CODE_STOPPED_INVAL   27U
#define TRB_CODE_STOPPED_SHORT   28U

/* ====================== 上下文 ====================== */
/* 布局即 xHCI spec §6.2 (u-boot/Linux/NetBSD 三方一致的同一编码):
 * slot dw0(dev_info): route[19:0] | speed[23:20] | mtt[25] | hub[26] | LAST_CTX[31:27]
 * slot dw1(dev_info2): max_exit[15:0] | root_hub_port[23:16] | ports[31:24] | tt_sid
 * slot dw2(dev_info2b): tt_port[19:16] | tt_think[21:20] | intr_target[31:22]
 * slot dw3(dev_state): usb_addr[7:0] | slot_state[31:27]
 * ep dw0(ep_info): ep_state[2:0] | mult[9:8] | max_pstreams[14:10] | lsa[15] | interval[23:16]
 * ep dw1(ep_info2): cerr[3:1] | ep_type[5:3] | hid[6] | max_burst[15:8] | max_packet[31:16]
 * ep dw2/dw3(deq): 64 位 TR dequeue pointer, dw2 bit0 = DCS
 * ep dw4(tx_info): avg_trb_len[31:16] | max_esit_payload[31:16]@dw4hi? 见 spec
 * 注意: 输入上下文里 slot dw3 的 slot_state/usb_addr 是 rsvdZ, 保持 0。
 * CSZ=1 (HCCPARAMS1 bit2, 2026-08-12 实测) -> 每上下文 64 字节(16 u32);
 * 输入上下文按 u-boot 风格: ICC 自占一个 64B 槽, 之后 ctx(dci) 位于 64*(dci+1)。 */
#define SLOT_CTX_DEV_INFO_ROUTE(x)        ((x) & 0xFFFFFU)
#define SLOT_CTX_DEV_INFO_SPEED(x)        (((x) & 0xFU) << 20)
#define SLOT_CTX_DEV_INFO_LAST_CTX(x)     (((x) & 0x1FU) << 27)
#define SLOT_CTX_SPEED_FS                 1U
#define SLOT_CTX_SPEED_LS                 2U
#define SLOT_CTX_SPEED_HS                 3U
#define SLOT_CTX_DEV_INFO2_MAX_EXIT(x)    ((x) & 0xFFFFU)
#define SLOT_CTX_DEV_INFO2_PORT(x)        (((x) & 0xFFU) << 16)
#define SLOT_CTX_DEV_STATE_ADDR(x)        ((x) & 0xFFU)
#define SLOT_CTX_DEV_STATE_SLOT_STATE(x)  (((x) & 0x1FU) << 27)
#define SLOT_CTX_STATE_DEFAULT            1U
#define SLOT_CTX_STATE_ADDRESSED          2U
#define SLOT_CTX_STATE_CONFIGURED         3U

/* Endpoint Context (spec §6.2.3; 旧宏把 avg_trb_len 放在 word3 - 那是 TR
 * Dequeue 的高 32 位, 控制器会拿 0x8_xxxxxxxx 去取环, 静默不执行,
 * 2026-09-20 板验读回输出上下文实锤) */
#define EP_CTX_0_INTERVAL_GET(x)          (((x) >> 16) & 0xFFU)
#define EP_CTX_0_INTERVAL_SET(x)          (((x) & 0xFFU) << 16)
#define EP_CTX_1_CERR_SET(x)              (((x) & 0x3U) << 1)
#define EP_CTX_1_EP_TYPE_GET(x)           (((x) >> 3) & 0x7U)
#define EP_CTX_1_EP_TYPE_SET(x)           (((x) & 0x7U) << 3)
#define EP_CTX_1_MAX_BURST_GET(x)         (((x) >> 8) & 0xFFU)
#define EP_CTX_1_MAX_BURST_SET(x)         (((x) & 0xFFU) << 8)
#define EP_CTX_1_MAX_PACKET_GET(x)        (((x) >> 16) & 0xFFFFU)
#define EP_CTX_1_MAX_PACKET_SET(x)        (((x) & 0xFFFFU) << 16)
#define EP_CTX_2_TR_DEQUEUE_LO(x)         ((x) & 0xFFFFFFF0U)
#define EP_CTX_2_CYCLE                    (1U << 0)
#define EP_CTX_3_TR_DEQUEUE_HI(x)         ((x) & 0xFFFFFFFFU)
#define EP_CTX_4_AVG_TRB_LEN(x)           ((x) & 0xFFFFU)

#define EP_TYPE_CONTROL  4U
#define EP_TYPE_ISOCH_OUT 1U
#define EP_TYPE_BULK_OUT 2U
#define EP_TYPE_INTR_OUT 3U
#define EP_TYPE_ISOCH_IN  5U
#define EP_TYPE_BULK_IN   6U
#define EP_TYPE_INTR_IN   7U

/* 输入控制上下文 (32 字节, u-boot struct xhci_input_control_ctx) */
#define ICC_ADD_CONTEXT(x)   ((x) & 0xFFFFFFFFU)
#define ICC_DROP_CONTEXT(x)  ((x) & 0xFFFFFFFFU)

/* ====================== DWC3 寄存器 (core 绝对偏移, u-boot core.h 对齐)
 * core 区 = DWC3 base + 0xC100..0xC6FF; xHCI 寄存器 = DWC3 base + 0x0 ====================== */
#define DWC3_GCTL            0xC110U
#define DWC3_GSTS            0xC118U
#define DWC3_GUCTL1          0xC11CU
#define DWC3_GSNPSID         0xC120U
#define DWC3_GUCTL           0xC12CU
#define DWC3_GHWPARAMS0      0xC140U
#define DWC3_GHWPARAMS1      0xC144U
#define DWC3_GUSB2PHYCFG     0xC200U
#define DWC3_GUSB3PIPECTL    0xC2C0U

#define DWC3_GSNPSID_MASK    0xFFFF0000U
#define DWC3_GSNPSID_REV_MASK 0xFFFFU
#define DWC3_GSNPSID_VAL     0x55330000U
#define DWC3_REVISION_MASK   0xFFFFU

#define DWC3_GCTL_CORESOFTRESET (1U << 11)
#define DWC3_GCTL_PRTCAPDIR(x)  ((x) << 12)
#define DWC3_GCTL_PRTCAP_MASK   (3U << 12)
#define DWC3_GCTL_PRTCAP_HOST   1U
#define DWC3_GCTL_PRTCAP_DEVICE 2U
#define DWC3_GCTL_PRTCAP_OTG    3U
#define DWC3_GCTL_SCALEDOWN(x)  ((x) << 4)
#define DWC3_GCTL_SCALEDOWN_MASK DWC3_GCTL_SCALEDOWN(3)
#define DWC3_GCTL_DISSCRAMBLE   (1U << 3)
#define DWC3_GCTL_DSBLCLKGTNG   (1U << 0)
#define DWC3_GCTL_U2RSTECN      (1U << 16)
#define DWC3_GCTL_RUN_STOP      (1U << 31)

#define DWC3_GHWPARAMS1_EN_PWROPT(x) (((x) & (3U << 24)) >> 24)
#define DWC3_GHWPARAMS1_EN_PWROPT_NO 0U
#define DWC3_GHWPARAMS1_EN_PWROPT_CLK 1U

#define DWC3_GUSB2PHYCFG_PHYSOFTRST   (1U << 31)
#define DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS (1U << 30)
#define DWC3_GUSB2PHYCFG_ENBLSLPM     (1U << 8)
#define DWC3_GUSB2PHYCFG_SUSPHY       (1U << 6)
#define DWC3_GUSB2PHYCFG_PHYIF        (1U << 3)
#define DWC3_GUSB2PHYCFG_PHYIF_MASK   DWC3_GUSB2PHYCFG_PHYIF
#define DWC3_GUSB2PHYCFG_USBTRDTIM(x) ((x) << 10)
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK DWC3_GUSB2PHYCFG_USBTRDTIM(0xFU)
#define DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT DWC3_GUSB2PHYCFG_USBTRDTIM(0x5U)

#define DWC3_GUSB3PIPECTL_PHYSOFTRST  (1U << 31)
#define DWC3_GUSB3PIPECTL_DISRXDETINP3 (1U << 28)
#define DWC3_GUSB3PIPECTL_UX_EXIT_PX  (1U << 27)
#define DWC3_GUSB3PIPECTL_DEPOCHANGE  (1U << 18)
#define DWC3_GUSB3PIPECTL_SUSPENDUSB3 (1U << 17)

#define DWC3_DCFG            0xC700U
#define DWC3_DCFG_SPEED_MASK (7U << 0)
#define DWC3_DCFG_SPEED_HS   0U

#define DWC3_GUCTL_HOST_AUTO_RETRY    (1U << 17)
#define DWC3_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK (1U << 26)
#define DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS (1U << 28)

/* xHCI 寄存器从 DWC3 base 偏移 0 开始 */
#define DWC3_XHCI_REGS_OFFSET 0x0U

/* ====================== 内部结构 ====================== */

struct xhci_trb {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
};

struct xhci_erst_seg {
    uint32_t seg_base_lo;
    uint32_t seg_base_hi;
    uint32_t seg_size;
    uint32_t rsvd;
};

#endif /* USB_HC_XHCI_H */
