/*
 * emrl.h -- emrl line editing library header file
 *
 * Copyright (C) 2017 Graeme Hattan (graemeh.dev@gmail.com)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef EMRL_H
#define EMRL_H

#include "emrl_config.h"

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

struct emrl_history
{
	char *p_oldest;
	char *p_newest;
	char *p_current;
	char *p_put;
	char *p_cmd_free_bak;
	char *p_buf_last;
	char buf[EMRL_HISTORY_BUF_BYTES];
};

// emrl resources
struct emrl_res
{
	struct emrl_history history;
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
void emrl_add_to_history(struct emrl_res *p_this, const char *p_command);

#endif	/* EMRL_H */
