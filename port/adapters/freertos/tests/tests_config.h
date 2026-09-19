/*
 * @file   tests_config.h
 * @brief  Per-suite switches for the kernel test suite (TestRunner.c reads
 *         these; official ThirdParty-Template shape).
 *
 * 1 = the suite's tasks are created at boot and its check function runs in
 * the 5 s monitor sweep; 0 = the suite is compiled out of the runner but its
 * source stays in the tree. Excluded suites are excluded for a recorded
 * reason, not because the files are absent:
 *
 *   MATH (flop.c / sp_flop.c)      FPU context: the build is
 *                                  -mgeneral-regs-only, so the FP-register
 *                                  integrity test cannot even be compiled.
 *                                  Deferred until the FPU path is a deliverable.
 *   MESSAGE_BUFFER / STREAM_BUFFER / STREAM_BUFFER_INTERRUPT
 *                                  The official demos assume a single CPU
 *                                  (stream buffers are not thread safe); the
 *                                  reference SMP line flagged all three
 *                                  "needs SMP changes". Deferred to a round
 *                                  that ports their SMP notes properly.
 *   REGISTER (RegTests.c)          Same FPU blocker as MATH (the aarch64 reg
 *                                  test writes an FP lane).
 */

#ifndef FREEWEBCAMERA_TESTS_CONFIG_H
#define FREEWEBCAMERA_TESTS_CONFIG_H

/* v1 suites - the tick + task + heap feature surface. */
#define configSTART_TASK_NOTIFY_TESTS			1
#define configSTART_TASK_NOTIFY_ARRAY_TESTS		1
#define configSTART_BLOCKING_QUEUE_TESTS		1
#define configSTART_SEMAPHORE_TESTS			1
#define configSTART_POLLED_QUEUE_TESTS			1
#define configSTART_INTEGER_MATH_TESTS			1
#define configSTART_GENERIC_QUEUE_TESTS			1
#define configSTART_PEEK_QUEUE_TESTS			1
#define configSTART_RECURSIVE_MUTEX_TESTS		1
#define configSTART_COUNTING_SEMAPHORE_TESTS		1
#define configSTART_QUEUE_SET_TESTS			1
#define configSTART_QUEUE_OVERWRITE_TESTS		1
#define configSTART_EVENT_GROUP_TESTS			1
#define configSTART_INTERRUPT_SEMAPHORE_TESTS		1
#define configSTART_QUEUE_SET_POLLING_TESTS		1
#define configSTART_BLOCK_TIME_TESTS			1
#define configSTART_ABORT_DELAY_TESTS			1
#define configSTART_TIMER_TESTS				1
#define configSTART_INTERRUPT_QUEUE_TESTS		1
#define configSTART_DELETE_SELF_TESTS			1

/* Excluded suites - reasons in the file header. */
#define configSTART_MATH_TESTS				0
#define configSTART_MESSAGE_BUFFER_TESTS		0
#define configSTART_STREAM_BUFFER_TESTS			0
#define configSTART_STREAM_BUFFER_INTERRUPT_TESTS	0
#define configSTART_REGISTER_TESTS			0

#endif /* FREEWEBCAMERA_TESTS_CONFIG_H */
