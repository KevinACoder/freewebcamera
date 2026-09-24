/*
 * @file   dbg_scenario.h
 * @brief  gdb-lane diagnostics: the breakpoint probe targets (app layer).
 *
 * The shell command wrapper lives in the cherrysh adapter (commands are
 * adapter surface); these are the probe bodies it drives.
 */

#ifndef FREEWEBCAMERA_DBG_SCENARIO_H
#define FREEWEBCAMERA_DBG_SCENARIO_H

/* Written by dbg_probe_breakpoint() after the BRK returns: the evidence
 * word a gdb session can check (0x3568) without any other output path. */
extern volatile unsigned long dbg_probe_word;

/* BRK here (GDB_BRK_IMM): entering a stub session stopped at this frame is
 * the Z0 software-breakpoint acceptance stop. */
void dbg_probe_breakpoint(void);

/* Three nops: the single-step acceptance target (stepi walks 0x..100 ->
 * 0x..104 shape). */
void dbg_probe_step(void);

#endif /* FREEWEBCAMERA_DBG_SCENARIO_H */
