#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "emrl.h"

#define PRINT(str) (void)p_this->fputs(str, p_this->file)

/*#define SEQ_HIDE_CURSOR	"\033?25l"*/
/*#define SEQ_SHOW_CURSOR	"\033?25h"*/
#define SEQ_HIDE_CURSOR	""
#define SEQ_SHOW_CURSOR	""
#define SEQ_STEP_RIGHT "\033[C"
#define SEQ_STEP_LEFT "\033[D"
#define SEQ_ERASE_BACK "\b \b"

enum rp_type
{
	rp_insert,
	rp_erase
};

static inline void process_escape_state(struct emrl_res *p_this, char chr);
static inline void interpret_csi_escape(struct emrl_res *p_this);
static inline void erase_forward(struct emrl_res *p_this);
static inline void erase_back(struct emrl_res *p_this);
static inline void add_string(struct emrl_res *p_this, char *p_str);
static inline void reprint_from_cursor(struct emrl_res *p_this, enum rp_type type, size_t back_mv);
static inline void reset_esc(struct emrl_res *p_this, bool known);
static inline unsigned char_to_printable(unsigned char chr, char *p_print_str);

// history
// complete

void emrl_init(struct emrl_res *p_this, emrl_fputs_func fputs, emrl_file file, const char *delim)
{
	p_this->fputs = fputs;
	p_this->file = file;
	p_this->delim = p_this->p_delim = delim;

	// Set up to use caret notation when echoing unknown escape sequences
	p_this->p_esc = p_this->esc_buf;
	p_this->p_esc_last = p_this->esc_buf + sizeof p_this->esc_buf - 1;

	p_this->p_cursor = p_this->p_cmd_free = p_this->cmd_buf;
	p_this->p_cmd_last = p_this->cmd_buf + sizeof p_this->cmd_buf - 1;
	p_this->esc_state = emrl_esc_none;
}


char *emrl_process_char(struct emrl_res *p_this, char chr)
{
	char str_buf[5];

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

	switch(chr)
	{
		case EMRL_ASCII_DEL:
			erase_back(p_this);
			break;

		case EMRL_ASCII_ESC:
			p_this->esc_state = emrl_esc_new;
			break;

		default:
			char_to_printable(chr, str_buf);
			add_string(p_this, str_buf);
			break;
	}

	return NULL;
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
				break;

			case 'B':
				// Down
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
		if(0 == strncmp(p_this->esc_buf+1, "3~", 2))
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
		// No - remove character under cursor and reprint
		size_t len = p_this->p_cmd_free - p_this->p_cursor;
		memmove(p_this->p_cursor, p_this->p_cursor+1, len);
		--p_this->p_cmd_free;
		reprint_from_cursor(p_this, rp_erase, len);
	}
}

static inline void erase_back(struct emrl_res *p_this)
{
	// Are we at the start of the line? Don't erase the prompt!
	if(p_this->p_cursor != p_this->cmd_buf)
	{
		// Are we at the end of the line?
		if(p_this->p_cursor == p_this->p_cmd_free)
		{
			// Yes - simple erase sequence
			--p_this->p_cursor;
			--p_this->p_cmd_free;
			PRINT(SEQ_ERASE_BACK);
		}
		else
		{
			// No - remove character before cursor and reprint
			size_t len = p_this->p_cmd_free - p_this->p_cursor;
			memmove(p_this->p_cursor-1, p_this->p_cursor, len);
			--p_this->p_cursor;
			--p_this->p_cmd_free;

			PRINT("\b");
			reprint_from_cursor(p_this, rp_erase, len+1);
		}
		
	}
}

static inline void add_string(struct emrl_res *p_this, char *p_str)
{
	size_t add_len = strlen(p_str);

	// Enough space in the command buffer?
	if((p_this->p_cmd_last - p_this->p_cmd_free) > (ptrdiff_t)add_len)
	{
		// Are we at the end of the line?
		if(p_this->p_cursor == p_this->p_cmd_free)
		{
			// Yes - simple append
			memcpy(p_this->p_cmd_free, p_str, add_len);
			p_this->p_cmd_free += add_len;
			PRINT(p_str);
		}
		else
		{
			// No - insert and reprint from cursor
			size_t to_end_len = p_this->p_cmd_free - p_this->p_cursor;
			memmove(p_this->p_cursor+add_len, p_this->p_cursor, to_end_len);
			memcpy(p_this->p_cursor, p_str, add_len);
			p_this->p_cmd_free += add_len;
			reprint_from_cursor(p_this, rp_insert, to_end_len);
		}

		p_this->p_cursor += add_len;	// Update internal cursor
	}
}

static inline void reprint_from_cursor(struct emrl_res *p_this, enum rp_type type, size_t back_mv)
{
	char out_buf[16];

	switch(type)
	{
		default:
		case rp_insert:
#ifdef EMRL_HIDE_CURSOR_DURING_REPRINT
			(void)snprintf(out_buf, sizeof out_buf, "\033[%zuD" SEQ_SHOW_CURSOR, back_mv);
#else
			(void)snprintf(out_buf, sizeof out_buf, "\033[%zuD", back_mv);
#endif
			break;

		case rp_erase:
			// Prepend a space to the move back to cover erased char,
#ifdef EMRL_HIDE_CURSOR_DURING_REPRINT
			(void)snprintf(out_buf, sizeof out_buf, " \033[%zuD" SEQ_SHOW_CURSOR, back_mv);
#else
			(void)snprintf(out_buf, sizeof out_buf, " \033[%zuD", back_mv);
#endif
			break;
	}

	// Ensure buffer is null terminated before printing
	*p_this->p_cmd_free = '\0';
#ifdef EMRL_HIDE_CURSOR_DURING_REPRINT
	PRINT(SEQ_HIDE_CURSOR);
#endif
	PRINT(p_this->p_cursor);
	PRINT(out_buf);
}

static inline void reset_esc(struct emrl_res *p_this, bool known)
{
	if(!known)
	{
		// Space for "^[" (ESC in caret notation),
		// plus up to 4 bytes for each  member of esc_buf ("M-[x" possible for each)
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

static inline unsigned char_to_printable(unsigned char chr, char *p_print_str)
{
	unsigned len;

	if(isprint(chr))
	{
		*p_print_str++ = chr;
		len = 1;
	}
	else
	{
		// Use caret notation, with M- for the non-ascii range
		if(chr >= 128)
		{
			*p_print_str++ = 'M';
			*p_print_str++ = '-';
			chr -= 128;
			len = 4;
		}
		else
		{
			len = 2;
		}

		*p_print_str++ = '^';
		*p_print_str++ = (EMRL_ASCII_DEL == chr) ? ('?') : ('@' + chr);
	}
	
	*p_print_str = '\0';

	return len;
}
