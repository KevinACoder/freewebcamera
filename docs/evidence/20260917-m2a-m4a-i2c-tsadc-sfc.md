# M2-A + M4-A: I2C (Rockchip v5) / TSADC / SFC read-only — 2026-09-17

- **Date**: 2026-09-17
- **Board**: RK3568 E4AP5G1-ITX, boot via `boot_os freertos` (tftp 0xa000000 + go)
- **Image**: `build/freertos.bin` 331848 B, sha256 `8241dd59e6d76ae497c9110701a68641e77cd280dfc263ec5f6be851b13bfd7f`
- **Repo state**: drivers/{rk_i2c,rk_tsadc,rk_sfc}.c + regs/h headers, port/adapters/periph/periph_cmds.c, Makefile +2 lines, tools/cleanroom-scan.sh trace-exception list
- **Scope**: I2C driver (CMSIS `Driver_I2C0/1`) + RX8025T + INA3221; SFC SPI-NOR read-only (`Driver_Flash0`); on-chip TSADC temperature — **TSADC blocked this round** (see §5)
- **User constraint**: SPI flash MUST NOT be written — enforced by design (see §3)

## 1. Decisions recorded (docs/DESIGN.md §14)

- **D29**: RK3568 I2C is **Rockchip v5, not DesignWare**; the M2 note pointing at embox
  `i2c_designware` (upstream Anton Bondarev code, Synopsys register layout) was a
  misreference. Truth source: the author's standalone-line port (2026-08-11, board-proven)
  + linux `i2c-rk3x.c` cross-check.
- **D30**: Temperature truth source = SoC TSADC. RK809 has **no readable temperature
  register** (only hotdie/TSD threshold bits at 0xf2; no rk8xx thermal driver exists in
  any mainline lineage). G7's "RK809 temperature via I2C" plan is dead.
- **D31**: SFC delivered READ-ONLY this round (M4-A). `ARM_DRIVER_FLASH` facade (SFC is a
  flash engine, not a general SPI master). Hard read-only triple gate (§3). GPIO1_C7
  (fspi_d2) deliberately NOT muxed — stays with eMMC rstnout (x1 reads never need it).

## 2. What was verified today, on hardware

| # | Claim | Evidence (serial, quoted) |
|---|---|---|
| 1 | I2C0 probe scan finds RK809 | `i2c0 scan:` `0x20 ack` (+`0x40 ack` — candidate SYR827 rail per DESIGN §8), `i2c0: 2 device(s)` |
| 2 | I2C1 probe scan matches Linux i2cdetect ground truth | `i2c1 scan:` `0x32 ack`/`0x40 ack`/`0x52 ack`, `i2c1: 3 device(s)` (Linux run 2026-09-14: 0x32 UU / 0x40 UU / 0x52 free) |
| 3 | RX8025T reads + seconds tick | `rtc: 2026-09-17 wd0 01:56:50 (raw 50 56 01 00 17 09 26)` → `...01:56:51` → `rtc: seconds tick +1 in 1.1 s` |
| 4 | RX8025T set + readback | `rtc set 0 18 1 17 9 26` → `rtc: 2026-09-17 wd0 01:18:00 (raw 00 18 01 00 17 09 26)` → `01:18:01` tick |
| 5 | INA3221 identity + rails | `pwr: ina3221 mfr 5449 die 3220 conf 7127`; `3V3: 3296 mV` (standalone 0x0CE0=3296 mV, exact), `5V0: 5040 mV` (NetBSD 5.040 V, exact), `12V: 12080 mV` (NetBSD 12.056 V, same magnitude) |
| 6 | SFC JEDEC ID | `sfc: jedec id ef 60 17 (W25Q64DW expects ef 60 17)` — matches U-Boot `sf probe` on the same boots (`Device 2: JEDEC id bytes: ef, 60, 17`) |
| 7 | SFC geometry + read-only facade | `sfc: 8192 KiB, sector 4096, page 256, READ-ONLY driver` |
| 8 | SPI-NOR FIT slot read (FIFO path, 16 B) | `100000: d0 0d fe ed 00 00 0b 76 00 00 00 38 00 00 0a dc` — FDT magic, reproduced identically across 3 cold boots |
| 9 | SPI-NOR read (DMA path, 598 B) | `sfc: read 598 bytes from 0x100000, 598 non-FF`; content = the U-Boot FIT's own FDT strings: `RK3568 E4AP5G1-ITX U-Boot FIT (build-fit.sh)`, `ARM Trusted Firmware`, `sha256`, `atf-1` — first line byte-identical to the FIFO read |
| 10 | Read-only + no-boot-damage | No write opcode can reach the wire (whitelist); after all testing the board still cold-boots from the same SPI NOR three consecutive times |

Cold-boot series (final image `8241dd59`): 3/3 green — all M0 anchors (SHELL READY,
SWITCH/TICK/SPI SOFTTRIG OK, CMSIS RTOS2 OK, ITS LPI OK, M0 ANCHORS DONE), SDIO READY,
FS READY, NET READY. Additional cold boots during bring-up (see §5) also 5/5 green.

## 3. Read-only guarantees (user constraint "spi flash 不要写")

1. `rk_sfc_exec_read()` has **no write direction at all**; opcode whitelist is
   `0x9F JEDEC / 0x5A SFDP / 0x05 RDSR / 0x03 READ` (rk_sfc_regs.h) — anything else
   never reaches the controller.
2. `Driver_Flash0.ProgramData / EraseSector / EraseChip` return
   `ARM_DRIVER_ERROR_UNSUPPORTED` and log a refusal line.
3. No shell command exposes a write/erase operation (`sfc info|jedec|sfdp|read` only).
4. Evidence of non-damage: U-Boot (which sha256-verifies every FIT component at boot)
   kept booting cleanly from the same flash across all 7 cold boots today.

## 4. Bugs found and fixed this round

- **I2C pending-transmit hang** (first board round): issuing a new START from the
  enabled-but-idle state left by a `xfer_pending` transmit never asserts STARTIPD
  (`i2c1 start timeout (con=0x4112b ipd=0x0)`). Every proven sequence on this controller
  starts from the disabled engine. Fix: a 1-byte `xfer_pending` transmit is buffered
  without touching the bus; the following MasterReceive replays it as the MRXRADDR byte
  of the proven ONE-SHOT TRX combined read. num>1 pending is refused (UNSUPPORTED).
- **SFC DMA phase offset**: a 598 B DMA read (non-word-aligned length) returned 3 leading
  garbage bytes and everything shifted (`66 69 64 2d d0 0d fe ed ...` instead of
  `d0 0d fe ed ...`). Two fixes: DMA chunks are word-aligned (1-3 byte tails run as their
  own FIFO chunk), and a settle delay between the command/address phase and
  DMA_TRIGGER (same hazard class as the FIFO path's first-word settle).
- **SFC FIFO first-word settle too short**: 3 us left a race window at the 24 MHz SCLK;
  an intermittent 3-byte phase shift was observed on a 16 B read across boots. Raised to
  10 us (head, tail and pre-DMA guards). After the fix: FIFO 16 B, FIFO 598 B and DMA
  paths agree byte-for-byte across two more cold boots.

## 5. What this round did NOT do / blocked

- **TSADC temperature: BLOCKED, not delivered.** The sensor never converts from this
  image: `DATA0/DATA1 = 0` persistently while every config register is verified written
  (`user_con=fc0 auto_con=13003`, GRF taps `grf after=00000107` = TSEN+ANA0/1/2 set,
  gates read open). Diagnostics across 5 boots:
  - CRU `CLKSEL_CON51` (tsadc_tsen mux/div) is **immutable from our EL1 image**: hiword,
    plain and 0xFFFF-enable stores all read back 0 while CON50/CON52 hold real values and
    CON77 reads its documented default (0xb). Linux-mainline/NetBSD measured working
    temperatures on this same board (38.3/36.7 C, 33.75/32.5 C), and the NetBSD CRU is a
    fixed-rate stub that never programs CON51 — i.e. those runs used the reset-default
    clocks (24 MHz straight through). The AUTO-period-vs-latency theory (1622 ticks =
    67.5 us at 24 MHz < the 97 us USER_INTER_PD_SOC latency) was tested with a 48750-tick
    period and did NOT unlock conversions.
  - Remaining suspects: an EL/secure-firmware interaction (OP-TEE locking or a required
    EL2-phase write), or a reset-cycle ordering detail. Next probe: write CON51 from
    U-Boot (EL3-adjacent path) before `go`, or run the identical sequence at EL2.
  - The `temp` command reports `cpu sensor not converting` honestly; no fake value is
    produced. Driver + command are merged and will work once the sensor comes up.
- M2 remainder (WebUI `/api/status`, N1 page cross-check) awaits M1-B httpd.
- M4 remainder (OTA write path + write-range gates) is future work by design (D31).

## 6. Destructive actions performed

None. No flash write/erase was attempted (driver cannot issue one). No eMMC/SD/ATA
operations beyond the boot-time mounts that M3 already established.

## 7. Numbers for the record

- Image: 331848 B (was 317.6 kB at M3-B); gates: cleanroom PASS (with D20 whole-file-port
  exception list), check-deps PASS, K4 link OK.
- I2C: 100 kHz both buses, polling; scan of 112 addresses per bus completes < 1 s.
- SFC: JEDEC after one priming read (continuous-read-mode guard); read path DMA ≥ 64 B,
  FIFO below; 16 KiB chunk cap.

## 8. Where the pieces live

| Piece | Path |
|---|---|
| I2C controller + CMSIS facade | `drivers/rk_i2c.c`, `rk_i2c.h`, `rk_i2c_regs.h` |
| TSADC (blocked, merged) | `drivers/rk_tsadc.c`, `rk_tsadc.h` |
| SFC/NOR read-only + Flash facade | `drivers/rk_sfc.c`, `rk_sfc.h`, `rk_sfc_regs.h` |
| Shell commands (i2c/rtc/pwr/temp/sfc) | `port/adapters/periph/periph_cmds.c` |
| Cleanroom whole-file-port exception list | `tools/cleanroom-scan.sh` |
