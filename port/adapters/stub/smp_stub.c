/*
 * @file   smp_stub.c
 * @brief  Kernel-replacement stub for the SMP secondary entry point.
 *
 * app/main.c calls board_smp_start_secondaries() (port/board/common/smp.c,
 * real board code with no kernel dependency - it links into the k4 stub
 * build directly). But that code addresses freertos_secondary_entry, which
 * lives in smp_secondary.S - assembly the k4 build deliberately excludes
 * (only startup.S is kept, and the secondary path hands off to the kernel
 * port's uxPortSecondaryMain, which cannot exist without a kernel).
 *
 * So, like the other stubs: a fake symbol so the layers under test still
 * link. Never runs - the k4 image is never booted.
 */

char freertos_secondary_entry[1];
