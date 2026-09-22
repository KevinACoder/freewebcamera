/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Register map and SDIO bus constants for the RTL8189FTV (RTL8188F
 * silicon).  Names follow the vendor rtl8189fs driver (GPL-2.0) so that
 * sequences can be cross-checked line by line; the code itself is
 * written against the NetBSD sdmmc(4) API.
 */

#ifndef _DEV_SDMMC_RTW8189F_REG_H_
#define _DEV_SDMMC_RTW8189F_REG_H_

/*
 * The SDIO function exposes a 17-bit address space, DeviceID in
 * bits [16:13]:
 *
 *	0 SDIO local registers (0x10250000 | 0x000-0xfff)
 *	1 WLAN_TX_EXQ    4 WLAN_TX_HIQ    5 WLAN_TX_MIQ    6 WLAN_TX_LOQ
 *	7 WLAN_RX0FF     8 WLAN I/O registers (MAC/BB/RF space)
 */
#define RTW8189F_SDIO_LOCAL_DEVICE_ID		0
#define RTW8189F_WLAN_TX_EXQ_DEVICE_ID		3
#define RTW8189F_WLAN_TX_HIQ_DEVICE_ID		4
#define RTW8189F_WLAN_TX_MIQ_DEVICE_ID		5
#define RTW8189F_WLAN_TX_LOQ_DEVICE_ID		6
#define RTW8189F_WLAN_RX0FF_DEVICE_ID		7
#define RTW8189F_WLAN_IOREG_DEVICE_ID		8

#define RTW8189F_SDIO_LOCAL_MSK			0x0fff
#define RTW8189F_WLAN_IOREG_MSK			0x7fff
#define RTW8189F_WLAN_FIFO_MSK			0x1fff
#define RTW8189F_WLAN_RX0FF_MSK			0x0003

#define RTW8189F_SDIO_LOCAL_BASE		0x10250000

/* SDIO local (DeviceID 0) registers. */
#define RTW8189F_SDIO_REG_TX_CTRL		0x0000
#define RTW8189F_SDIO_REG_HIMR			0x0014
#define RTW8189F_SDIO_REG_HISR			0x0018
#define RTW8189F_SDIO_REG_RX0_REQ_LEN		0x001c
#define RTW8189F_SDIO_REG_FREE_TXPG		0x0020
#define RTW8189F_SDIO_REG_AC_OQT_FREEPG		0x002a	/* TX OQT free (AC) */
#define RTW8189F_SDIO_REG_NOAC_OQT_FREEPG	0x002b	/* TX OQT free (no-AC) */
#define RTW8189F_SDIO_REG_HRPWM1		0x0080

/* HISR / HIMR bits. */
#define RTW8189F_HIMR_RX_REQUEST		__BIT(0)
#define RTW8189F_HIMR_AVAL			__BIT(1)
#define RTW8189F_HISR_RX_REQUEST		__BIT(0)
#define RTW8189F_HISR_AVAL			__BIT(1)

/* MAC register file (accessed through DeviceID 8). */
#define RTW8189F_REG_SYS_ISO_CTRL		0x0000
#define RTW8189F_REG_SYS_FUNC_EN		0x0002
#define RTW8189F_REG_APS_FSMCO			0x0004
#define RTW8189F_REG_SYS_CLKR			0x0008
#define RTW8189F_REG_RSV_CTRL			0x001c
#define RTW8189F_REG_EFUSE_CTRL			0x0030
#define RTW8189F_REG_EFUSE_TEST			0x0034
#define RTW8189F_REG_MCUFWDL			0x0080
#define RTW8189F_REG_HMETFR			0x01cc
#define RTW8189F_REG_HMEBOX(n)			(0x01d0 + (n) * 4)
#define RTW8189F_REG_HMEBOX_EXT(n)		(0x01f0 + (n) * 4)
#define RTW8189F_REG_CR				0x0100

/* REG_CR bits, as programmed at power-on (0x063f, Linux sdio_power_on_check
 * reports val_mix 0x63f on this very board). */
#define RTW8189F_HCI_TXDMA_EN			__BIT(0)
#define RTW8189F_HCI_RXDMA_EN			__BIT(1)
#define RTW8189F_TXDMA_EN			__BIT(2)
#define RTW8189F_RXDMA_EN			__BIT(3)
#define RTW8189F_PROTOCOL_EN			__BIT(4)
#define RTW8189F_SCHEDULE_EN			__BIT(5)
#define RTW8189F_MACTXEN			__BIT(6)
#define RTW8189F_MACRXEN			__BIT(7)
#define RTW8189F_ENSEC				__BIT(9)
#define RTW8189F_CALTMR_EN			__BIT(10)
#define RTW8189F_CR_POWERON \
	(RTW8189F_HCI_TXDMA_EN | RTW8189F_HCI_RXDMA_EN | RTW8189F_TXDMA_EN | \
	 RTW8189F_RXDMA_EN | RTW8189F_PROTOCOL_EN | RTW8189F_SCHEDULE_EN | \
	 RTW8189F_ENSEC | RTW8189F_CALTMR_EN)

/* REG_SYS_FUNC_EN bits. */
#define RTW8189F_FEN_ELDR			__BIT(12)
#define RTW8189F_FEN_CPUEN			__BIT(10)	/* 0x02[10] */
#define RTW8189F_FEN_CORE_EN			__BIT(8)	/* 0x02[8] */

/* REG_SYS_ISO_CTRL / REG_SYS_CLKR bits used by eFuse access. */
#define RTW8189F_PWC_EV12V			__BIT(15)	/* 0x00[15] */
#define RTW8189F_LOADER_EN			__BIT(5)	/* 0x08[5] */
#define RTW8189F_ANA8M				__BIT(1)	/* 0x08[1] */

/* REG_MCUFWDL bits. */
#define RTW8189F_MCUFWDL_EN			__BIT(0)
#define RTW8189F_MCUFWDL_RDY			__BIT(1)
#define RTW8189F_FWDL_CHKSUM_RPT		__BIT(2)
#define RTW8189F_WINTINI_RDY			__BIT(6)
#define RTW8189F_RAM_DL_SEL			__BIT(7)

/* Firmware download. */
#define RTW8189F_FW_START_ADDRESS		0x1000
#define RTW8189F_FW_DL_PAGE_SIZE		4096
#define RTW8189F_FW_MAX_SIZE			0x8000
#define RTW8189F_FW_HDR_SIZE			32

/* eFuse. */
#define RTW8189F_EFUSE_CTRL_DATA_M		__BITS(7, 0)
#define RTW8189F_EFUSE_CTRL_ADDR_M		__BITS(17, 8)
#define RTW8189F_EFUSE_CTRL_ADDR_S		8	/* bits [17:8] */
#define RTW8189F_EFUSE_CTRL_VALID		__BIT(31)
#define RTW8189F_EFUSE_CTRL_LDOEEN		__BIT(30)
#define RTW8189F_EFUSE_REAL_CONTENT_LEN		256
#define RTW8189F_HWSET_MAX_SIZE			512
#define RTW8189F_EEPROM_MAC_ADDR		0x11a

/* Firmware image header (32 bytes; first two bytes = signature). */
#define RTW8189F_FW_SIGNATURE			0x88f1
#define RTW8189F_FW_VERSION_OFF			4	/* u8 version at [4] */

/*
 * TX/RX DMA configuration (vendor sdio_halinit.c _InitQueueReservedPage
 * et al.).  TX page size is always 128 bytes.
 */
#define RTW8189F_REG_RQPN			0x0200
#define RTW8189F_REG_TDECTRL			0x0208
#define RTW8189F_REG_RQPN_NPQ			0x0214
#define RTW8189F_REG_AUTO_LLT			0x0224
#define RTW8189F_BIT_AUTO_INIT_LLT		__BIT(16)
#define RTW8189F_REG_RXDMA_AGG_PG_TH		0x0280
#define RTW8189F_REG_RXDMA_MODE_CTRL		0x0290	/* 8188F */

#define RTW8189F_RQPN_HPQ(x)			((x) & 0xff)
#define RTW8189F_RQPN_LPQ(x)			(((x) & 0xff) << 8)
#define RTW8189F_RQPN_PUBQ(x)			(((x) & 0xff) << 16)
#define RTW8189F_RQPN_NPQ(x)			((x) & 0xff)
#define RTW8189F_RQPN_LD_RQPN			__BIT(31)

/*
 * NORMAL_PAGE_NUM_* / TX_TOTAL_PAGE_NUMBER_8188F: vendor reserves
 * BCNQ_PAGE_NUM_8188F = MAX_BEACON_LEN/128 + 6 = 10 pages (WOWLAN 0),
 * so TX total is 0xFF - 10 = 0xF5 and the boundary is 0xF6.
 */
#define RTW8189F_PAGE_NUM_HPQ			0x0c
#define RTW8189F_PAGE_NUM_LPQ			0x02
#define RTW8189F_PAGE_NUM_NPQ			0x02
#define RTW8189F_TX_TOTAL_PAGE_NUMBER		0xf5	/* 0xFF - 10 - 0 */
#define RTW8189F_NUM_PUBQ \
	(RTW8189F_TX_TOTAL_PAGE_NUMBER - RTW8189F_PAGE_NUM_HPQ - \
	 RTW8189F_PAGE_NUM_LPQ - RTW8189F_PAGE_NUM_NPQ)
#define RTW8189F_TX_PAGE_BOUNDARY		0xf6	/* TX_TOTAL + 1 */
#define RTW8189F_RX_DMA_BOUNDARY		0x3f7f	/* 0x4000 - 0x80 - 1 */

/* Queue-to-TXDMA-ring mapping (REG_TRXDMA_CTRL). */
#define RTW8189F_REG_TRXDMA_CTRL		0x010c
#define RTW8189F_TRXDMA_HIQ_MAP(x)		(((x) & 0x3) << 14)
#define RTW8189F_TRXDMA_MGQ_MAP(x)		(((x) & 0x3) << 12)
#define RTW8189F_TRXDMA_BKQ_MAP(x)		(((x) & 0x3) << 10)
#define RTW8189F_TRXDMA_BEQ_MAP(x)		(((x) & 0x3) << 8)
#define RTW8189F_TRXDMA_VIQ_MAP(x)		(((x) & 0x3) << 6)
#define RTW8189F_TRXDMA_VOQ_MAP(x)		(((x) & 0x3) << 4)
#define RTW8189F_QUEUE_LOW			1
#define RTW8189F_QUEUE_NORMAL			2
#define RTW8189F_QUEUE_HIGH			3

#define RTW8189F_REG_PBP			0x0104
#define RTW8189F_PBP_128			0x1
#define RTW8189F_PBP_RX(x)			(x)
#define RTW8189F_PBP_TX(x)			((x) << 4)

/* TX buffer boundaries (all get TX_PAGE_BOUNDARY). */
#define RTW8189F_REG_TXPKTBUF_BCNQ_BDNY		0x0424
#define RTW8189F_REG_TXPKTBUF_MGQ_BDNY		0x0425
#define RTW8189F_REG_TXPKTBUF_WMAC_LBK_BF_HD	0x045d
#define RTW8189F_REG_TRXFF_BNDY			0x0114

/* Protocol configuration. */
#define RTW8189F_REG_FWHW_TXQ_CTRL		0x0420
#define RTW8189F_REG_HWSEQ_CTRL			0x0423
#define RTW8189F_REG_SPEC_SIFS			0x0428
#define RTW8189F_REG_RETRY_LIMIT		0x042a
#define RTW8189F_REG_RRSR			0x0440
#define RTW8189F_REG_ARFR0			0x0444
#define RTW8189F_REG_ARFR1			0x044c
#define RTW8189F_REG_AMPDU_MAX_TIME		0x0456
#define RTW8189F_REG_BAR_MODE_CTRL		0x04cc
#define RTW8189F_AMPDU_RTY_NEW			__BIT(7)
#define RTW8189F_RATE_RRSR_CCK_ONLY_1M		0xffff1
#define RTW8189F_RETRY_LIMIT(x)			(((x) & 0x3f) | (((x) & 0x3f) << 8))

#define RTW8189F_REG_EDCA_VO_PARAM		0x0500
#define RTW8189F_REG_EDCA_VI_PARAM		0x0504
#define RTW8189F_REG_EDCA_BE_PARAM		0x0508
#define RTW8189F_REG_EDCA_BK_PARAM		0x050c
#define RTW8189F_REG_BCNTCFG			0x0510
#define RTW8189F_REG_PIFS			0x0512
#define RTW8189F_REG_SIFS_CTX			0x0514
#define RTW8189F_REG_SIFS_TRX			0x0516
#define RTW8189F_REG_TBTT_PROHIBIT		0x0540
#define RTW8189F_REG_BCN_CTRL			0x0550
#define RTW8189F_REG_DRVERLYINT			0x0558
#define RTW8189F_REG_BCNDMATIM			0x0559
#define RTW8189F_REG_USTIME_TSF			0x055c	/* 8188F */
#define RTW8189F_REG_SECONDARY_CCA_CTRL		0x0577	/* 8188F */
#define RTW8189F_BCN_DIS_TSF_UDT		__BIT(4)
#define RTW8189F_BCN_EN_BCN_FUNCTION		__BIT(3)

/* WMAC configuration. */
#define RTW8189F_REG_RCR			0x0608
#define RTW8189F_REG_RX_DRVINFO_SZ		0x060f
#define RTW8189F_REG_MACID			0x0610
#define RTW8189F_REG_BSSID			0x0618
#define RTW8189F_REG_MAR			0x0620
#define RTW8189F_REG_USTIME_EDCA		0x0638	/* 8188F */
#define RTW8189F_REG_MAC_SPEC_SIFS		0x063a
#define RTW8189F_REG_ACKTO			0x0640
#define RTW8189F_REG_NAV_UPPER			0x0652
#define RTW8189F_REG_RXFLTMAP0			0x06a0
#define RTW8189F_REG_RXFLTMAP1			0x06a2
#define RTW8189F_REG_RXFLTMAP2			0x06a4
#define RTW8189F_REG_RX_PKT_LIMIT		0x060c	/* 8188F */
#define RTW8189F_REG_C2HEVT_CLEAR		0x01af

/* REG_RCR bits (default ReceiveConfig = 0x700060ce, no AAP / no APPFCS:
 * HW strips the 4-byte FCS, so RX frames need no trimming). */
#define RTW8189F_RCR_APP_MIC			__BIT(30)
#define RTW8189F_RCR_APP_ICV			__BIT(29)
#define RTW8189F_RCR_APP_PHYST_RXFF		__BIT(28)
#define RTW8189F_RCR_HTC_LOC_CTRL		__BIT(14)
#define RTW8189F_RCR_AMF			__BIT(13)
#define RTW8189F_RCR_ADF			__BIT(11)
#define RTW8189F_RCR_AICV			__BIT(9)
#define RTW8189F_RCR_ACRC32			__BIT(8)
#define RTW8189F_RCR_CBSSID_BCN			__BIT(7)
#define RTW8189F_RCR_CBSSID_DATA		__BIT(6)
#define RTW8189F_RCR_AB				__BIT(3)
#define RTW8189F_RCR_AM				__BIT(2)
#define RTW8189F_RCR_APM			__BIT(1)
#define RTW8189F_RCR_AAP			__BIT(0)
#define RTW8189F_RCR_DEFAULT \
	(RTW8189F_RCR_APP_MIC | RTW8189F_RCR_APP_ICV | \
	 RTW8189F_RCR_APP_PHYST_RXFF | RTW8189F_RCR_HTC_LOC_CTRL | \
	 RTW8189F_RCR_AMF | RTW8189F_RCR_CBSSID_BCN | \
	 RTW8189F_RCR_CBSSID_DATA | RTW8189F_RCR_AB | \
	 RTW8189F_RCR_AM | RTW8189F_RCR_APM)

/* REG_CR network type field. */
#define RTW8189F_CR_NETTYPE_M			0x30000
#define RTW8189F_CR_NETTYPE(x)			(((x) & 0x3) << 16)
#define RTW8189F_NT_LINK_AP			0x2

/* BB register file and RF LSSI access (rtl8188f_phycfg.c). */
#define RTW8189F_BB_HSSI_P1			0x0820	/* rFPGA0_XA_HSSIParameter1 */
#define RTW8189F_BB_HSSI_P2			0x0824	/* rFPGA0_XA_HSSIParameter2 */
#define RTW8189F_BB_LSSI_WRITE			0x0840	/* rFPGA0_XA_LSSIParameter */
#define RTW8189F_BB_LSSI_READBACK		0x08a0	/* rFPGA0_XA_LSSIReadBack */
#define RTW8189F_BB_HSPI_READBACK		0x08b8	/* TransceiverA_HSPI_ReadBack */
#define RTW8189F_BB_RFMOD			0x0800	/* rFPGA0_RFMOD */
#define RTW8189F_BB_RFMOD_CCK_EN		__BIT(24)
#define RTW8189F_BB_RFMOD_OFDM_EN		__BIT(25)
#define RTW8189F_LSSI_READ_ADDR_M		0x7f800000
#define RTW8189F_LSSI_READ_EDGE			0x80000000
#define RTW8189F_LSSI_READBACK_M		0x000fffff

/* RF register indices (RF6052). */
#define RTW8189F_RF_CHNLBW			0x18	/* channel in bits[7:0] */
#define RTW8189F_RF20_CHNLBW_BW_M		0x0c00	/* bits[11:10] */
#define RTW8189F_RF20_20MHZ			0x0c00	/* BIT10|BIT11 */

/* TX power index registers (PHY_SetTxPowerIndex_8188F); one byte per rate. */
#define RTW8189F_TXAGC_OFDM6_18			0x0e00	/* 6/9/12/18M */
#define RTW8189F_TXAGC_OFDM24_54		0x0e04	/* 24/36/48/54M */
#define RTW8189F_TXAGC_CCK1			0x0e08	/* bits[15:8] = 1M */
#define RTW8189F_TXAGC_CCK2_11			0x086c	/* 2M b1 / 5.5M b2 / 11M b3 */

/*
 * IQK (TX/RX I/Q imbalance calibration) register file, vendor halrf_8188f.c.
 * Field writes use the vendor shift convention: data is shifted to the
 * mask's lowest set bit ((data << ctz(mask)) & mask).
 */
#define RTW8189F_BB_IQK				0x0e28	/* IQK mode on/off, bits[31:8] */
#define RTW8189F_BB_TXIQK_TONE_A		0x0e30
#define RTW8189F_BB_RXIQK_TONE_A		0x0e34
#define RTW8189F_BB_TXIQK_PI_A			0x0e38
#define RTW8189F_BB_RXIQK_PI_A			0x0e3c
#define RTW8189F_BB_TXIQK			0x0e40	/* rTx_IQK = rIQK_AGC_Rsp */
#define RTW8189F_BB_RXIQK			0x0e44
#define RTW8189F_BB_IQK_AGC_PTS			0x0e48
#define RTW8189F_BB_IQK_AGC_RSP			0x0e4c
#define RTW8189F_BB_TXPW_BEFORE_IQK_A		0x0e94
#define RTW8189F_BB_TXPW_AFTER_IQK_A		0x0e9c
#define RTW8189F_BB_RXPW_BEFORE_IQK_A2		0x0ea4
#define RTW8189F_BB_RXPW_AFTER_IQK_A2		0x0eac
#define RTW8189F_BB_XA_RXIQK			0x0c14	/* RX IQC matrix */
#define RTW8189F_BB_ECCA_THRESHOLD		0x0c4c
#define RTW8189F_BB_AGC_RSSI_TABLE		0x0c78
#define RTW8189F_BB_XA_TXIQK			0x0c80	/* TX IQC matrix */
#define RTW8189F_BB_XC_TXAFE			0x0c94
#define RTW8189F_BB_RXIQK_EXT_ANT		0x0ca0
#define RTW8189F_BB_RXIQK_INITGAIN		0x0c50	/* byte0, save/restore */
#define RTW8189F_BB_CAL_LIFETIME		0x0520	/* [23:16] = 0xff for IQK */
#define RTW8189F_BB_TRX_PATH_ENABLE		0x0c04
#define RTW8189F_BB_TR_MUX_PAR			0x0c08
#define RTW8189F_BB_XCD_RF_INTERFACE_SW		0x0874

/* RF register indices used by the IQK/LCK sequences (RF6052). */
#define RTW8189F_RF_WE_LUT			0xef	/* bit19: table LUT mode */
#define RTW8189F_RF_RCK_OS			0x30
#define RTW8189F_RF_TXPA_G1			0x31
#define RTW8189F_RF_TXPA_G2			0x32
#define RTW8189F_RF_LOK				0x08	/* LOK result */

/* IQK constants (vendor MAX_TOLERANCE / IQK_DELAY_TIME_8188F). */
#define RTW8189F_IQK_MAX_TOLERANCE		5
#define RTW8189F_IQK_DELAY_MS			25

/*
 * TX power PG section of the eFuse logical map (vendor pg_txpwr_saddr):
 * 18 bytes for path A: CCK-1T base for 6 CCK channel groups (1-2, 3-5,
 * 6-8, 9-11, 12-13, 14), OFDM-1T/BW40 base for 5 groups (1-2, 3-5, 6-8,
 * 9-11, 12-14), then 4-bit signed diffs; byte 18 holds BW20-1T (MSB) and
 * OFDM-1T (LSB).  Bases > 63 are invalid and fall back to the IC-default
 * table (rtl8188f_pg_txpwr_def_info).
 */
#define RTW8189F_EFUSE_TXPWR_OFF		0x10
#define RTW8189F_TXPWR_MAX			63
#define RTW8189F_TXPWR_DEF_CCK			0x22
#define RTW8189F_TXPWR_DEF_OFDM			0x27
#define RTW8189F_TXPWR_DEF_OFDM_DIFF		4

/* System interrupt mask register (8188F). */
#define RTW8189F_REG_HSIMR			0x0058

/*
 * TX descriptor (40 bytes; RTL8188F SDIO: the CMD53 FIFO write itself
 * submits the frame, no OWN bit, dword7[15:0] carries a checksum over
 * the first 32 bytes, HW sequence numbers enabled via REG_HWSEQ_CTRL).
 */
#define RTW8189F_TXDESC_SIZE			40
#define RTW8189F_TXDESC_QSEL_MGNT		0x12

/* dword0: [15:0] packet size, [23:16] offset (= descriptor size). */
#define RTW8189F_TXDW0_PKTLEN_M			0x0000ffff
#define RTW8189F_TXDW0_OFFSET_S			16
#define RTW8189F_TXDW0_BMC			__BIT(24)
/* dword1: [12:8] queue select, [20:16] rate id. */
#define RTW8189F_TXDW1_QSEL_S			8
#define RTW8189F_TXDW1_RATEID_S			16
/* dword2: BIT19 SPE_RPT (firmware CCX TX report request). */
#define RTW8189F_TXDW2_SPE_RPT			__BIT(19)
/* dword3: BIT8 use_rate, BIT17 retry_limit_enable, [23:18] data retry limit. */
#define RTW8189F_TXDW3_USE_RATE			__BIT(8)
#define RTW8189F_TXDW3_RETRY_LIMIT_EN		__BIT(17)
#define RTW8189F_TXDW3_DATA_RETRY_LIMIT_S	18
/* dword4: [6:0] TX rate. */
#define RTW8189F_TXDW4_RATE_M			0x7f
/* dword6: [11:0] SW_DEFINE, echoed in the C2H TX report. */
#define RTW8189F_TXDW6_SW_DEFINE_M		0x00000fff
/* dword7: [15:0] SDIO checksum. */
#define RTW8189F_TXDW7_CHKSUM_M			0x0000ffff
/* dword8: BIT15 hwseq enable. */
#define RTW8189F_TXDW8_HWSEQ_EN			__BIT(15)
/* dword9: [23:12] sequence. */
#define RTW8189F_TXDW9_SEQ_S			12

/* TX rate indices (DESC8188F_RATE*) and rate-adaptive table ids. */
#define RTW8189F_RATE_1M			0x00
#define RTW8189F_RATE_6M			0x04
#define RTW8189F_RATEID_G			7	/* RATEID_IDX_G */

#endif /* !_DEV_SDMMC_RTW8189F_REG_H_ */
