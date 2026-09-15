/*
 * @file   heap.c
 * @brief  Static heap region for FreeRTOS heap_4.
 *
 * configAPPLICATION_ALLOCATED_HEAP is set, so the kernel expects the
 * integrator to define ucHeap rather than declaring its own array. Doing it
 * here keeps the size in one place (the config) and lets it live in .bss,
 * which mmu.c maps as Normal cacheable memory.
 *
 * That mapping matters: heap_4 performs unaligned accesses, and a heap in
 * Device memory would trap or crawl.
 */

#include <stdint.h>

#include "FreeRTOS.h"

/* The kernel requires this exact name and type. */
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(16)));
