# M3-A: FatFs over the three block devices (SATA ×2 + NVMe), with MSI completions

Date: 2026-09-16
Board: Indium Engine RK3568-E4AP5G1-ITX (E4AP5G1-ITX), cold power cycles per KI-001
Image: `freertos.bin`, 256224 bytes, sha256 `064d4d05648b12bdec34f489d94bd1ffcaf8cb018ed357ca47469159de5afd64`
Repo state: `freewebcamera/` at this round's commits (M3-A series)
Scope: what this round claims, what it does not, and the raw evidence for both.

An earlier image of the same round (`b049f9a243afba0478a6f69ed35f5983470632a4e40ec390807953ee6202b201`)
carried a `stor mkfs` defect (see §4.2). The three cold boots in §2 and every
number in §3 come from the fixed image above.

---

## 1. What was verified today, on hardware

Log lines are quoted verbatim from the serial console.

| # | Claim | Evidence (quoted) |
|---|---|---|
| 1 | Both on-chip SATA controllers taken over; device geometry matches what U-Boot's `scsi scan` reported in the same boot | `ahci0: port 0, 250069680 sectors of 512 bytes (122104 MiB), Gen3` and the same for `ahci1`; U-Boot printed `Capacity: 122104.3 MB = 119.2 GB (250069680 x 512)` for both disks |
| 2 | Both PCIe links inherited from firmware, no PHY/reset touched | `pcie0: firmware link inherited (bus 0-1), link Gen2 x1 (max Gen2 x1)`, `pcie1: firmware link inherited (bus 2-3), link Gen1 x1 (max Gen3 x2)` |
| 3 | NVMe controller found behind pcie2x1, namespace sized from its own Identify data | `nvme0: 126f:2263 at 0xf4300000, ns1: 500118192 blocks of 512 bytes (244198 MiB)` |
| 4 | MSI-X programmed and an LPI mapped through the ITS for that function | `its: 01:00.0 got 1 message irq(s) (event 0 -> intid 8192)`, `msix: 126f:2263 rid 0100, 1 vector(s), table BAR0+0x2000 (16 entries)` |
| 5 | **The endpoint→ITS doorbell path actually delivers an interrupt** (first time on this board in this project's line: `its_test` injects INT commands, it never exercised a device write to GITS_TRANSLATER) | `nvme0: message interrupts delivered (intid 8192, 1 so far)` — printed once, from task context, the first time a completion arrived *with* a delivery |
| 6 | Three block devices bound and mounted as FAT volumes | `fs: 3 block device(s) bound`, then `fs: sata0 mounted, FAT32, 122089 MB total, 122085 MB free`, `fs: sata1 mounted, ...`, `fs: nvme0 mounted, FAT32, 244168 MB total, 244160 MB free` |
| 7 | Write + read-back closure on NVMe (MSI path) | `2:/big.bin: 8388608 bytes written and verified, 1044 ms (7846 KiB/s)` |
| 8 | Write + read-back closure on both SATA ports (polled AHCI path) | `0:/big.bin: 4194304 bytes written and verified, 51 ms (80313 KiB/s)`, `1:/big.bin: 4194304 bytes written and verified, 50 ms (81920 KiB/s)` |
| 9 | Directory listing through FatFs | `     4194304       big.bin` / `0:/: 1 entry, FR_0` |
| 10 | Data survives a power cycle; a fresh boot re-mounts and re-reads it | after cold boot 3: `stor rd 2 /big.bin` → `2:/big.bin: 8388608 bytes, fnv1a e3472c45, 1028 ms (7968 KiB/s), FR_0` |

The write/verify command (`stor wr`) regenerates the byte pattern on read-back
and compares — it is not a status check: a write that reported success but
landed elsewhere fails at the first mismatching byte.

## 2. Cold-boot series

Three consecutive cold boots (KI-001: ≥6 s off, 12 s settle) of the accepted
image, each with the storage anchors present:

| Boot | AHCI ×2 | PCIe ×2 | NVMe | MSI delivered | FAT mounts |
|---|---|---|---|---|---|
| 1 (boot-130045, first boot of this image, before the disks were formatted) | ok | ok | ok | yes | none (FR_13, "no FAT volume") |
| 2 (boot-131211) | ok | ok | ok | yes | sata0, sata1, nvme0 — all FAT32 |
| 3 (boot-131250) | ok | ok | ok | yes | sata0, sata1, nvme0 — all FAT32 |

Boot 2 and 3 also serve as the persistence test: the volumes were created in
boot 1's session and mounted automatically by `fs_start()` in the next two
boots, with the filesystem reporting its own capacity (122089 MB / 244168 MB)
rather than the device size.

The M0 anchors (SWITCH/TICK/SPI/CMSIS RTOS2/ITS LPI ladder) stayed green in all
three boots; nothing in this round regressed them.

## 3. Numbers for the record

| What | Value |
|---|---|
| NVMe sequential write+read (8 MiB file, 64 KiB chunks → 8 KiB NVMe commands) | 7.8 MB/s (~1024 write + 1024 read commands in 1044 ms) |
| SATA sequential write+read (4 MiB file, 64 KiB ATA DMA EXT chunks) | 80–82 MB/s |
| SATA geometry | 119 GB each, Gen3 link, 512-byte logical sectors |
| NVMe geometry | 238.6 GiB namespace, 512-byte LBA, MSI-X table 16 entries at BAR0+0x2000 |
| LPI handed to the NVMe function | INTID 8192 (event id 0) |
| Formatting | `f_fdisk` 1 partition (whole disk) + `f_mkfs` FAT32, ~90 s per 119 GB SATA disk |

## 4. Findings

### 4.1 The first shell command after a boot is swallowed (3/3 boots, pre-existing)

On every cold boot, the first command sent to the console produces the driver's
`[uart] rx irq entry: intid=150 iir=0000000c lsr=00000001` trace line and
nothing else: no echo, no command output. The second command behaves normally,
and the shell is provably alive in between (`uptime` answered when sent as the
second command in the first session).

This is the console receive path, which this round did not touch, and it is the
"unarmed re-entry masks instead of consuming" window described in
`port/board/its_test.c`'s neighbourhood of notes: an interrupt that arrives
before the consumer has re-armed is masked, and the byte stays in the FIFO.
It just had not been observable before, because this is the first milestone
where a command is sent immediately after boot rather than after a human pause.

Workaround for acceptance runs: send one throwaway line first. Worth a real fix
in the console driver (consume-on-unarmed, or arm the FIFO level earlier), as a
separate unit — not a storage defect.

### 4.2 `f_mkfs` with a zeroed parameter block is not "defaults" (fixed in this round)

First image: `stor mkfs 2` → `stor: mkfs nvme0 failed (FR_19)`. FR_19 is
`FR_INVALID_PARAMETER`: `f_mkfs` computes `fsopt = opt->fmt & (FM_ANY | FM_SFD)`
and rejects `fmt == 0` because it is neither `FM_FAT` nor `FM_FAT32`. Passing
`NULL` selects FatFs's own defaults (`FM_ANY` + automatic allocation unit),
which is what picks FAT32 for these volumes. Fixed in the command, and the
usage text now matches the behaviour (`mkfs <n> yes`, like `fdisk`).

### 4.3 The DBI window needed a page-table entry

`mmu.c` mapped the low 4 GiB; the DesignWare PCIe register file sits at
`0x3C0000000` (15 GiB). Added as `L1[15]` with exactly the two DBI frames
(4 MiB each) populated, everything else in that 1 GiB left invalid on purpose
so a stray access faults loudly. Without it the first iATU write would have
been a translation fault at boot.

### 4.4 The LPI priority had to move to the API-call level

The ITS driver's LPI property byte was `0xa0` (logical 10), which is *above*
`BOARD_IRQ_PRIORITY_API_CALL` (0xb0 raw) in GIC terms. An MSI handler that
wakes a waiter through an RTOS call would trip the kernel port's own priority
assertion. Now `0xb0`, the same level the GMAC and console handlers run at.

## 5. What this does not claim

- **Not** NCQ / multiple commands in flight: the AHCI path is slot 0, polled,
  one command at a time, as the ported driver is. The NVMe path is one command
  in flight on one I/O queue pair, single vector.
- **Not** eMMC or the SDIO/TF slot: this round is SATA ×2 + NVMe only. M3's
  eMMC/SDIO half (fsl_sdmmc + `rk3568_dwc` host) is untouched.
- **Not** WebUI pages served from storage (M3 acceptance item 5), and not
  `/api/status` storage fields — M1/M2 work.
- **Not** a filesystem with meaningful timestamps: no RTC is ported yet, so
  `FF_FS_NORTC` stamps one fixed date.
- **Not** INTx fallback or the non-X MSI capability path; MSI-X is what this
  endpoint exposes and what was exercised.
- **Not** a benchmark: the numbers in §3 are single-shot, one run each, with
  the console idle.
- The M0 anchors are re-observed, not re-verified beyond what §2 says.

## 6. Destructive actions performed (on the lab's authority for this round)

All three disks were repartitioned and formatted by this acceptance run:

- `nvme0` — the expendable test disk; carries the 8 MiB verified file.
- `sata0`, `sata1` — the two 119 GB SanDisk SD8SBAT128G1122 disks. Both were
  handed to `f_fdisk` (new MBR, one partition covering the disk) and `f_mkfs`.
  **Whatever was on them is gone**, including any NetBSD/GPT wedges from
  earlier lab lines; the NetBSD `netbsd-sata` root path (N0) would need a fresh
  image written before it can be used again.

SPI-NOR (`0x100000` U-Boot FIT slot) and the TF slot were not touched.

## 7. Where the pieces live

| Piece | Location |
|---|---|
| FatFs R0.16 (vendored, byte-identical) | `third-party/fatfs/` (ff.c, ff.h, ffunicode.c, diskio.h + license/readme/history) |
| FatFs integration | `port/adapters/fatfs/` (ffconf.h, diskio.c, blkdev.c, fatfs_os.c, fatfs_adapter.c, fatfs_cmds.c) |
| Block device interface | `include/blkdev.h` (ops + registry), implemented in the adapter |
| SATA/AHCI driver | `drivers/dwc_ahci.c` + `drivers/rk3568_sata.c` |
| PCIe backend | `drivers/dwc_pcie.c` + `drivers/rk3568_pcie.c` (+ `mmu.c` L1[15]) |
| MSI-X programming | `drivers/dwc_msix.c` |
| NVMe driver | `drivers/dwc_nvme.c`, `drivers/dwc_nvme_regs.h` |
| ITS MSI domain | `port/board/gicv3_msi.c` (+ `include/msi.h`) |
| Shell commands | `stor ...` in `port/adapters/fatfs/fatfs_cmds.c` |