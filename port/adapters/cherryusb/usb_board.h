/*
 * @file   usb_board.h
 * @brief  The CherryUSB adapter's own board facts: the RK3568 USB host
 *         register bases, interrupt IDs and the CRU/PMU/GRF constants the
 *         usb2phy1 domain sequence needs (the fparameters.h equivalents of
 *         the standalone line, cross-checked against DESIGN §10 and the
 *         NetBSD rk_usb2phy / Linux dts values).
 *
 * Kept here rather than in board_conf.h because only this adapter consumes
 * them. All GRF/PMU writes use the RK3568 "field << 16" write-enable style;
 * CRU and the SYSCON-style PHY GRFs share it, GPIO does not.
 *
 * @author zhugengyu
 * @date   19.09.2026
 */

#ifndef USB_BOARD_H
#define USB_BOARD_H

/* --- host controllers (EHCI, panel USB2.0 Type-A, each behind an onboard
 * CH334P hub; dts GIC_SPI 130/133 -> INTID +32) --- */
#define USBH_EHCI_NUM			2U
#define USBH_EHCI0_BASE			0xFD800000UL
#define USBH_EHCI0_IRQ			162U
#define USBH_EHCI1_BASE			0xFD880000UL
#define USBH_EHCI1_IRQ			165U

/* --- bus/power/clock blocks --- */
#define USBH_CRU_BASE			0xFDD20000UL
#define USBH_PMUCRU_BASE		0xFDD00000UL
#define USBH_PMU_BASE			0xFDD90000UL
#define USBH_GPIO3_BASE			0xFE760000UL
#define USBH_USB2PHY1_GRF_BASE		0xFDCA8000UL	/* SYSCON, hi-word WE */

/* PMU: PD_PIPE power domain (USB3 pipe + the shared USB bus island). */
#define USBH_PMU_BUS_IDLE_SFTCON0	0x050U
#define USBH_PMU_BUS_IDLE_ACK		0x060U
#define USBH_PMU_PD_PIPE_IDLE_BIT	(1U << 11)
#define USBH_PMU_PWR_GATE_SFTCON	0x0A0U
#define USBH_PMU_PWR_DWN_ST		0x098U
#define USBH_PMU_PD_PIPE_BIT		(1U << 8)

/* PMUCRU clkgate_con2: bit0 ref24m, bit1 xin_osc0_usbphy0_g, bit2
 * xin_osc0_usbphy1_g. CRU clkgate polarity: low-bit 1 = clock gated OFF,
 * 0 = enabled - so the enable write carries data 0. The usb2phy1 domain
 * needs all three (the PHY reference clocks gate off after U-Boot hands
 * over). */
#define USBH_PMUCRU_CLKGATE_CON2	0x188U
#define USBH_PMUCRU_USBPHY_GATES	0x00000007U

/* CRU soft resets. con14 bits4-9: H_USB2HOST0/ARB/UTMI + H_USB2HOST1/ARB/
 * UTMI (assert + release); con28 bit11: P_USB2PHY1_GRF pclk (release only);
 * con29 bits3-5: usb2phy1 POR / port reset (release only). HCLK_USB2HOST0/1
 * gates are on by default - no clock-id exists for them. */
#define USBH_CRU_SOFTRST_CON14		0x438U
#define USBH_CRU_SOFTRST_CON14_BITS	0x03F0U
#define USBH_CRU_SOFTRST_CON28		0x470U
#define USBH_CRU_SOFTRST_CON28_BITS	0x0800U
#define USBH_CRU_SOFTRST_CON29		0x474U
#define USBH_CRU_SOFTRST_CON29_BITS	0x0038U

/* usb2phy1 GRF port controls (offsets identical to usb2phy0):
 * OTG_CON0 suspend release = 0x0c00, HOST_CON1 host role = 0x1d2
 * (U-Boot/Linux/NetBSD agree), CLKOUT_CON2 bit4 cleared opens the 480m
 * output - the UTMI clock the EHCI runs from. */
#define USBH_USB2PHY_GRF_OTG_CON0	0x0000U
#define USBH_USB2PHY_GRF_HOST_CON1	0x0004U
#define USBH_USB2PHY_GRF_CLKOUT_CON2	0x0008U
#define USBH_USB2PHY_OTG_SUS_VAL	0x0C00U
#define USBH_USB2PHY_OTG_SUS_MASK	0x0FFFU
#define USBH_USB2PHY_HOST_SUS_VAL	0x01D2U
#define USBH_USB2PHY_HOST_SUS_MASK	0x01FFU
/* CLKOUT_CON2: write-enable bit4 with value 0 opens the 480m output. */
#define USBH_USB2PHY_CLKOUT_WE		0x0010U

/* VBUS enables on GPIO3: A0 = panel USB2.0 group (both EHCI roots), A1 =
 * USB3 socket group. Without the pull-up the ports have no power at all
 * (KI-004). Data-direction + data registers, hi-word write-enable style. */
#define USBH_GPIO3_SWPORT_DDR		0x0000U
#define USBH_GPIO3_SWPORT_DR		0x0008U
#define USBH_VBUS_PINS			((1U << 0) | (1U << 1))

/* Helper: a GRF-style write-enable word (field value in low half, the same
 * field bits in the high half acting as the write strobe). */
#define GRF_WR(bits, val)		(((bits) << 16) | (val))

/* Controller base by busid (bus N == EHCI N; no xHCI in this image). */
#define USBH_EHCI_BASE(id)		((uintptr_t)((id) == 0U ? \
					USBH_EHCI0_BASE : USBH_EHCI1_BASE))

#endif /* USB_BOARD_H */
