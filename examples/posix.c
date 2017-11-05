#define _XOPEN_SOURCE 600

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "emrl.h"

#define DEFAULT_SOCKET_PATH "/tmp/emrl-socket"
#define PROMPT "emrl>"

enum mode
{
	mode_local,
	mode_pty,
	mode_socket
};

static inline void perror_exit(const char *info);
static inline void install_cleanup_handlers(void);
static inline int setup_pty(void);
static inline int setup_socket(void);
static inline void configure_terminal(int fd, enum mode mode);
static inline void emrl_loop(FILE *in_stream, FILE *out_stream);
static void cleanup(void);
static void signal_exit(int signum);

static volatile sig_atomic_t reset_stdin = 0;
static struct termios term_orig;

static volatile sig_atomic_t unlink_sock_path = 0;
static const char *sock_path = DEFAULT_SOCKET_PATH;

int main(int argc, char *argv[])
{
	int opt;
	bool usage = false;
	enum mode mode = mode_local;
	while(!usage && (opt = getopt(argc, argv, ":ps:")) != -1)
	{
		bool missing_arg = (opt == ':');
		if(missing_arg)
			opt = optopt;

		switch(opt)
		{
			default:
			case '?':
				usage = true;
				break;

			case 'p':
				mode = mode_pty;
				break;

			case 's':
				mode = mode_socket;
				if(!missing_arg)
					sock_path = optarg;
				break;
		}
	}

	if(usage || optind < argc)
	{
		char *prog_path = (argc > 0) ? (argv[0]) : ("posix");
		(void)fprintf(stderr, "usage: %s: [-p | -s [socket_path]]\n", prog_path);
		return EXIT_FAILURE;
	}
	
	install_cleanup_handlers();

	int fd;
	FILE *in_stream, *out_stream;
	if(mode_local == mode)
	{
		fd = STDIN_FILENO;
		in_stream = stdin;
		out_stream = stdout;
	}
	else
	{
		if(mode_pty == mode)
			fd = setup_pty();
		else
			fd = setup_socket();

		in_stream = fdopen(fd, "r");
		if(NULL == in_stream)
			perror_exit("fdopen in_stream");

		out_stream = fdopen(fd, "w");
		if(NULL == out_stream)
			perror_exit("fdopen out_stream");
	}

	if(mode_socket != mode)
		configure_terminal(fd, mode);

	emrl_loop(in_stream, out_stream);

	return EXIT_SUCCESS;
}

static inline void install_cleanup_handlers(void)
{
	// Handle the standard termination signals that don't cause a core dump
	struct sigaction action = {
		.sa_handler = signal_exit,
		.sa_flags = SA_RESETHAND
	};

	// Mask the signals we are handling to avoid repeated cleanups
	if(sigemptyset(&action.sa_mask))
		perror("sigfillset");

	if(sigaddset(&action.sa_mask, SIGINT) < 0)
		perror_exit("sigaddset SIGINT");

	if(sigaddset(&action.sa_mask, SIGTERM) < 0)
		perror_exit("sigaddset SIGTERM");

	if(sigaddset(&action.sa_mask, SIGHUP) < 0)
		perror_exit("sigaddset SIGHUP");


	if(sigaction(SIGINT, &action, NULL) < 0)
		perror_exit("sigaction SIGINT");

	if(sigaction(SIGTERM, &action, NULL) < 0)
		perror_exit("sigaction SIGTERM");

	if(sigaction(SIGHUP, &action, NULL) < 0)
		perror_exit("sigaction SIGHUP");


	// Register cleanup function to be called at exit
	if(atexit(cleanup) < 0)
		perror_exit("atexit");
}

static inline void perror_exit(const char *info)
{
	perror(info);
	exit(EXIT_FAILURE);
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
		perror_exit("slave open");

	printf("Connect to device '%s'\n", slave_path);

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
	// Race condition here, very unlikely though
	unlink_sock_path = 1;

	// Start listening for connections
	if(listen(sock_listen, 1) < 0)
		perror_exit("listen");
	
	printf("Connect to socket '%s'\n", sock_path);

	// Wait for an incoming connection
	int sock_data = accept(sock_listen, NULL, NULL);
	if(sock_data < 0)
		perror_exit("accept");
       
	return sock_data;
}

static inline void configure_terminal(int fd, enum mode mode)
{
	struct termios term_emrl;

	// Save the original terminal attributes
	if(tcgetattr(fd, &term_orig) < 0)
		perror_exit("tcgetattr orig");

	term_emrl = term_orig;

	term_emrl.c_iflag = 0;
	term_emrl.c_oflag = 0;
	term_emrl.c_cflag = CS8 | CREAD;

	// Ctrl-C generates SIGINT
	term_emrl.c_lflag = ISIG;

	// Make read block until one byte is available
	term_emrl.c_cc[VMIN] = 1;
	term_emrl.c_cc[VTIME] = 0;

	// Set a flag to ensure the local terminal settings get restored at exit if we are changing them
	if(mode_local == mode)
		reset_stdin = 1;

	if(tcsetattr(fd, TCSANOW, &term_emrl) < 0)
		perror_exit("tcsetattr emrl");
}

static inline void emrl_loop(FILE *in_stream, FILE *out_stream)
{
	// Initialise emrl, use fputs to write to out_stream, '\r' is line delimiter
	struct emrl_res emrl;
	emrl_init(&emrl, fputs, out_stream, "\r");

	// Write a prompt
	(void)fputs(PROMPT, out_stream);
	(void)fflush(out_stream);

	// Read characters until EOF or error
	int chr = fgetc(in_stream);
	while(EOF != chr)
	{
		// Feed character to emrl
		char *pCommand = emrl_process_char(&emrl, (char)chr);

		// If return value is non-null, emrl matched the delimiter and return the command text
		if(NULL != pCommand)
		{
			// Ignore the command if it is empty
			if('\0' != pCommand[0])
			{
				// Print the command text under the command line and add it to history
				(void)fprintf(out_stream, "\r\n>>>>>%s", pCommand);
				emrl_add_to_history(&emrl, pCommand);
			}

			// Write the prompt
			(void)fputs("\r\n" PROMPT, out_stream);
		}

		// Must flush since stdout is still line buffered
		(void)fflush(out_stream);
		chr = fgetc(in_stream);
	}

	// If fgetc set the error indicator, it was probably doing a read which means errno will set
	if(ferror(in_stream))
		perror_exit("fgetc (read fail?)");
}

static void cleanup(void)
{
	if(reset_stdin)
	{
		// Restore original terminal settings
		if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_orig) < 0)
			perror("tcsetattr orig");
	}

	if(unlink_sock_path)
	{
		// Remove socket path
		if(unlink(sock_path) < 0)
			perror("unlink");
	}
}

static void signal_exit(int signum)
{
	cleanup();

	// Use the GNU recommended method to exit - re-raise the signal with original handler
	// This works since SA_RESETHAND was used with the sigaction call
	if(raise(signum) != 0)
		perror("raise");
}
