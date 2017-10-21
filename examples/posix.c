#include <termios.h>
#include <unistd.h>

#include "emrl.h"

#define PROMPT "emrl>"

int main(void)
{
	struct termios term_orig, term_emrl;
	struct emrl_res emrl;
	int chr;
	char *pCommand;

	// Save the original terminal attributes
	if(tcgetattr(STDIN_FILENO, &term_orig) < 0)
	{
		perror("tcgetattr orig");
		return 1;
	}

	term_emrl = term_orig;

	term_emrl.c_iflag = 0;
	term_emrl.c_oflag = 0;
	term_emrl.c_cflag = CS8 | CREAD;

	// Ctrl-C generates SIGINT
	term_emrl.c_lflag = ISIG;

	// Make read block until one byte is available
	term_emrl.c_cc[VMIN] = 1;
	term_emrl.c_cc[VTIME] = 0;

	if(tcsetattr(STDIN_FILENO, TCSANOW, &term_emrl) < 0)
	{
		perror("tcsetattr raw");
		return 1;
	}

	// Initialise emrl, use fputs to write to stdout, '\r' is line delimiter
	emrl_init(&emrl, fputs, stdout, "\r");

	// Write a prompt
	fputs(PROMPT, stdout);
	fflush(stdout);

	// Read characters until EOF or error
	while(read(STDIN_FILENO, &chr, 1) > 0 && EMRL_ASCII_EOT != chr)
	{
		// Feed character to emrl
		pCommand = emrl_process_char(&emrl, (char)chr);

		// If return value is non-null, emrl matched the delimiter and return the command text
		if(NULL != pCommand)
		{
			// Ignore the command if it is empty
			if('\0' != pCommand[0])
			{
				// Print the command text under the command line and add it to history
				printf("\r\n>>>>>%s", pCommand);
				emrl_add_to_history(&emrl, pCommand);
			}

			// Write the prompt
			fputs("\r\n" PROMPT, stdout);
		}

		// Must flush since stdout is still line buffered
		fflush(stdout);
	}

	// Restore original terminal settings
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_orig) < 0)
	{
		perror("tcsetattr orig");
		return 1;
	}

	return 0;
}
