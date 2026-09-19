/*
 * @file   rk3568.ld
 * @brief  Link script for the flat raw-binary image loaded by U-Boot.
 *
 * The image is loaded with `tftp 0xa000000 freertos.bin` and entered with
 * `go 0xa000000` (bootelf is unusable on this board). The load address must
 * match what boot_os uses; see the workspace ROADMAP.
 *
 * RAM window is capped at 96M. Memory above 0x10000000 is off limits on this
 * board - touching it raises an external abort - and the region below
 * 0x0a000000 belongs to firmware (ATF/OP-TEE), so the image gets exactly
 * [0x0a000000, 0x10000000).
 *
 * The linker-set sections (FSymTab/VSymTab) are kept because CherrySH builds
 * its command table from them; without the KEEP the linker discards commands
 * that nothing calls directly.
 */

ENTRY(_start)

RAM_BASE = 0x0a000000;
RAM_SIZE = 96M;

SECTIONS
{
	. = RAM_BASE;

	.text : {
		/* _start MUST be first. U-Boot's `go 0xa000000` starts executing
		 * at the load address without consulting any ELF header, so
		 * whatever sits here is what actually runs. Placing the vector
		 * table first meant the first instruction executed was a branch
		 * to the fault handler, which parked before the console existed
		 * - the image loaded, ran, and printed nothing.
		 *
		 * The vector tables are located by pointer, not by address:
		 * startup.S loads VBAR_EL1 from the boot_vectors symbol and the
		 * kernel port loads it from _freertos_vector_table. Neither
		 * needs offset 0. */
		KEEP(*(.text.boot))
		KEEP(*(.vectors))
		KEEP(*(.vectors.*))
		*(.text .text.*)
	}

	.rodata : ALIGN(8) {
		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)
	}

	.data : ALIGN(8) {
		__data_start = .;
		*(.data .data.*)
		__data_end = .;
	}

	/* CherrySH command and variable tables. */
	. = ALIGN(8);
	__fsymtab_start = .;
	.fsymtab : { KEEP(*(FSymTab)) }
	__fsymtab_end = .;
	. = ALIGN(8);
	__vsymtab_start = .;
	.vsymtab : { KEEP(*(VSymTab)) }
	__vsymtab_end = .;

	/* CherryUSB host class registry. usbh_initialize() brackets the table
	 * with these symbols; without the KEEP the linker discards the hub
	 * class entry (nothing calls it directly) and no device is ever
	 * claimed. */
	. = ALIGN(8);
	__usbh_class_info_start__ = .;
	.usbh_class_info : { KEEP(*(.usbh_class_info)) }
	__usbh_class_info_end__ = .;

	.bss (NOLOAD) : ALIGN(16) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	}

	/* Boot stack, below the FreeRTOS heap. */
	. = ALIGN(16);
	__stack_bottom = .;
	. = . + 64K;
	__stack_top = .;

	/* FreeRTOS heap (heap_4) grows from here; the kernel's
	 * configTOTAL_HEAP_SIZE must not exceed __heap_size. 4M covers the
	 * kernel-test image's suite (~100 resident tasks; the reference line
	 * ran the same suite with 20M); the main image's ucHeap stays at its
	 * configured 1M - this region only bounds it, bss is NOLOAD so the
	 * larger reservation costs nothing in the binary. */
	. = ALIGN(16);
	__heap_start = .;
	. = . + 4M;
	__heap_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}

	/* Guard: the image must fit the usable RAM window. */
	ASSERT(. <= (RAM_BASE + RAM_SIZE), "image exceeds the usable RAM window")
}
