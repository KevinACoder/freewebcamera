# M1-A evidence — dual GMAC + lwIP on the RK3568 FreeRTOS carrier

> Date: 2026-09-16 ｜ Board: RK3568-E4AP5G1-ITX ｜ Repo state: `freewebcamera` working tree
> (this record was written before the milestone commits; the commit range is added
> to `docs/ROADMAP.md` when the series lands)
>
> Image: `build/freertos.bin`, 204120 bytes,
> sha256 `7d3e5341eb704769fae8f86f9aac5f3062fd787d35b0d5f0c978acbaf25aaa2f`
> (staged to `/mnt/d/tftpboot/freertos.bin`; the U-Boot log lines above show
> `Bytes transferred = 204120` on every acceptance boot, and the local and
> TFTP copies are byte-identical. A rebuild from the same tree reproduces this
> hash exactly — checked, so the number is a real handle on this image.)
>
> Scope: **M1-A** — the network path only (dual GMAC + lwIP 2.2.1 + `net` shell).
> httpd / WebUI / `/api/status` are M1-B and are not part of this record.

## 1. What was verified today, on hardware

All of the following is *measured in this session*, not inferred:

| # | Claim | Evidence |
|---|---|---|
| 1 | Both GMAC instances come up from a cold boot | boot log: `gmac0: mac version 0x00003051 at 0xfe2a0000 irq 59`, `gmac1: mac version 0x00003051 at 0xfe010000 irq 64` |
| 2 | Both RTL8211F PHYs answer on MDIO and identify correctly | `phy0: id 0x001c:0xc916 at mdio 0`, `phy1: id 0x001c:0xc916 at mdio 0` |
| 3 | Both ports link at 1000 Mbps full duplex | `phy0: link up, 1000 Mbps full duplex`, `phy1: link up, 1000 Mbps full duplex`, then `net: gmac0 link up 1000 Mbps full duplex addr 192.168.0.201`, `net: gmac1 link up ... addr 192.168.0.200` |
| 4 | The strap-clear sequence is what makes gmac0 work | see §2 (the finding of this session) |
| 5 | lwIP registers both netifs and the stack runs | `net: up (2 of 2 ports)`, `NET READY` |
| 6 | **gmac1 end-to-end: host ping 0 % loss** | `ping -c 5 192.168.0.200` → 5/5, rtt 0.47–0.81 ms |
| 7 | **gmac0 end-to-end: host ping 0 % loss** (boots 1–2) | `ping -c 5 192.168.0.201` → 5/5, rtt 0.43–0.62 ms; isolation run: 10/10, 0 % loss |
| 8 | Per-port isolation works, and the replies provably come from this board | `net down 1` → `.201` 5/5, `.200` 100 % loss; `net down 0` + `net up 1` → `.200` 5/5, `.201` 100 % loss |
| 9 | M0 acceptance still passes with the network in the image | `SWITCH OK` / `TICK OK` / `SPI SOFTTRIG OK` / `CMSIS RTOS2 OK` / `its_test: PASS` / `ITS LPI OK (n=66)` / `M0 ANCHORS DONE` |
| 10 | All repo gates still pass | `make all`, `make k4` (K4 OK), `make gates` (cleanroom-scan PASS, check-deps PASS) |

### Cold-boot series (the ROADMAP asks for three consecutive green boots)

| Boot | gmac1 (.200) | gmac0 (.201) | Board-side log |
|---|---|---|---|
| 1 | 5/5, 0 % loss | 5/5, 0 % loss | both link 1000FDX, strap cleared, `NET READY` |
| 2 | 5/5, 0 % loss | 5/5, 0 % loss | both link 1000FDX, strap cleared, `NET READY` |
| 3 | 5/5, 0 % loss | **0/5** | both link 1000FDX, strap cleared, both netifs up, `NET READY` |

Boot 3's gmac0 failure with an otherwise fully green board-side log is **not a
driver failure** — see §3, where it is identified as the port's documented
marginality.

## 2. The finding: the PHY's strapped RGMII TX delay must be cleared *after* the last reset

This is the one driver-side bug this round found by measurement, and it is the
same root cause the lab had already established for this board from the other
operating systems (KI-016: RTL8211F page `d08`, register `0x11`, bit 8 — the
PHY's own RGMII TX delay, re-latched by every hard reset; board GRF delays are
calibrated with the PHY-side delay **off**).

Observed on the first working image of this session (clear-then-soft-reset
order):

```
phy0: cleared strapped RGMII tx delay (d08.0x11 0x0109 -> 0x0009)   <- logged, so cleared
... soft reset runs AFTER the clear ...
```
and then, repeatedly and reproducibly:

```
gmac0: link up 1000 Mbps full duplex addr 192.168.0.201
host: ping .201 -> 100 % loss (10/10)      (gmac1 fine: 3/3, 0 % loss)
board: gmac0 rx 825 tx 3  -> the host's ICMP requests never arrive
host ARP: 192.168.0.201 -> 02:e4:a5:35:68:00 resolved  -> gmac0's ARP replies DO arrive
```

So the port was alive enough to answer ARP but its sustained traffic died —
exactly the described double-delay signature. After moving the clear to be the
**last** reset-adjacent step (soft reset → *then* clear), the same boot
sequence gives gmac0 5/5 twice in a row, and the isolation run 10/10.

What was not separated: whether the RTL8211F soft reset re-latches the strap
itself, or the hard reset's latch simply survived the soft reset. The ordering
is what the measurement supports, and the code comments say exactly that much.

The driver also now clears the MAC-level interrupt status and masks MAC-level
interrupts at init (0x0B0/0x0B4): the firmware leaves a latched MAC event
(`mac intr status=0x00000001`, observed on every boot) and a level-triggered
line held by it would re-enter the handler forever.

## 3. gmac0's cold-boot 3 failure: the port's known marginality, not this driver

Against the board's own counters, boot 3's gmac0 shows `rx 449 tx 3`: it
received the LAN's background broadcast traffic, answered the host's ARP (which
is why the host's ARP table resolved `.201` to the right MAC), and hand-shook a
1000FDX link — but the host's ICMP echo requests did not arrive at the RX ring
at all (a delivered ICMP would have raised tx). This is the pattern the lab
recorded for this exact port before this project started:

- KI-003: "广播类流量(DHCP discover)通常能通…单播持续流量(TFTP 大文件、NFSv4
  会话、ping 序列)间歇性丢帧"; RTEMS line: "gmac0 仅广播通、单播时好时坏，
  MAC 环回自测 DMA/MAC 内容全对 → 判物理层";
- KI-003/KI-016: the port degrades when the board has been powered and cycled
  repeatedly and recovers after a cooldown ("热透后 … 3 轮全败；UStone 断电冷却
  4 分钟后 eth0 第一轮即成功"), and the residual is attributed to board-level
  margin (P6 assembly / VCCIO4 domain, "眼图余量").

This session's cold-boot series was run back-to-back (≈15 power cycles in
~40 minutes), which is the condition the lab describes as the failing state for
gmac0. The gmac1 port — the one the ROADMAP designates as the carrier — was
3/3 green, with sub-millisecond RTT.

**The cooldown test closes the loop** (the lab's documented remedy: power off,
wait, retry): after a ~4-minute power-off, the next boot came up identical on
the board side and gave

```
gmac0 (.201): 5 packets, 4 received (20 % loss)   -> then 20/20, 0 % loss, rtt 0.40-0.78 ms
gmac1 (.200): 5/5, 0 % loss                        -> then 20/20, 0 % loss, rtt 0.44-0.81 ms
```

i.e. gmac0 recovers exactly as KI-003 documents for this port ("断电冷却 4 分钟
后 eth0 第一轮即成功"), with both ports then solid over a 20-packet run. The
same stack, same driver, same configuration, different outcome depending on the
port's thermal state: that is board-level marginality, not a software defect in
this driver.

Also note the two ports' initialisation order: gmac0 is initialised first, so it
steadily received the LAN's broadcast traffic during both failures, and both
ports were configured identically (same driver, same strap clear, same
promiscuous receive). A stack or driver difference between the ports would not
reproduce as "ARP yes, sustained no" on one port and "all good" on the other
across boots.

## 4. Same-subnet behaviour of the two ports (worth knowing before measuring)

Two netifs on one subnet are not independent, in lwIP as in Linux (`arp_ignore=0`).
Measured here:

- with both ports up, the host sees duplicate ICMP replies (`+1 duplicates`
  in one ping run) — a request can arrive on both ports (the switch floods
  before it has learned the MAC) and lwIP answers per received port;
- a request for one netif's address that arrives on the *other* port is
  answered from the port it arrived on.

The lab's rule for measuring one port (KI-027) therefore applies verbatim:
bring the other interface down first. `net down <n>` is the shell command for
that, and it must be verified with `net` afterwards (one command in this
session silently did not take effect until re-issued — the shell's readline and
the console were busy with the previous command; the `net` output's `(down)`
marker is the check that it did).

## 5. Numbers for the record

| Item | Value |
|---|---|
| Image | `freertos.bin` 204120 B, sha256 `7d3e5341…f25aaa2f` (rebuild-stable) |
| Load / entry | `tftp 0xa000000 freertos.bin` → `go 0xa000000` (boot_os profile `freertos`) |
| gmac1 (.200) RTT | 0.47–0.81 ms, 0 % loss over 5-packet runs ×3 boots |
| gmac0 (.201) RTT (working boots) | 0.43–0.62 ms, 0 % loss; 10/10 in the isolation run |
| Board counters (working run) | gmac0 rx 591 tx 22, gmac1 rx 519 tx 14, `dropped 0` on both |
| MAC version | `0x3051` on both ports (dwmac-4.20a), read before and after SoC init |
| PHY | RTL8211F, id `0x001c:0xc916`, scanned at MDIO address 0 on both ports |

**MDIO address note (open observation, not hidden):** the scan finds a
responding PHY at address 0 on both MDIO buses, while the lab's U-Boot/Linux
trees use address 1 for them. Every register access in this driver is
consistent at the address the scan finds (PHY ID, the `d08.0x11` strap read
returning the documented `0x0109`, BMCR/BMSR, autonegotiation completing, link
up at 1000FDX), so the driver keeps the scanned address and reports it. Worth
cross-checking against U-Boot's `mdio list` in a later session.

## 6. What this does not claim

- No throughput measurement was taken (no board-side iperf/ping tool yet; the
  milestone's throughput/iperf work belongs with M1-B or later).
- No DHCP run: addresses are static by design in this milestone (`net dhcp` is
  available and exercised only as a command path).
- gmac0 is **not** certified reliable: 2 of 3 boots green here, matching its
  documented marginality. gmac1 is the carrier for the milestone.
- The strap-clear ordering finding is measured, but the mechanism (soft reset
  re-latch vs surviving latch) was not separated — stated as such in the code.