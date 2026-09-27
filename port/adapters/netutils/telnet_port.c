/*
 * @file   telnet_port.c
 * @brief  First-party telnetd: one CherrySH shell instance per connection.
 *
 * Upstream's netutils telnet/ was NOT vendored: it is built on the
 * RT-Thread console architecture (register a char device, switch the
 * console to it, redirect the one finsh thread). CherrySH has no console
 * device to switch - it is a multi-instance shell: every chry_shell_t
 * carries its own buffers and its own sput/sget I/O hooks, instances share
 * the FSymTab command table through pointers, and the running command gets
 * the session's shell pointer as argv[argc+1] (CSH_FROM_ARGV), so csh_printf
 * output lands back on the calling session. The honest port is therefore a
 * telnetd that spawns a second shell instance bound to the socket, while
 * the UART console keeps its own instance untouched - sessions coexist,
 * neither switches the other off.
 *
 * Per connection: chry_shell_t + RX ring (the socket task filters telnet
 * IAC sequences and CR/LF duplication, then parks bytes in the ring) + a
 * task that runs chry_shell_task_repl in a loop, exactly like the UART
 * shell task. The session binds itself into the netutils shim's output
 * router, so rt_kprintf/LOG_* from vendored commands executing in this
 * session land on the telnet client instead of the UART.
 *
 * No login/auth: the UART console has none either, and the board is a lab
 * device. Ctrl+C cancels the current line, as on the UART (long-running
 * commands are not interruptible - same semantics as the console).
 *
 * Commands:
 *   telnetd           start the listener (port 23)
 *   telnetd --stop    stop the listener (active sessions finish naturally)
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <lwip/sockets.h>
#include <cmsis_os2.h>
#include <rtthread.h>		/* netutils shim: session binding + kprintf */

#include "csh.h"
#include "cherrysh_adapter.h"
#include "chry_ringbuffer.h"

/* Link-script section bounds: the telnet session's shell instance shares the
 * UART shell's FSymTab command table through these pointers (re-declared
 * here because cherrysh keeps them local to its own adapter unit and the
 * upstream typedefs are anonymous, so they cannot move to the shared
 * header). */
extern const chry_syscall_t __fsymtab_start;
extern const chry_syscall_t __fsymtab_end;
extern const chry_sysvar_t __vsymtab_start;
extern const chry_sysvar_t __vsymtab_end;

#define TELNET_PORT		23
#define TELNET_SESSION_MAX	2
#define TELNET_TASK_STACK	4096
#define TELNET_RX_POLL_MS	20

/* telnet protocol bytes */
#define TN_IAC		255u
#define TN_DONT		254u
#define TN_DO		253u
#define TN_WONT		252u
#define TN_WILL		251u
#define TN_SB		250u
#define TN_SE		240u
#define TN_OPT_ECHO	1u
#define TN_OPT_SGA	3u

/* IAC filter states */
enum {
	TN_ST_TEXT = 0,		/* ordinary data */
	TN_ST_IAC,		/* saw 255 */
	TN_ST_CMD,		/* saw 255 cmd - next byte is the option */
	TN_ST_SB,		/* inside 250 ... 240 */
	TN_ST_SB_IAC,		/* inside SB, saw 255 (SE?) */
};

struct telnet_session {
	uint8_t in_use;
	int sock;
	chry_shell_t shell;
	chry_ringbuffer_t rx;
	uint8_t rx_storage[512];
	char history[1024];
	char prompt[64];
	char line[CONFIG_CSH_LNBUFF_SIZE];
	uint8_t iac_state;
	uint8_t iac_cmd;
	uint8_t prev_cr;
	osThreadId_t task;
};

static struct telnet_session sessions[TELNET_SESSION_MAX];
static int listen_sock = -1;
static volatile uint8_t listener_stop;

static void telnet_session_task(void *arg);

/* --- I/O hooks (one pair per session, bound at init) ------------------------- */

static inline struct telnet_session *sess_of(chry_readline_t *rl)
{
	/* same container_of the upstream core uses to get back from rl */
	return (struct telnet_session *)
		(((chry_shell_t *)(void *)((char *)rl -
					   offsetof(chry_shell_t, rl)))->user_data);
}

static uint16_t telnet_sput(chry_readline_t *rl, const void *data, uint16_t size)
{
	struct telnet_session *sess = sess_of(rl);
	int n;

	if (data == NULL || size == 0U) {
		return 0U;
	}
	n = send(sess->sock, data, size, 0);
	if (n < 0) {
		/* peer gone mid-write: break the session loop loose */
		shutdown(sess->sock, 0);
		return 0U;
	}
	return (uint16_t)size;
}

static uint16_t telnet_sget(chry_readline_t *rl, void *data, uint16_t size)
{
	struct telnet_session *sess = sess_of(rl);
	uint32_t got;

	if (data == NULL || size == 0U) {
		return 0U;
	}
	got = chry_ringbuffer_read(&sess->rx, data, size);
	return (uint16_t)got;
}

/* --- telnet wire filtering ----------------------------------------------------- */

/* Minimal RFC 854 compliance: answer our advertised options, refuse the
 * rest, swallow subnegotiation blobs, de-duplicate the CR LF / CR NUL line
 * endings telnet clients send (readline maps both CR and LF to Enter, so
 * an unfiltered pair would run one empty command per line). */
static void telnet_negotiate(struct telnet_session *s, uint8_t cmd, uint8_t opt)
{
	uint8_t reply[3] = { TN_IAC, 0, 0 };

	if (cmd == TN_DO) {
		reply[1] = (opt == TN_OPT_ECHO || opt == TN_OPT_SGA) ? TN_WILL
								     : TN_WONT;
	} else if (cmd == TN_WILL) {
		reply[1] = (opt == TN_OPT_ECHO || opt == TN_OPT_SGA) ? TN_DO
								     : TN_DONT;
	} else {
		return;		/* DONT/WONT: nothing to answer */
	}
	reply[2] = opt;
	(void)send(s->sock, reply, sizeof(reply), 0);
}

static void telnet_filter(struct telnet_session *s, const uint8_t *in, int n)
{
	ssize_t i;

	for (i = 0; i < n; i++) {
		uint8_t c = in[i];

		switch (s->iac_state) {
		case TN_ST_TEXT:
			if (c == TN_IAC) {
				s->iac_state = TN_ST_IAC;
				break;
			}
			if (c == '\n' && s->prev_cr) {
				s->prev_cr = 0;		/* CR LF: LF is the echo */
				break;
			}
			if (c == '\0' && s->prev_cr) {
				s->prev_cr = 0;		/* CR NUL */
				break;
			}
			s->prev_cr = (c == '\r');
			(void)chry_ringbuffer_write(&s->rx, &c, 1);
			break;
		case TN_ST_IAC:
			if (c == TN_IAC) {
				(void)chry_ringbuffer_write(&s->rx, &c, 1);
				s->iac_state = TN_ST_TEXT;
			} else if (c == TN_SB) {
				s->iac_state = TN_ST_SB;
			} else if (c >= TN_WILL && c <= TN_DO) {
				s->iac_cmd = c;
				s->iac_state = TN_ST_CMD;
			} else {
				s->iac_state = TN_ST_TEXT;
			}
			break;
		case TN_ST_CMD:
			telnet_negotiate(s, s->iac_cmd, c);
			s->iac_state = TN_ST_TEXT;
			break;
		case TN_ST_SB:
			if (c == TN_IAC) {
				s->iac_state = TN_ST_SB_IAC;
			}
			break;
		case TN_ST_SB_IAC:
			if (c == TN_SE) {
				s->iac_state = TN_ST_TEXT;
			} else {
				s->iac_state = TN_ST_SB;
			}
			break;
		default:
			s->iac_state = TN_ST_TEXT;
			break;
		}
	}
}

/* --- session lifecycle ------------------------------------------------------------ */

static void telnet_session_task(void *arg)
{
	struct telnet_session *s = arg;
	uint8_t buf[128];

	netutils_shim_bind_session(&s->shell);
	csh_printf(&s->shell,
		   "freewebcamera telnet - commands run on the board shell; "
		   "'help' lists them\n");

	while (1) {
		int n = recv(s->sock, buf, sizeof(buf), MSG_DONTWAIT);

		if (n > 0) {
			telnet_filter(s, buf, n);
		} else if (n == 0) {
			break;			/* peer closed */
		} else {
			/* EWOULDBLOCK: nothing to read - give the socket a
			 * rest, then let readline drain what the filter
			 * parked. A blocking SO_RCVTIMEO here would hold the
			 * line hostage between keystrokes; the poll delay
			 * costs nothing while idle. */
			(void)osDelay(TELNET_RX_POLL_MS);
		}
		(void)chry_shell_task_repl(&s->shell);
	}

	netutils_shim_unbind_session();
	(void)lwip_close(s->sock);
	s->in_use = 0;
}

static struct telnet_session *telnet_session_start(int sock)
{
	chry_shell_init_t init = { 0 };
	struct telnet_session *s = NULL;
	osThreadAttr_t attr = { 0 };
	int i;

	for (i = 0; i < TELNET_SESSION_MAX; i++) {
		if (!sessions[i].in_use) {
			s = &sessions[i];
			break;
		}
	}
	if (s == NULL) {
		const char *busy = "telnet: session table full\r\n";

		(void)send(sock, busy, strlen(busy), 0);
		return NULL;
	}

	s->in_use = 1;
	s->sock = sock;
	s->iac_state = TN_ST_TEXT;
	s->prev_cr = 0;
	if (chry_ringbuffer_init(&s->rx, s->rx_storage,
				 sizeof(s->rx_storage)) != 0) {
		s->in_use = 0;
		return NULL;
	}

	memset(&init, 0, sizeof(init));
	init.sput = telnet_sput;
	init.sget = telnet_sget;
	init.command_table_beg = &__fsymtab_start;
	init.command_table_end = &__fsymtab_end;
	init.variable_table_beg = &__vsymtab_start;
	init.variable_table_end = &__vsymtab_end;
	init.prompt_buffer = s->prompt;
	init.prompt_buffer_size = sizeof(s->prompt);
	init.history_buffer = s->history;
	init.history_buffer_size = sizeof(s->history);
	init.line_buffer = s->line;
	init.line_buffer_size = sizeof(s->line);
	init.host = "rk3568";
	init.user[0] = "root";
	init.user_data = s;
	if (chry_shell_init(&s->shell, &init) != 0) {
		s->in_use = 0;
		return NULL;
	}

	/* the negotiation opens the session: we echo, we suppress GA */
	{
		static const uint8_t will_echo_sga[] = {
			TN_IAC, TN_WILL, TN_OPT_ECHO,
			TN_IAC, TN_WILL, TN_OPT_SGA,
		};

		(void)send(sock, will_echo_sga, sizeof(will_echo_sga), 0);
	}

	attr.name = "telnet";
	attr.stack_size = TELNET_TASK_STACK;
	attr.priority = osPriorityNormal;
	s->task = osThreadNew(telnet_session_task, s, &attr);
	if (s->task == NULL) {
		s->in_use = 0;
		return NULL;
	}
	return s;
}

/* --- listener ---------------------------------------------------------------------- */

static void telnetd_task(void *arg)
{
	(void)arg;

	while (!listener_stop) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int conn = accept(listen_sock, (struct sockaddr *)&peer,
				  &plen);

		if (conn < 0) {
			if (!listener_stop) {
				(void)osDelay(100);
			}
			continue;
		}
		if (telnet_session_start(conn) == NULL) {
			lwip_close(conn);
		}
	}
}

static int cmd_telnetd(int argc, char *argv[])
{
	chry_shell_t *csh = CSH_FROM_ARGV(argc, argv);
	osThreadAttr_t attr = { 0 };
	struct sockaddr_in addr;
	osThreadId_t task;

	if (argc > 1 && strcmp(argv[1], "--stop") == 0) {
		if (listen_sock < 0) {
			csh_printf(csh, "telnetd: not running\n");
			return -1;
		}
		listener_stop = 1;
		lwip_close(listen_sock);	/* unblocks accept */
		listen_sock = -1;
		csh_printf(csh, "telnetd: listener stopped\n");
		return 0;
	}

	if (listen_sock >= 0) {
		csh_printf(csh, "telnetd: already running on port %d\n",
			   TELNET_PORT);
		return -1;
	}

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock < 0) {
		csh_printf(csh, "telnetd: socket failed\n");
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(TELNET_PORT);
	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listen_sock, TELNET_SESSION_MAX) != 0) {
		csh_printf(csh, "telnetd: bind/listen failed\n");
		lwip_close(listen_sock);
		listen_sock = -1;
		return -1;
	}

	listener_stop = 0;
	attr.name = "telnetd";
	attr.stack_size = 2048;
	attr.priority = osPriorityNormal;
	task = osThreadNew(telnetd_task, NULL, &attr);
	if (task == NULL) {
		lwip_close(listen_sock);
		listen_sock = -1;
		csh_printf(csh, "telnetd: task failed\n");
		return -1;
	}
	csh_printf(csh, "telnetd: listening on port %d\n", TELNET_PORT);
	return 0;
}
CSH_CMD_EXPORT_ALIAS_FULL(cmd_telnetd, telnetd, "telnetd [--stop]",
			  "telnet server: one shell instance per connection");
