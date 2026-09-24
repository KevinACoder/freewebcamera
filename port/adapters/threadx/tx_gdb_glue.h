/*
 * @file   tx_gdb_glue.h
 * @brief  Adapter-facing entry points of the gdb stub wiring.
 */

#ifndef TX_GDB_GLUE_H
#define TX_GDB_GLUE_H

/* Register the dbgport (console UART polled access) and pre-configure the
 * debug architecture (OS lock clear, slot counts). Call once from
 * tx_application_define, before anything can trap. */
void tx_gdb_init(void);

/* Polled Ctrl-C watcher. Called from the tick dispatch (IRQ context):
 * drains the RX FIFO - which no one else reads, this image never arms the
 * console RX interrupt - and turns 0x03 into a stub break-in. */
void tx_gdb_tick_poll(void);

#endif /* TX_GDB_GLUE_H */
