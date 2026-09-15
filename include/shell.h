/*
 * @file   shell.h
 * @brief  Project-owned interface: start the interactive console shell.
 *
 * This is a project interface, like the pcie/nvme/ahci/video gap headers, not a
 * standard CMSIS one. It lives in include/ because the application consumes it:
 * `app/` may only depend on `include/`, so an entry point it calls has to be
 * declared here rather than reached for inside `port/adapters/`.
 *
 * The implementation is port/adapters/cherrysh/cherrysh_adapter.c. Nothing in
 * that directory is visible to the application, and swapping which shell
 * provides this interface is a change to one adapter, not to the app.
 *
 * WHY IT RETURNS AN ERROR INSTEAD OF ABORTING
 *
 * A shell that fails to start is not a reason to halt a system that is otherwise
 * up and running with a working console. The caller decides: the M0 acceptance
 * app reports the failure and carries on, because every other acceptance anchor
 * is still meaningful without a shell.
 */

#ifndef FREEWEBCAMERA_SHELL_H
#define FREEWEBCAMERA_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the interactive shell: create its task, install the console receive
 * path, and arm the console interrupt.
 *
 * MUST be called from a task, i.e. after osKernelStart(). See
 * port/adapters/cherrysh/cherrysh_adapter.h for why.
 *
 * Returns 0 on success, non-zero if the shell could not be created. */
int shell_start(void);

/* True once the shell task is running and its prompt has been installed. Lets a
 * caller distinguish "shell came up" from "shell was never asked to". */
int shell_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* FREEWEBCAMERA_SHELL_H */
