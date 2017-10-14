#ifndef EMRL_H
#define EMRL_H

#include "emrl_config.h"

#define EMRL_MAX_CMD_LEN 127

#define EMRL_ASCII_ETX 3
#define EMRL_ASCII_EOT 4
#define EMRL_ASCII_ESC 27
#define EMRL_ASCII_DEL 127

typedef int (*emrl_fputs_func)(const char *, emrl_file);

enum emrl_esc
{
	emrl_esc_none,
	emrl_esc_new,
	emrl_esc_ss3,
	emrl_esc_csi
};

// emrl resources
struct emrl_res
{
	emrl_fputs_func fputs;
	emrl_file file;
	const char *delim;
	const char *p_delim;
	char *p_esc;
	const char *p_esc_last;
	char *p_cursor;
	char *p_cmd_free;
	const char *p_cmd_last;
	enum emrl_esc esc_state;
	char esc_buf[6];
	char cmd_buf[EMRL_MAX_CMD_LEN + 1];
};

void emrl_init(struct emrl_res *p_this, emrl_fputs_func fputs, emrl_file file, const char *delim);
char *emrl_process_char(struct emrl_res *p_this, char chr);

#endif	/* EMRL_H */
