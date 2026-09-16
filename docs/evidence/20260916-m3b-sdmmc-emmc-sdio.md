# M3-B: fsl_sdmmc + the eMMC block device (FatFs volume 3) + SDIO card bring-up

Date: 2026-09-16
Board: Indium Engine RK3568-E4AP5G1-ITX (E4AP5G1-ITX), cold power cycles per KI-001
Image: `freertos.bin`, 317728 bytes, sha256 `15fba3165d69fc6e5a25ad3004460d880ad96a65adfeaa3400a7be285079d2de`
Repo state: `freewebcamera/` at this round's commits (M3-B series)
Scope: what this round claims, what it does not, and the raw evidence for both.

This round's method was fixed by the operator: **reproduce the previous working
implementation first, then port, and only clean-room once it runs.** §1 records
that reproduction; §5 records the three defects that the port had introduced.

---

## 1. The reference reproduction (ran first, before touching the port)

The standalone SDK's eMMC example was rebuilt from source and run on this board
to re-establish the baseline that the port is measured against. Its `example/peripherals/sd`
had been built in Aug 2026 but never re-verified after the tree was moved, so the
artifacts in the tree carried dead `/home/zhugy/workspace/bsd_virt_lab/...` paths.

Rebuild (config `configs/rk3568_aarch64_el1_itx_emmc.config`: polling,
`CONFIG_RK3568_SD_READ_ONLY=y`, `CONFIG_NON_CACHEABLE=y`, letter shell):

```
export AARCH64_CROSS_PATH=…/xpack-aarch64-none-elf-gcc-13.2.1-1.1
make load_kconfig LOAD_CONFIG_NAME=rk3568_aarch64_el1_itx_emmc
make clean && make -j
```

Result: linked clean, `rk3568_aarch64_itx_sd.bin` sha256
`0ecfa6cad45e188a23fcf3ff0d10adc931b2bf1a2a8773a7b0cada427a837906`, no
`bsd_virt_lab` strings left in `build/`. Booted over TFTP to the letter shell
(`rockchip:/$`), then:

```
sd fdwmshc_emmc_detect_example
  Name: ARJ11 [@0x2]
    Size: 29 GB
    Bus-Speed: 52 MHz
    Timing: High-Speed
    Bus Width: 8-bit
    Boot Partiion: 8192
    User Partiion: 61079552
  =================== eMMC Card Info ==================
    Capacity        : 29824 MiB
    CID Manufacturer: 0xf4
    CID OEM/App ID  : 0x122
    CID Product     : ARJ11
    CSD structure   : 3
    CSD spec ver    : 4
    CSD transfer spd: 0x32
    EXT_CSD cardType: 0x57
    EXT_CSD cache   : 1
  FDwmshcEmmcDetectExampleEx@119: eMMC detect example [success].

sd fdwmshc_emmc_read_example
  eMMC init success.
  0xaa9fbc0: 02 00 EE FF FF FF 01 00 00 00 9A 64 11 00 00 00
  0xaa9fbf0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 55 AA
  0xaa9fc00: 45 46 49 20 50 41 52 54 00 00 01 00 5C 00 00 00   EFI PART
  FDwmshcEmmcReadExampleEx@108: FDwmshc eMMC read example [success].
```

That is the protective MBR (`55 AA`) plus a `EFI PART` GPT header read from eMMC
LBA 0 — the same content the 2026-08-08 bring-up commit recorded, and the same
geometry (61079552 × 512 B = 29 824 MiB) that U-Boot and NetBSD report.

**Critical scope limit of the reference:** its build has
`CONFIG_RK3568_SD_READ_ONLY=y` and rejects every write at three layers, so the
reference proves the *read* path only. Its own comment in
`drivers/mmc/fdwmshc/fdwmshc.c:349` says the data path is SDMA "ADMA2 待查"
(ADMA2, unexamined). Both facts matter in §5.

## 2. What was verified today, on hardware (the port)

Log lines quoted verbatim from the serial console of the accepted image.

| # | Claim | Evidence (quoted) |
|---|---|---|
| 1 | eMMC enumerated on the DWC MSHC controller, geometry matches U-Boot/NetBSD/Linux ground truth | `sdmmc: eMMC man 0xf4 ARJ11, 29824 MiB, 8-bit 24MHz` |
| 2 | CID/CSD/EXT_CSD decoded correctly, including the register layout that had been in dispute | `sdmmc: CSD struct=3 spec=4, cardType=0x57, flags=0x1357`, `sdmmc: ext_csd[192]=08 [196]=57 [3]=00 [160]=07` |
| 3 | Four block devices bound; eMMC is volume 3 | `fs: 4 block device(s) bound`, `stor` → `3 emmc0   29824 MB  not mounted` |
| 4 | MBR written to the eMMC | `stor fdisk 3 yes` → `stor: emmc0: MBR written, one partition, whole disk` |
| 5 | FAT formatted | `stor mkfs 3 yes` → `stor: emmc0 formatted` |
| 6 | Mounted | `stor mount 3` → `stor: mount emmc0: FR_0` |
| 7 | **8 MiB write + read-back verify on the eMMC** (the write path is the part no prior reference proved) | `stor wr 3 /big.bin 8388608` → `3:/big.bin: 8388608 bytes written and verified, 577 ms (14197 KiB/s)` |
| 8 | Directory listing | `stor ls 3` → `     8388608       big.bin` / `3:/: 1 entry, FR_0` |
| 9 | Data survives a power cycle; a fresh boot auto-mounts and re-reads it | cold boot 2: `fs: emmc0 mounted, FAT32, 29820 MB total, 29812 MB free`; `stor ls 3` → `big.bin` 8388608 still present |
| 10 | SDIO card on sdmmc0 enumerated (RTL8189FTV, no wireless driver) | `sdio: card 24c:f179, 1 IO func(s), CCCR v0.2, bus 50000 kHz (HS)` and `SDIO READY` |
| 11 | No regression on the M3-A storage line | `fs: sata0 mounted, FAT32, 122089 MB total`, `fs: sata1 mounted, …`, `fs: nvme0 mounted, FAT32, 244168 MB total` in every boot |
| 12 | No regression on the M0/M1 anchors | `ITS LPI OK (n=66)`, `M0 ANCHORS DONE`, `net: up (2 of 2 ports)`, `NET READY` in every boot |

`stor wr` regenerates the byte pattern on read-back and compares, so #7 is a real
data-integrity check, not a status check.

## 3. Cold-boot series

Three consecutive cold boots (KI-001: ≥6 s off, 12 s settle) of the accepted
image:

| Boot | eMMC enumerated | eMMC mount | SDIO | SATA ×2 / NVMe | M0 anchors |
|---|---|---|---|---|---|
| 1 (boot-185629, pre-format) | 29824 MiB, 8-bit | none (`FR_13`, no FAT) | ok | ok | ok |
| 2 (boot-185842) | 29824 MiB, 8-bit | FAT32 29820 MB | ok | ok | ok |
| 3 (boot-185932) | 29824 MiB, 8-bit | FAT32 29820 MB | ok | ok | ok |

Boots 2 and 3 are also the persistence proof: the MBR/FAT32 created in boot 1's
session were found and mounted by a fresh boot with no host-side setup, and the
8 MiB file written before the power cycle was listed again after it.

## 4. Numbers for the record

| What | Value |
|---|---|
| eMMC write+read verify (8 MiB file, 32 KiB SDMA chunks) | 14.2 MB/s (577 ms) |
| eMMC capacity | 61079552 × 512 B = 29 824 MiB (matches U-Boot `mmc info` and NetBSD `ld0`) |
| eMMC operating point | 24 MHz from the CRU 24M source, 8-bit, High-Speed, polled SDMA |

## 5. The three defects the port had introduced (and how each was found)

The port was a faithful, line-for-line copy of the reference in every layer
except the controller driver and the memory model. The vendored protocol layer is
byte-identical (md5-verified against the reference), the dispatcher and both
host-glue files are semantically verbatim. Three defects lived outside that:

**5.1 The EXT_CSD read returned all zeros, so the card measured 0 MiB.**
Two causes compounded. First the port had replaced the reference's SDMA with
ADMA2 while never setting `HOST_CTRL2.HOST_VER4_EN`, so the controller fetched
no descriptor and still posted transfer-complete. Second, and the actual reason
the symptom survived fixing the first, the staging buffer here is ordinary
write-back-cacheable memory (the reference build maps its buffers
`MT_NORMAL_NC`, so its driver contains no cache maintenance at all). The protocol
layer memsets that buffer to zero immediately before an EXT_CSD read
(`third-party/sdmmc/mmc/fsl_mmc.c`, the `memset(alignBuffer, 0, …)` before CMD8),
leaving dirty zero cache lines that were written back over the device's data; the
port's invalidate ran only *after* the transfer finished, which is too late to be
deterministic. Fixed by restoring SDMA (the configuration the reference proves on
this board) and moving a `flush_invalidate` to *before* issuing a read, keeping
the post-completion invalidate. Diagnosis: a one-shot register + buffer dump
added to the data-error path (`dwc_mshc_dump_state`), which printed
`buf[0..3]=00000000 …` where the reference had read `08 57`.

**5.2 Writes failed with `DATA_CRC | DATA_END_BIT` at 50/52 MHz.**
Systematic clock sweep, one cold boot each, identical code otherwise:

| CRU source / divider | Card clock | Reads | Writes |
|---|---|---|---|
| 24M source, bypass | 24 MHz | clean | **clean** |
| 50M source, bypass | 50 MHz | clean | `eint=0x60` DATA_CRC\|DATA_END_BIT |
| 50M source, /2 | 25 MHz | clean | `eint=0x60` |
| 100M source, /2 | 52 MHz | CMD6 index errors, card drops to 4-bit, CMD17/CMD12 time out | — |

So the failure tracks the **clock source/tap**, not the frequency: the 24M
source works at 24 MHz, the 50M tap fails even at 25 MHz, and the 100M tap fails
outright. The board's eMMC runs at 24 MHz. The working point was read back from
the registers to confirm the port and U-Boot agree on everything else that
governs host→card output (`HC1=0x24`, `PWR=0x0b`, `EMMC_CON0=4`, DLL bypass,
`CMDOUT=0`), which is why this is recorded as a measured board limit rather than
a register bug. The reference never exercised the write direction, so its 52 MHz
read result does not cover this.

**5.3 Two silent configuration errors inherited from the reference.**
`HOST_CTRL1` bus width was written as `bit1 | bit5` for 8-bit; the spec and
U-Boot use `bit5` alone (U-Boot reads back `0x24`). And `PWR_CTRL` was written as
`0x1d` (3.0 V) because the reference's macro already pre-shifted the field and
was then shifted again on the way out; U-Boot writes `0x0b` (1.8 V | bus power),
which is what the board's `vcc_1v8` eMMC IO rail requires. Both were corrected.
Neither was sufficient alone to fix 5.2, but both were wrong.

## 6. Destructive actions performed (on the lab's authority for this round)

The eMMC was fully repartitioned and formatted by this acceptance run:

- `stor fdisk 3 yes` overwrote LBA 0 with a new MBR. This destroys the stale FIT
  copy that was parked at LBA 0x4000 and any prior content. The eMMC is a data
  disk on this board (it cannot be a boot source — the vendor miniloader's MMC1
  voltage select fails, measured 2026-09-12), so this is the authorized use.
- `stor mkfs 3 yes` formatted the resulting partition as FAT32.
- `sata0`, `sata1`, `nvme0` were **not** written this round; they were only
  mounted.

SPI-NOR (`0x100000` U-Boot FIT slot) and the sdmmc0 TF slot were not touched.

## 7. What this round did NOT do

- **Not 52 MHz writes.** The eMMC write path is proven at 24 MHz. Writes at
  50/52 MHz fail with a data CRC and are not claimed. Whether the mitigation is
  the DLL/`CMDOUT` phase, the CRU drive-phase (`EMMC_CON0`), or a board-level
  output-margin limit is not narrowed down yet.
- **Not HS200/HS400.** The board caps the controller at 52 MHz in its DTS; the
  reference recorded HS200 as failing on this board for signal-integrity reasons.
- **Not the wireless driver.** The SDIO card is enumerated (CCCR/FBR, CMD52
  read-back, single IO function). The RTL8189FTV function itself, CMD53 data, and
  net80211 integration are a later milestone.
- **Not interrupt mode.** Both controllers are driven polled; the interrupt
  transfer paths are carried over from the reference but run disabled
  (`config.enableIrq = false`).
- **Not eMMC boot.** Unchanged from the M3-A record: the eMMC is a storage
  target, not a boot source.

## 8. Where the pieces live

| Piece | Location |
|---|---|
| fsl_sdmmc protocol layer (vendored, from the standalone line) | `third-party/sdmmc/{common,mmc,sd,osa,sdio}/` |
| Vendor-header shadows + the two SDK headers the stack names | `port/adapters/sdmmc/shadow/` |
| Dispatcher + both host glues (ported line-for-line) | `port/adapters/sdmmc/sdmmc_dispatch.c`, `sdmmc_host_dwmmc.c`, `sdmmc_host_dwmshc.c` |
| OSA on CMSIS-RTOS2, IRQ parking, board clocks | `port/adapters/sdmmc/sdmmc_osa.c`, `sdmmc_glue_irq.c`, `sdmmc_board.h` |
| eMMC → `ARM_DRIVER_BLKDEV`, SDIO adapter, shell commands | `port/adapters/sdmmc/sdmmc_storage.c`, `sdmmc_adapter.c`, `sdmmc_cmds.c` |
| DW-MMC and DWC MSHC controller drivers | `drivers/dwc_mmc.c` (+`dwc_mmc_regs.h`), `drivers/dwc_mshc.c` (+`dwc_mshc_regs.h`) |
| Board tables | `drivers/rk3568_sdmmc.c` |
| Interface headers | `include/dwmmc.h`, `include/dwcmshc.h`, `include/sdio.h` |
| FatFs wiring (volume 3) | `port/adapters/fatfs/{ffconf.h,diskio.c,blkdev.c}` |
| Reference reproduction (outside this repo, read-only) | `/home/zhugy/workspace/rk3568_lab/os/standalone/example/peripherals/sd` |
