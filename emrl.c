/*
 * emrl.c -- emrl line editing library functions
 *
 * Copyright (C) 2017 Graeme Hattan (graemeh.dev@gmail.com)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "emrl.h"

#define PRINT(str) (void)p_this->fputs(str, p_this->file)

#define SEQ_STEP_RIGHT "\033[C"
#define SEQ_STEP_LEFT "\b"
#define SEQ_DELETE_FORWARD "\033[P"
#define SEQ_DELETE_BACK "\b\033[P"
#define SEQ_INSERT_SPACE "\033[@"
#define SEQ_ERASE_TO_END "\033[K"

// TODO
// allow buffer setting
// check historic support for escape sequences above
// more history api -> help choose what to add
// completion
// option to build without snprintf
// static initialisation macro
// recognise more keys - insert, pgup, pgdown, home, end
// C++
// utf8 support?
// use BEL?
// attempt to handle print errors?


enum rp_type
{
	rp_insert,
	rp_erase
};

static inline void process_escape_state(struct emrl_res *p_this, char chr);
static inline void interpret_csi_escape(struct emrl_res *p_this);
static inline void erase_forward(struct emrl_res *p_this);
static inline void erase_back(struct emrl_res *p_this);
static inline void move_cursor_to_end(struct emrl_res *p_this);
static inline void add_string(struct emrl_res *p_this, char *p_str);
#if !defined(USE_INSERT_ESCAPE_SEQUENCE) || !defined(USE_DELETE_ESCAPE_SEQUENCE)
static inline void reprint_from_cursor(struct emrl_res *p_this, enum rp_type type, size_t back_mv);
#endif
static inline void reset_esc(struct emrl_res *p_this, bool known);
static inline char *hist_search_forward(struct emrl_res *p_this, char *p_entry);
static inline char *hist_search_backward(struct emrl_res *p_this, char *p_entry);
static inline void hist_show_prev(struct emrl_res *p_this);
static inline void hist_show_next(struct emrl_res *p_this);
static inline void hist_show_current(struct emrl_res *p_this);
static inline void clear_from_prompt(struct emrl_res *p_this);
static inline void deferred_history_copy(struct emrl_res *p_this);
static inline unsigned char_to_printable(unsigned char chr, char *p_print_str);

void emrl_init(struct emrl_res *p_this, emrl_fputs_func fputs, emrl_file file, const char *delim)
{
	p_this->fputs = fputs;
	p_this->file = file;
	p_this->delim = p_this->p_delim = delim;

	p_this->p_esc = p_this->esc_buf;
	p_this->p_esc_last = p_this->esc_buf + sizeof p_this->esc_buf - 1;

	p_this->p_cursor = p_this->p_cmd_free = p_this->cmd_buf;
	p_this->p_cmd_last = p_this->cmd_buf + sizeof p_this->cmd_buf - 1;
	p_this->esc_state = emrl_esc_none;

	struct emrl_history *ph = &p_this->history;
	ph->p_oldest = ph->p_newest = ph->p_current = NULL;
	ph->p_put = ph->buf + 1;
	ph->p_buf_last = ph->buf + sizeof ph->buf - 1;

	// Initialise first and last byte of history buffer to zero to delimit the buffer boundaries.
	// This enables faster searching using strchr (as opposed to a loop with bounds checks),
	// provided of course that no data is written to these bytes.
	ph->buf[0] = '\0';
	ph->buf[sizeof ph->buf - 1] = '\0';
	// Need a null delimiting start of unwritten oldest entry
	ph->buf[sizeof ph->buf - 2] = '\0';
}


char *emrl_process_char(struct emrl_res *p_this, char chr)
{
	if(emrl_esc_none != p_this->esc_state)
	{
		process_escape_state(p_this, chr);
		return NULL;
	}

	if(chr == *p_this->p_delim)
	{
		++p_this->p_delim;
		if('\0' == *p_this->p_delim)
		{
			deferred_history_copy(p_this);
			move_cursor_to_end(p_this);

			*p_this->p_cmd_free = '\0';
			p_this->p_delim = p_this->delim;
			p_this->p_cursor = p_this->p_cmd_free = p_this->cmd_buf;

			return p_this->cmd_buf;
		}
	}
	else
	{
		p_this->p_delim = p_this->delim;
	}

	char str_buf[5];
	switch(chr)
	{
		case '\b':
			erase_back(p_this);
			break;

		case '\r':
		case '\n':
			// Ignore (unless in delim string)
			break;

		case EMRL_ASCII_ESC:
			p_this->esc_state = emrl_esc_new;
			break;

		case EMRL_ASCII_DEL:
			erase_back(p_this);
			break;

		default:
			char_to_printable(chr, str_buf);
			add_string(p_this, str_buf);
			break;
	}

	return NULL;
}


void emrl_add_to_history(struct emrl_res *p_this, char *p_command)
{
	struct emrl_history *ph = &p_this->history;
	size_t cmd_len = strlen(p_command) + 1;

	if(cmd_len > (sizeof ph->buf - 2))
		return;

	ph->p_newest = ph->p_put;

	// Will we overwrite the oldest member?
	bool overwrite;
	if(NULL == ph->p_oldest)
	{
		// No oldest member to overwrite, remember to initialise
		ph->p_oldest = ph->p_newest;
		overwrite = false;
	}
	else
	{
		// Calculate number of bytes to before we pass p_oldest
		ptrdiff_t len_to_ovr = ph->p_oldest - ph->p_put;
		if(len_to_ovr < 0)
			len_to_ovr += (sizeof ph->buf - 2);

		overwrite = (cmd_len > (size_t)len_to_ovr);
	}

	// Will we pass the end of the buffer?
	size_t len_to_wrap = ph->p_buf_last - ph->p_put;
	if(cmd_len <= len_to_wrap)
	{
		// No, one copy needed
		(void)memcpy(ph->p_put, p_command, cmd_len);
		ph->p_put += cmd_len;

		// Pre-wrap p_put since it is used to update p_newest
		if(ph->p_put == ph->p_buf_last)
			ph->p_put = ph->buf + 1;
	}
	else
	{
		// Yes, two copies needed
		(void)memcpy(ph->p_put, p_command, len_to_wrap);
		cmd_len -= len_to_wrap;
		(void)memcpy(ph->buf+1, p_command+len_to_wrap, cmd_len);
		ph->p_put = ph->buf + 1 + cmd_len;
	}

	if(overwrite)
		ph->p_oldest = hist_search_forward(p_this, ph->p_put);
}


static inline void process_escape_state(struct emrl_res *p_this, char chr)
{
	// Overflow check not needed in emrl_esc_new (always first character)
	// or emrl_esc_ss3 (always second character)
	*p_this->p_esc++ = chr;

	if(emrl_esc_new == p_this->esc_state)
	{
		switch(chr)
		{
			case '[':
				p_this->esc_state = emrl_esc_csi;
				break;

			case 'O':
				p_this->esc_state = emrl_esc_ss3;
				break;

			default:
				reset_esc(p_this, false);
				break;
		}
	}
	else if(emrl_esc_ss3 == p_this->esc_state)
	{
		reset_esc(p_this, false);
	}
	else if(emrl_esc_csi == p_this->esc_state)
	{
		// Check for a 'final byte'
		if(chr >= 0x40 && chr <= 0x7e)
		{
			interpret_csi_escape(p_this);
		}
		else if(p_this->p_esc == p_this->p_esc_last)
		{
			reset_esc(p_this, false);
		}
	}
}

static inline void interpret_csi_escape(struct emrl_res *p_this)
{
	bool known = true;
	size_t len = p_this->p_esc - p_this->esc_buf;
	if(2 == len)
	{
		switch(p_this->esc_buf[1])
		{
			case 'A':
				// Up
				hist_show_prev(p_this);
				break;

			case 'B':
				// Down
				hist_show_next(p_this);
				break;

			case 'C':
				// Right
				if(p_this->p_cursor != p_this->p_cmd_free)
				{
					++p_this->p_cursor;
					PRINT(SEQ_STEP_RIGHT);
				}
				break;

			case 'D':
				// Left
				if(p_this->p_cursor != p_this->cmd_buf)
				{
					--p_this->p_cursor;
					PRINT(SEQ_STEP_LEFT);
				}
				break;

			default:
				known = false;
				break;
		}

	}
	else if(3 == len)
	{
		if(0 == memcmp(p_this->esc_buf+1, "3~", 2))
			erase_forward(p_this);
		else
			known = false;
	}
	else
	{
		known = false;
	}

	reset_esc(p_this, known);
}

static inline void erase_forward(struct emrl_res *p_this)
{
	// Are we at the end if the line? If so, nothing to erase
	if(p_this->p_cursor != p_this->p_cmd_free)
	{
		// No - remove character under cursor
		deferred_history_copy(p_this);
		size_t len = p_this->p_cmd_free - p_this->p_cursor;
		(void)memmove(p_this->p_cursor, p_this->p_cursor+1, len);
		--p_this->p_cmd_free;
#ifdef USE_DELETE_ESCAPE_SEQUENCE
		PRINT(SEQ_DELETE_FORWARD);
#else
		reprint_from_cursor(p_this, rp_erase, len);
#endif
	}
}

static inline void move_cursor_to_end(struct emrl_res *p_this)
{
	ptrdiff_t to_end_len = p_this->p_cmd_free - p_this->p_cursor;
	if(to_end_len > 0)
	{
		char out_buf[16];
		(void)snprintf(out_buf, sizeof out_buf, "\033[%tdC", to_end_len);
		PRINT(out_buf);
	}
}

static inline void erase_back(struct emrl_res *p_this)
{
	// Are we at the start of the line? Don't erase the prompt!
	if(p_this->p_cursor != p_this->cmd_buf)
	{
		deferred_history_copy(p_this);

		// Are we at the end of the line?
		if(p_this->p_cursor == p_this->p_cmd_free)
		{
			// Yes - simple erase sequence
			--p_this->p_cursor;
			--p_this->p_cmd_free;
			PRINT(SEQ_DELETE_BACK);
		}
		else
		{
			// No - remove character before cursor and reprint
			size_t len = p_this->p_cmd_free - p_this->p_cursor;
			(void)memmove(p_this->p_cursor-1, p_this->p_cursor, len);
			--p_this->p_cursor;
			--p_this->p_cmd_free;

#ifdef USE_DELETE_ESCAPE_SEQUENCE
			PRINT(SEQ_DELETE_BACK);
#else
			PRINT(SEQ_STEP_LEFT);
			reprint_from_cursor(p_this, rp_erase, len+1);
#endif
		}
		
	}
}

static inline void add_string(struct emrl_res *p_this, char *p_str)
{
	size_t add_len = strlen(p_str);

	// Enough space in the command buffer?
	if((p_this->p_cmd_last - p_this->p_cmd_free) > (ptrdiff_t)add_len)
	{
		deferred_history_copy(p_this);

		// Are we at the end of the line?
		if(p_this->p_cursor == p_this->p_cmd_free)
		{
			// Yes - simple append
			(void)memcpy(p_this->p_cmd_free, p_str, add_len);
			p_this->p_cmd_free += add_len;
			PRINT(p_str);
		}
		else
		{
			// No - insert
			size_t to_end_len = p_this->p_cmd_free - p_this->p_cursor;
			(void)memmove(p_this->p_cursor+add_len, p_this->p_cursor, to_end_len);
			(void)memcpy(p_this->p_cursor, p_str, add_len);
			p_this->p_cmd_free += add_len;
#ifdef USE_INSERT_ESCAPE_SEQUENCE
			char buf[16];
			if(1 == add_len)
			{
				// Usually we will just insert characters as the user types
				// Optimise this a little and save on print calls, which may be cumbersome
				(void)memcpy(buf, SEQ_INSERT_SPACE, sizeof SEQ_INSERT_SPACE - 1);
				buf[sizeof SEQ_INSERT_SPACE - 1] = *p_str;
				buf[sizeof SEQ_INSERT_SPACE] = '\0';
				PRINT(buf);
			}
			else
			{
				// Only actually happens if we are printing unknown keys or escape sequences
				(void)snprintf(buf, sizeof buf, "\033[%zu@", add_len);
				PRINT(buf);
				PRINT(p_str);
			}

#else
			reprint_from_cursor(p_this, rp_insert, to_end_len);
#endif
		}

		p_this->p_cursor += add_len;	// Update internal cursor
	}
}

#if !defined(USE_INSERT_ESCAPE_SEQUENCE) || !defined(USE_DELETE_ESCAPE_SEQUENCE)
static inline void reprint_from_cursor(struct emrl_res *p_this, enum rp_type type, size_t back_mv)
{
	char out_buf[16];

	switch(type)
	{
		default:
		case rp_insert:
			(void)snprintf(out_buf, sizeof out_buf, "\033[%zuD", back_mv);
			break;

		case rp_erase:
			// Prepend a space to the move back to cover erased char,
			(void)snprintf(out_buf, sizeof out_buf, " \033[%zuD", back_mv);
			break;
	}

	// Ensure buffer is null terminated before printing
	*p_this->p_cmd_free = '\0';
	PRINT(p_this->p_cursor);
	PRINT(out_buf);
}
#endif

static inline void reset_esc(struct emrl_res *p_this, bool known)
{
	if(!known)
	{
		// Space for "^[" (ESC in caret notation),
		// plus 4 bytes for each  member of esc_buf ("M-[x" possible for each)
		// plus null terminator
		char str_buf[4 * (sizeof p_this->esc_buf) + 3];
		char *p_str = str_buf + 2;
		char *p_esc_tmp = p_this->esc_buf;

		// ESC character not in escape buffer, add manually
		str_buf[0] = '^';
		str_buf[1] = '[';

		do
		{
			p_str += char_to_printable(*p_esc_tmp++, p_str);
		}
		while(p_esc_tmp < p_this->p_esc);

		add_string(p_this, str_buf);
	}

	p_this->p_esc = p_this->esc_buf;
	p_this->esc_state = emrl_esc_none;
}

static inline char *hist_search_forward(struct emrl_res *p_this, char *p_entry)
{
	assert(p_entry > p_this->history.buf);
	assert(p_entry <= p_this->history.p_buf_last);		// May be equal when updating p_oldest

	p_entry = strchr(p_entry, '\0');

	// Hit end of buffer? Keep searching from start
	if(p_entry == p_this->history.p_buf_last)
		p_entry = strchr(p_this->history.buf+1, '\0');

	++p_entry;											// Skip past string terminator

	// Did previous entry go right to the end of the buffer, wrap if so
	if(p_entry == p_this->history.p_buf_last)
		p_entry = p_this->history.buf + 1;

	return p_entry;
}

static inline char *hist_search_backward(struct emrl_res *p_this, char *p_entry)
{
	assert(p_entry > p_this->history.buf);
	assert(p_entry < p_this->history.p_buf_last);

	// Is the entry at the start of the buffer?
	if(p_entry-1 == p_this->history.buf)
		p_entry = p_this->history.p_buf_last - 2;	// Previous null at end, start before it
	else
		p_entry -= 2;								// Go back past null of previous entry

	while('\0' != *p_entry)
		--p_entry;

	// Hit start of buffer? Keep searching from end
	if(p_entry == p_this->history.buf)
	{
		p_entry = p_this->history.p_buf_last - 1;
		while('\0' != *p_entry)
			--p_entry;
	}

	// Entry starts after the null
	if(++p_entry == p_this->history.p_buf_last)
		p_entry = p_this->history.buf + 1;
		
	return p_entry;
}

static inline void hist_show_prev(struct emrl_res *p_this)
{
	struct emrl_history *ph = &p_this->history;

	if(NULL != ph->p_newest && ph->p_current != ph->p_oldest)
	{
		if(NULL == ph->p_current)
		{
			ph->p_current = ph->p_newest;
			ph->p_cmd_free_bak = p_this->p_cmd_free;
		}
		else
		{
			ph->p_current = hist_search_backward(p_this, ph->p_current);
		}

		hist_show_current(p_this);
	}
}

static inline void hist_show_next(struct emrl_res *p_this)
{
	struct emrl_history *ph = &p_this->history;

	// Is history search active?
	if(NULL != ph->p_current)
	{
		// Are we already at the newest entry?
		if(ph->p_current == ph->p_newest)
		{
			// Yes, drop out of history search and display original command
			ph->p_current = NULL;

			clear_from_prompt(p_this);
			p_this->p_cursor = p_this->p_cmd_free = ph->p_cmd_free_bak;

			// Print command text, if there is any to print
			if(p_this->p_cmd_free > p_this->cmd_buf)
			{
				*p_this->p_cmd_free = '\0';
				PRINT(p_this->cmd_buf);
			}
		}
		else
		{
			// No, show next newest entry
			ph->p_current = hist_search_forward(p_this, ph->p_current);
			hist_show_current(p_this);
		}
	}
}

static inline void hist_show_current(struct emrl_res *p_this)
{
	struct emrl_history *ph = &p_this->history;

	assert(NULL != ph->p_current);

	clear_from_prompt(p_this);
	PRINT(ph->p_current);
	size_t len = strlen(ph->p_current);
	if(ph->p_current + len == ph->p_buf_last)
	{
		PRINT(ph->buf + 1);
		len += strlen(ph->buf + 1);
	}

	// Set p_cmd_free so that arrow movement behaves like cmd_buf contains the history entry,
	// but don't overwrite anything until the user edits or presses return
	p_this->p_cursor = p_this->p_cmd_free = p_this->cmd_buf + len;
}

static inline void clear_from_prompt(struct emrl_res *p_this)
{
	ptrdiff_t back_mv = p_this->p_cursor-p_this->cmd_buf;
	if(back_mv > 0)
	{
		char out_buf[16];
		(void)snprintf(out_buf, sizeof out_buf, "\033[%tdD" SEQ_ERASE_TO_END, back_mv);
		PRINT(out_buf);
	}
}

// When searching through the history, we just print the entry without copying it to the buffer.
// If the user started typing something before searching history, this allows them to return to it.
// However, the data needs to be copied over before editing or returning the history entry. This
// function checks if there is data to copy, and if so, copies it.
static inline void deferred_history_copy(struct emrl_res *p_this)
{
	struct emrl_history *ph = &p_this->history;

	// History search active?
	if(NULL != ph->p_current)
	{
		// Yes, copy current history entry to the command buffer
		size_t len = strlen(ph->p_current);
		(void)memcpy(p_this->cmd_buf, ph->p_current, len);
		if(ph->p_current + len == ph->p_buf_last)
			strcpy(p_this->cmd_buf + len, ph->buf + 1);

		// p_cmd_free should already point to the end of the command

		// Exit history search
		ph->p_current = NULL;
	}
}

static inline unsigned char_to_printable(unsigned char chr, char *p_print_str)
{
	unsigned len;

	// Use caret notation, with M- for the non-ascii range
	if(isprint(chr))
	{
		*p_print_str++ = chr;
		len = 1;
	}
	else if(chr < 128)
	{
		*p_print_str++ = '^';
		*p_print_str++ = (EMRL_ASCII_DEL == chr) ? ('?') : ('@' + chr);
		len = 2;
	}
	else
	{
		*p_print_str++ = 'M';
		*p_print_str++ = '-';
		return 2 + char_to_printable(chr-128, p_print_str);
	}
	
	*p_print_str = '\0';

	return len;
}
