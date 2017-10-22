#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "emrl.h"

#define PROMPT "emrl>"

#define USAGE		(1<<0)
#define PTY_MODE	(1<<1)

static inline void perror_exit(const char *info);
static inline int setup_pty(void);
static inline void configure_terminal(int fd, struct termios *p_term_orig);
static inline void emrl_loop(FILE *in_stream, FILE *out_stream);

int main(int argc, char *argv[])
{
	int opt;
	unsigned flags = 0;
	while(!(flags & USAGE) && (opt = getopt(argc, argv, "p")) != -1)
	{
		switch(opt)
		{
			default:
			case '?':
				flags |= USAGE;
				break;

			case 'p':
				flags |= PTY_MODE;
				break;
		}
	}

	if(flags & USAGE || optind < argc)
	{
		char *prog_name = argc > 0 ? argv[0] : "posix";
		(void)fprintf(stderr, "usage: %s: [-p]\n", prog_name);
		return EXIT_FAILURE;
	}

	int fd;
	FILE *in_stream, *out_stream;
	if(flags & PTY_MODE)
	{
		fd = setup_pty();

		in_stream = fdopen(fd, "r");
		if(NULL == in_stream)
			perror_exit("fdopen in_stream");

		out_stream = fdopen(fd, "w");
		if(NULL == out_stream)
			perror_exit("fdopen out_stream");
	}
	else
	{
		fd = STDIN_FILENO;
		in_stream = stdin;
		out_stream = stdout;
	}

	struct termios term_orig;
	configure_terminal(fd, &term_orig);

	emrl_loop(in_stream, out_stream);

	// Restore original terminal settings
	if(tcsetattr(fd, TCSAFLUSH, &term_orig) < 0)
		perror_exit("tcsetattr orig");

	return EXIT_SUCCESS;
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

	char *slave_name = ptsname(fd);
	if(NULL == slave_name)
		perror_exit("ptsname");

	printf("Connect terminal emulator to %s\n", slave_name);

	return fd;
}

static inline void configure_terminal(int fd, struct termios *p_term_orig)
{
	struct termios term_emrl;

	// Save the original terminal attributes
	if(tcgetattr(fd, p_term_orig) < 0)
		perror_exit("tcgetattr orig");

	term_emrl = *p_term_orig;

	term_emrl.c_iflag = 0;
	term_emrl.c_oflag = 0;
	term_emrl.c_cflag = CS8 | CREAD;

	// Ctrl-C generates SIGINT
	term_emrl.c_lflag = ISIG;

	// Make read block until one byte is available
	term_emrl.c_cc[VMIN] = 1;
	term_emrl.c_cc[VTIME] = 0;

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
	while(EOF != chr && EMRL_ASCII_EOT != chr)
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

	if(ferror(in_stream))
	{
		fputs("in stream error\n", stderr);
		exit(EXIT_FAILURE);
	}
}
