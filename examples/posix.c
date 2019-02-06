#define _XOPEN_SOURCE 600

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "emrl.h"


#define DEFAULT_BAUD			1200.0
#define DEFAULT_SOCKET_PATH		"/tmp/emrl-socket"
#define PROMPT					"emrl>"
#define EOT						4


enum mode
{
	mode_local,
	mode_pty,
	mode_socket
};

struct setup
{
	enum mode mode;
	double baud;
};

struct ring
{
	char buf[32+EMRL_MAX_CMD_LEN];
	char *p_put;
	char *p_get;
	char *p_end;
};


static inline void parse_args(struct setup *p_setup, int argc, char *argv[]);
static inline void setup_termination_handlers(void);
static inline int setup_pty(void);
static inline int setup_socket(void);
static inline void configure_tty(int fd);
static inline void setup_baud_timer(sigset_t *p_sig, double baud);
static inline bool ring_empty(void);
static inline void ring_puts(const char *p_str);
static inline void write_from_ring(int fd);
static int emrl_puts(const char *p_str, FILE *p_file);
static inline bool feed_emrl(int fd, struct emrl_res *p_emrl);
static void cleanup(void);
static void perror_exit(const char *info);
static void signal_exit(int signum);


static volatile sig_atomic_t reset_stdin = 0;
static struct termios term_orig;

static volatile sig_atomic_t unlink_sock_path = 0;
static const char *sock_path = DEFAULT_SOCKET_PATH;

static struct ring ring =
{
	.buf = "",
	.p_put = ring.buf,
	.p_get = ring.buf,
	.p_end = ring.buf + sizeof ring.buf
};

int main(int argc, char *argv[])
{
	struct setup setup =
	{
		.mode = mode_local,
		.baud = DEFAULT_BAUD
	};

	parse_args(&setup, argc, argv);
	printf("\nEMbedded ReadLine test application\n\nSimulating %.0f baud\n\n", setup.baud);

	// Before configuring terminal, ensure cleanup() will be called on termination
	setup_termination_handlers();

	// Setup application input and output
	int in_fd;
	int out_fd;
	switch(setup.mode)
	{
		case mode_local:
			// Play nice with redirected io, useful for testing
			in_fd = STDIN_FILENO;
			out_fd = STDOUT_FILENO;
			if(isatty(in_fd))
				configure_tty(in_fd);

			break;
		
		case mode_pty:
			in_fd = out_fd = setup_pty();
			configure_tty(in_fd);
			break;

		case mode_socket:
			in_fd = out_fd = setup_socket();

			// Put the file descriptor into non-blocking mode
			if(0 != fcntl(in_fd, F_SETFL, O_NONBLOCK))
				perror_exit("fcntl(in_fd)");

			break;

		default:
			assert(false);
			return EXIT_FAILURE;
	}

	sigset_t signal;
	setup_baud_timer(&signal, setup.baud);

	// Initialise emrl, use emrl_fputs for output, '\r' is line delimiter
	struct emrl_res emrl;
	emrl_init(&emrl, emrl_puts, 0, "\r");

	// Write a prompt as soon as we start the loop
	ring_puts(PROMPT);

	bool eof = false;
	do
	{
		int signo;
		errno = sigwait(&signal, &signo);
		if(0 != errno)
			perror_exit("sigwait");

		assert(SIGRTMIN == signo);

		write_from_ring(out_fd);

		// Stop reading from terminal after EOF condition
		if(!eof)
			eof = feed_emrl(in_fd, &emrl);
	}
	while(!eof || !ring_empty());

	return EXIT_SUCCESS;
}

static inline void parse_args(struct setup *p_setup, int argc, char *argv[])
{
	int opt;
	bool usage = false;

	// Colon at the start of the opt string allows detection of missing option arguments
	while(!usage && (opt = getopt(argc, argv, ":b:ps:")) != -1)
	{
		// If argument is missing we get a colon for opt and option is in optopt
		bool missing_arg = (opt == ':');
		if(missing_arg)
			opt = optopt;

		switch(opt)
		{
			default:
			case '?':
				usage = true;
				break;

			case 'b':
				if(missing_arg)
				{
					usage = true;
				}
				else
				{
					p_setup->baud = strtod(optarg, &optarg);
					if('k' == *optarg || 'K' == *optarg)
					{
						p_setup->baud *= 1000.0;
						++optarg;
					}

					// Put some limits on the baud rate
					usage = ('\0' != *optarg || p_setup->baud < 0.01 || p_setup->baud > 1e6);
				}

				break;

			case 'p':
				p_setup->mode = mode_pty;
				break;

			case 's':
				p_setup->mode = mode_socket;
				if(!missing_arg)
					sock_path = optarg;
				break;
		}
	}

	if(usage || optind < argc)
	{
		const char *prog_path = (argc > 0) ? argv[0] : "posix";
		(void)fprintf(stderr, "usage: %s: [-p | -s [socket_path]]\n", prog_path);
		exit(EXIT_FAILURE);
	}
}

static inline void setup_termination_handlers(void)
{
	// Handle the standard termination signals that don't cause a core dump
	struct sigaction action = {
		.sa_handler = signal_exit,
		.sa_flags = SA_RESETHAND
	};

	// Mask the signals we are handling to avoid repeated cleanups
	if(sigemptyset(&action.sa_mask))
		perror_exit("sigemptyset");

	if(sigaddset(&action.sa_mask, SIGINT) < 0)
		perror_exit("sigaddset(SIGINT)");

	if(sigaddset(&action.sa_mask, SIGTERM) < 0)
		perror_exit("sigaddset(SIGTERM)");

	if(sigaddset(&action.sa_mask, SIGHUP) < 0)
		perror_exit("sigaddset(SIGHUP)");


	if(sigaction(SIGINT, &action, NULL) < 0)
		perror_exit("sigaction(SIGINT)");

	if(sigaction(SIGTERM, &action, NULL) < 0)
		perror_exit("sigaction(SIGTERM)");

	if(sigaction(SIGHUP, &action, NULL) < 0)
		perror_exit("sigaction(SIGHUP)");


	// Register cleanup function to be called at exit
	if(atexit(cleanup) < 0)
		perror_exit("atexit");
}

static inline int setup_pty(void)
{
	int fd = posix_openpt(O_RDWR|O_NOCTTY);
	if(fd < 0)
		perror_exit("posix_openpt");

	if(grantpt(fd) < 0)
		perror_exit("grantpt");

	if(unlockpt(fd) < 0)
		perror_exit("unlockpt");

	char *slave_path = ptsname(fd);
	if(NULL == slave_path)
		perror_exit("ptsname");

	// On Linux we get an EIO when the last process using the slave closes it, holding another file
	// descriptor for it prevents this and allows the device to be reused. Hopefully this won't
	// cause problems on other platforms.
	if(open(slave_path, O_RDWR|O_NOCTTY) < 0)
		perror_exit("open(slave_path)");

	printf("Connect to device '%s'\n\n", slave_path);

	return fd;
}

static inline int setup_socket(void)
{
	assert(NULL != sock_path);

	// Create a socket to listen on
	int sock_listen = socket(AF_UNIX, SOCK_STREAM, 0);
	if(sock_listen < 0) 
		perror_exit("socket");

	// Bind the socket to a filesystem path
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = {0}
	};

	size_t path_len = strlen(sock_path);
	if(path_len >= sizeof addr.sun_path)
	{
		fprintf(stderr,
		        "Socket bind path '%s' too long, must be %zu chars max\n",
		        sock_path,
		        sizeof addr.sun_path - 1);

		exit(EXIT_FAILURE);
	}

	memcpy(addr.sun_path, sock_path, path_len);
	if(bind(sock_listen, (struct sockaddr*)&addr, sizeof addr) < 0)
	{
		if(EADDRINUSE == errno)
		{
			fprintf(stderr, "Path '%s' already exists\n", sock_path);
			exit(EXIT_FAILURE);
		}
		else
		{
			perror_exit("bind");
		}
	}

	// Set a flag to remove the bound path at exit
	// TODO, Race condition here, very unlikely though
	unlink_sock_path = 1;

	// Start listening for connections
	if(listen(sock_listen, 1) < 0)
		perror_exit("listen");
	
	printf("Connect to socket '%s'\n\n", sock_path);

	// Wait for an incoming connection
	int sock_data = accept(sock_listen, NULL, NULL);
	if(sock_data < 0)
		perror_exit("accept");
       
	return sock_data;
}

static inline void configure_tty(int fd)
{
	// Save the original terminal attributes
	if(tcgetattr(fd, &term_orig) < 0)
		perror_exit("tcgetattr");

	struct termios term_emrl = term_orig;

	term_emrl.c_iflag = 0;
	term_emrl.c_oflag = 0;
	term_emrl.c_cflag = CS8 | CREAD;

	// Ctrl-C generates SIGINT
	term_emrl.c_lflag = ISIG;

	// Polling mode
	term_emrl.c_cc[VMIN] = 0;
	term_emrl.c_cc[VTIME] = 0;

	// Set a flag to ensure the local terminal settings get restored at exit if we are changing them
	if(STDIN_FILENO == fd)
		reset_stdin = 1;

	if(tcsetattr(fd, TCSAFLUSH, &term_emrl) < 0)
		perror_exit("tcsetattr(term_emrl)");
}

static inline void setup_baud_timer(sigset_t *p_sig, double baud)
{
	// Mask SIGRTMIN so that we can use sigwait on it
	if(sigemptyset(p_sig))
		perror_exit("sigemptyset");

	if(sigaddset(p_sig, SIGRTMIN) < 0)
		perror_exit("sigaddset SIGRTMIN");

	if(sigprocmask(SIG_SETMASK, p_sig, NULL) < 0)
		perror_exit("sigprocmask");

	
	// Create the timer
	struct sigevent event = {
		.sigev_notify = SIGEV_SIGNAL,
		.sigev_signo = SIGRTMIN
	};

	timer_t timer;
	if(timer_create(CLOCK_MONOTONIC, &event, &timer) < 0)
		perror_exit("timer_create");

	// Assume 10 bits per symbol (8N1)
	// Band range limited during parsing
	double sec;
	double sec_frac = modf(10.0 / baud, &sec);

	struct timespec tspec = {
		.tv_sec = (time_t)sec,
		.tv_nsec = lround(sec_frac * 1e9)
	};

	struct itimerspec ispec = {
		.it_interval = tspec,
		.it_value = tspec
	};

	if(timer_settime(timer, 0, &ispec, NULL) < 0)
		perror_exit("timer_settime");
}

static inline bool ring_empty(void)
{
	return (ring.p_get == ring.p_put);
}

static inline void ring_puts(const char *p_str)
{
	size_t len = strlen(p_str);

	size_t wrap = ring.p_end - ring.p_put;
	if(len >= wrap)
	{
		memcpy(ring.p_put, p_str, wrap);
		len -= wrap;
		p_str += wrap;
		ring.p_put = ring.buf;
	}

	memcpy(ring.p_put, p_str, len);
	ring.p_put += len;
}

static int emrl_puts(const char *p_str, FILE *p_file)
{
	(void)p_file;
	ring_puts(p_str);
	return 0;
}

static inline void write_from_ring(int fd)
{
	if(ring_empty())
		return;

	// Write will be non-blocking for a socket and blocking otherwise. This doesn't matter as
	// either way we don't read until all the data is written.
	//
	// For a socket we could get either EGAIN or EWOULDBLOCK if kernel buffer is full. As far as I
	// can see write shouldn't return zero (unlike read), but handle this case anyway.
	ssize_t res = write(fd, ring.p_get, 1);
	if(res < 0)
	{
		if(EAGAIN != errno && EWOULDBLOCK != errno)
			perror_exit("write");
	}
	else if(res > 0)
	{
		++ring.p_get;
		if(ring.p_get >= ring.p_end)
			ring.p_get = ring.buf;
	}
}

static inline bool feed_emrl(int fd, struct emrl_res *p_emrl)
{
	// Don't do anything if there is output backed up in the ring
	if(!ring_empty())
		return false;

	// If no data is available, read may give either EAGAIN or EWOULDBLOCK for sockets. For a
	// terminal it may give an EAGAIN error or return zero.
	char chr;
	ssize_t res = read(fd, &chr, 1);
	if(res < 0)
	{
		if(EAGAIN != errno && EWOULDBLOCK != errno)
			perror_exit("read");

		return false;
	}
	else if(0 == res)
	{
		return false;
	}

	// Allow Ctrl-D to quit when reading from stdin
	if(EOT == chr && STDIN_FILENO == fd)
		return true;

	char *p_command = emrl_process_char(p_emrl, chr);

	// Return early if there is no command to process
	if(NULL == p_command)
		return false;
	
	// Ignore the command if it is empty
	if('\0' != p_command[0])
	{
		// Print the command text under the command line and add it to history
		ring_puts("\r\n>>>>>");
		ring_puts(p_command);
		emrl_add_to_history(p_emrl, p_command);
	}

	// Write the prompt
	ring_puts("\r\n" PROMPT);

	return false;
}

static void cleanup(void)
{
	// From Linux atexit(3) man page:
	//
	// POSIX.1 says that the result of calling exit(3) more than once (i.e., calling exit(3) within
	// a function registered using atexit()) is undefined. On some systems (but not Linux), this
	// can result in an infinite recursion; portable programs should not invoke exit(3) inside a
	// function registered using atexit().
	//
	// So no perror_exit() here! Technicallly shouldn't use perror() either since it is not async
	// safe, but in most cases it will still produce useful output.

	if(reset_stdin)
	{
		// Restore original terminal settings
		if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_orig) < 0)
			perror("tcsetattr(term_orig)");
	}

	if(unlink_sock_path)
	{
		// Remove socket path
		if(unlink(sock_path) < 0)
			perror("unlink");
	}
}

static inline void perror_exit(const char *info)
{
	perror(info);
	exit(EXIT_FAILURE);
}

static void signal_exit(int signum)
{
	cleanup();

	// Use the GNU recommended method to exit - re-raise the signal with original handler
	// This works since SA_RESETHAND was used with the sigaction call
	if(raise(signum) != 0)
		perror("raise");
}
