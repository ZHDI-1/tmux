/* $OpenBSD: input.c,v 1.281 2026/10/26 10:23:50 nicm Exp $ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <ctype.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"
#include <event2/buffer.h>

/*
 * Based on the description by Paul Williams at:
 *
 * https://vt100.net/emu/dec_ansi_parser
 *
 * With the following changes:
 *
 * - 7-bit only.
 *
 * - Support for UTF-8.
 *
 * - OSC (but not APC) may be terminated by \007 as well as ST.
 *
 * - A state for APC similar to OSC. Some terminals appear to use this to set
 *   the title.
 *
 * - A state for the screen \033k...\033\\ sequence to rename a window. This is
 *   pretty stupid but not supporting it is more trouble than it is worth.
 *
 * - Special handling for ESC inside a DCS to allow arbitrary byte sequences to
 *   be passed to the underlying terminals.
 */

/* Type of terminator. */
enum input_end_type {
	INPUT_END_ST,
	INPUT_END_BEL
};

/* Request sent by a pane. */
struct input_request {
	struct client			*c;
	struct input_ctx		*ictx;

	enum input_request_type		 type;
	uint64_t			 t;
	enum input_end_type		 end;

	int				 idx;
	void				*data;

	TAILQ_ENTRY(input_request)	 entry;
	TAILQ_ENTRY(input_request)	 centry;
};
#define INPUT_REQUEST_TIMEOUT 500

/* Input parser cell. */
struct input_cell {
	struct grid_cell	cell;
	int			set;
	int			g0set;	/* 1 if ACS */
	int			g1set;	/* 1 if ACS */
};

/* Input parser argument. */
struct input_param {
	enum {
		INPUT_MISSING,
		INPUT_NUMBER,
		INPUT_STRING
	}			type;
	union {
		int		num;
		char	       *str;
	};
};

/* Input parser context. */
struct input_ctx {
	struct window_pane	       *wp;
	struct screen_write_ctx		ctx;
	struct colour_palette	       *palette;

	struct input_cell		cell;
	struct input_cell		old_cell;
	u_int				old_cx;
	u_int				old_cy;
	int				old_mode;

	u_char				interm_buf[4];
	size_t				interm_len;

	u_char				param_buf[64];
	size_t				param_len;

#define INPUT_BUF_START 32
	u_char			       *input_buf;
	size_t				input_len;
	size_t				input_space;
	enum input_end_type		input_end;

	struct input_param		param_list[24];
	u_int				param_list_len;

	struct utf8_data		utf8data;
	int				utf8started;

	int				ch;
	struct utf8_data		last;

	const struct input_state       *state;
	int				flags;
#define INPUT_DISCARD 0x1
#define INPUT_LAST 0x2

	struct input_requests		 requests;
	u_int				 request_count;
	struct event			 request_timer;

	/*
	 * All input received since we were last in the ground state. Sent to
	 * control clients on connection.
	 */
	struct evbuffer			*since_ground;
	int				 since_ground_pending;
	struct event			 ground_timer;
	int				 ground_timer_armed;
#ifdef ENABLE_IMAGES
	void				*kitty_state;
#endif
};

/* Helper functions. */
struct input_transition;
static void	input_request_timer_callback(int, short, void *);
static void	input_start_request_timer(struct input_ctx *);
static struct input_request *input_make_request(struct input_ctx *,
		    enum input_request_type);
static void	input_free_request(struct input_request *);
static int	input_add_request(struct input_ctx *, enum input_request_type,
		    int);
static int	input_split(struct input_ctx *);
static int	input_get(struct input_ctx *, u_int, int, int);
static void	input_set_state(struct input_ctx *,
		    const struct input_transition *);
static void	input_reset_cell(struct input_ctx *);
static void	input_report_current_theme(struct input_ctx *);
static void	input_osc_4(struct input_ctx *, const char *);
static void	input_osc_8(struct input_ctx *, const char *);
static void	input_osc_9(struct input_ctx *, const char *);
static void	input_osc_10(struct input_ctx *, const char *);
static void	input_osc_11(struct input_ctx *, const char *);
static void	input_osc_12(struct input_ctx *, const char *);
static void	input_osc_52(struct input_ctx *, const char *);
static void	input_osc_104(struct input_ctx *, const char *);
static void	input_osc_110(struct input_ctx *, const char *);
static void	input_osc_111(struct input_ctx *, const char *);
static void	input_osc_112(struct input_ctx *, const char *);
static void	input_osc_133(struct input_ctx *, const char *);

/* Transition entry/exit handlers. */
static void	input_clear(struct input_ctx *);
static void	input_ground(struct input_ctx *);
static void	input_enter_dcs(struct input_ctx *);
static void	input_enter_osc(struct input_ctx *);
static void	input_exit_osc(struct input_ctx *);
static void	input_enter_apc(struct input_ctx *);
static void	input_exit_apc(struct input_ctx *);
static void	input_enter_rename(struct input_ctx *);
static void	input_exit_rename(struct input_ctx *);

/* Input state handlers. */
static int	input_print(struct input_ctx *);
static int	input_intermediate(struct input_ctx *);
static int	input_parameter(struct input_ctx *);
static int	input_input(struct input_ctx *);
static int	input_c0_dispatch(struct input_ctx *);
static int	input_esc_dispatch(struct input_ctx *);
static int	input_csi_dispatch(struct input_ctx *);
static void	input_csi_dispatch_rm(struct input_ctx *);
static void	input_csi_dispatch_rm_private(struct input_ctx *);
static void	input_csi_dispatch_sm(struct input_ctx *);
static void	input_csi_dispatch_sm_private(struct input_ctx *);
static void	input_csi_dispatch_sm_graphics(struct input_ctx *);
static void	input_csi_dispatch_winops(struct input_ctx *);
static void	input_csi_dispatch_sgr_256(struct input_ctx *, int, u_int *);
static void	input_csi_dispatch_sgr_rgb(struct input_ctx *, int, u_int *);
static void	input_csi_dispatch_sgr(struct input_ctx *);
static void	input_csi_dispatch_sgr_numbers(struct input_ctx *, const int *, u_int);
static void	input_csi_dispatch_cup_args(struct input_ctx *, int, int);
static void	input_csi_dispatch_cup(struct input_ctx *);
static int	input_dcs_dispatch(struct input_ctx *);
static int	input_top_bit_set(struct input_ctx *);
static int	input_end_bel(struct input_ctx *);

/* Command table comparison function. */
static int	input_table_compare(const void *, const void *);

/* Command table entry. */
struct input_table_entry {
	int		ch;
	const char     *interm;
	int		type;
};

/* Escape commands. */
enum input_esc_type {
	INPUT_ESC_DECALN,
	INPUT_ESC_DECKPAM,
	INPUT_ESC_DECKPNM,
	INPUT_ESC_DECRC,
	INPUT_ESC_DECSC,
	INPUT_ESC_HTS,
	INPUT_ESC_IND,
	INPUT_ESC_NEL,
	INPUT_ESC_RI,
	INPUT_ESC_RIS,
	INPUT_ESC_SCSG0_OFF,
	INPUT_ESC_SCSG0_ON,
	INPUT_ESC_SCSG1_OFF,
	INPUT_ESC_SCSG1_ON,
	INPUT_ESC_ST
};

/* Escape command table. */
static const struct input_table_entry input_esc_table[] = {
	{ '0', "(", INPUT_ESC_SCSG0_ON },
	{ '0', ")", INPUT_ESC_SCSG1_ON },
	{ '7', "",  INPUT_ESC_DECSC },
	{ '8', "",  INPUT_ESC_DECRC },
	{ '8', "#", INPUT_ESC_DECALN },
	{ '=', "",  INPUT_ESC_DECKPAM },
	{ '>', "",  INPUT_ESC_DECKPNM },
	{ 'B', "(", INPUT_ESC_SCSG0_OFF },
	{ 'B', ")", INPUT_ESC_SCSG1_OFF },
	{ 'D', "",  INPUT_ESC_IND },
	{ 'E', "",  INPUT_ESC_NEL },
	{ 'H', "",  INPUT_ESC_HTS },
	{ 'M', "",  INPUT_ESC_RI },
	{ '\\', "", INPUT_ESC_ST },
	{ 'c', "",  INPUT_ESC_RIS },
};

/* Control (CSI) commands. */
enum input_csi_type {
	INPUT_CSI_CBT,
	INPUT_CSI_CNL,
	INPUT_CSI_CPL,
	INPUT_CSI_CUB,
	INPUT_CSI_CUD,
	INPUT_CSI_CUF,
	INPUT_CSI_CUP,
	INPUT_CSI_CUU,
	INPUT_CSI_DA,
	INPUT_CSI_DA_TWO,
	INPUT_CSI_DCH,
	INPUT_CSI_DECSCUSR,
	INPUT_CSI_DECSTBM,
	INPUT_CSI_DL,
	INPUT_CSI_DSR,
	INPUT_CSI_DSR_PRIVATE,
	INPUT_CSI_ECH,
	INPUT_CSI_ED,
	INPUT_CSI_EL,
	INPUT_CSI_HPA,
	INPUT_CSI_ICH,
	INPUT_CSI_IL,
	INPUT_CSI_MODOFF,
	INPUT_CSI_MODSET,
	INPUT_CSI_QUERY,
	INPUT_CSI_QUERY_PRIVATE,
	INPUT_CSI_RCP,
	INPUT_CSI_REP,
	INPUT_CSI_RM,
	INPUT_CSI_RM_PRIVATE,
	INPUT_CSI_SCP,
	INPUT_CSI_SD,
	INPUT_CSI_SGR,
	INPUT_CSI_SM,
	INPUT_CSI_SM_GRAPHICS,
	INPUT_CSI_SM_PRIVATE,
	INPUT_CSI_SU,
	INPUT_CSI_TBC,
	INPUT_CSI_VPA,
	INPUT_CSI_WINOPS,
	INPUT_CSI_XDA
};

/* Control (CSI) command table. */
static const struct input_table_entry input_csi_table[] = {
	{ '@', "",  INPUT_CSI_ICH },
	{ 'A', "",  INPUT_CSI_CUU },
	{ 'B', "",  INPUT_CSI_CUD },
	{ 'C', "",  INPUT_CSI_CUF },
	{ 'D', "",  INPUT_CSI_CUB },
	{ 'E', "",  INPUT_CSI_CNL },
	{ 'F', "",  INPUT_CSI_CPL },
	{ 'G', "",  INPUT_CSI_HPA },
	{ 'H', "",  INPUT_CSI_CUP },
	{ 'J', "",  INPUT_CSI_ED },
	{ 'K', "",  INPUT_CSI_EL },
	{ 'L', "",  INPUT_CSI_IL },
	{ 'M', "",  INPUT_CSI_DL },
	{ 'P', "",  INPUT_CSI_DCH },
	{ 'S', "",  INPUT_CSI_SU },
	{ 'S', "?", INPUT_CSI_SM_GRAPHICS },
	{ 'T', "",  INPUT_CSI_SD },
	{ 'X', "",  INPUT_CSI_ECH },
	{ 'Z', "",  INPUT_CSI_CBT },
	{ '`', "",  INPUT_CSI_HPA },
	{ 'b', "",  INPUT_CSI_REP },
	{ 'c', "",  INPUT_CSI_DA },
	{ 'c', ">", INPUT_CSI_DA_TWO },
	{ 'd', "",  INPUT_CSI_VPA },
	{ 'f', "",  INPUT_CSI_CUP },
	{ 'g', "",  INPUT_CSI_TBC },
	{ 'h', "",  INPUT_CSI_SM },
	{ 'h', "?", INPUT_CSI_SM_PRIVATE },
	{ 'l', "",  INPUT_CSI_RM },
	{ 'l', "?", INPUT_CSI_RM_PRIVATE },
	{ 'm', "",  INPUT_CSI_SGR },
	{ 'm', ">", INPUT_CSI_MODSET },
	{ 'n', "",  INPUT_CSI_DSR },
	{ 'n', ">", INPUT_CSI_MODOFF },
	{ 'n', "?", INPUT_CSI_DSR_PRIVATE },
	{ 'p', "$",  INPUT_CSI_QUERY },
	{ 'p', "?$", INPUT_CSI_QUERY_PRIVATE },
	{ 'q', " ", INPUT_CSI_DECSCUSR },
	{ 'q', ">", INPUT_CSI_XDA },
	{ 'r', "",  INPUT_CSI_DECSTBM },
	{ 's', "",  INPUT_CSI_SCP },
	{ 't', "",  INPUT_CSI_WINOPS },
	{ 'u', "",  INPUT_CSI_RCP }
};

/* Input transition. */
struct input_transition {
	int				first;
	int				last;

	int				(*handler)(struct input_ctx *);
	const struct input_state       *state;
};

/* Input state. */
struct input_state {
	const char			*name;
	void				(*enter)(struct input_ctx *);
	void				(*exit)(struct input_ctx *);
	const struct input_transition	*transitions;
	const struct input_transition	*lookup[256];
	u_char				 collect[256];
};

/* State transitions available from all states. */
#define INPUT_STATE_ANYWHERE \
	{ 0x18, 0x18, input_c0_dispatch, &input_state_ground }, \
	{ 0x1a, 0x1a, input_c0_dispatch, &input_state_ground }, \
	{ 0x1b, 0x1b, NULL,		 &input_state_esc_enter }

/* Forward declarations of state tables. */
static const struct input_transition input_state_ground_table[];
static const struct input_transition input_state_esc_enter_table[];
static const struct input_transition input_state_esc_intermediate_table[];
static const struct input_transition input_state_csi_enter_table[];
static const struct input_transition input_state_csi_parameter_table[];
static const struct input_transition input_state_csi_intermediate_table[];
static const struct input_transition input_state_csi_ignore_table[];
static const struct input_transition input_state_dcs_enter_table[];
static const struct input_transition input_state_dcs_parameter_table[];
static const struct input_transition input_state_dcs_intermediate_table[];
static const struct input_transition input_state_dcs_handler_table[];
static const struct input_transition input_state_dcs_escape_table[];
static const struct input_transition input_state_dcs_ignore_table[];
static const struct input_transition input_state_osc_string_table[];
static const struct input_transition input_state_apc_string_table[];
static const struct input_transition input_state_rename_string_table[];
static const struct input_transition input_state_consume_st_table[];

/* ground state definition. */
static struct input_state input_state_ground = {
	"ground",
	input_ground, NULL,
	input_state_ground_table,
	{ 0 }
};

/* esc_enter state definition. */
static struct input_state input_state_esc_enter = {
	"esc_enter",
	input_clear, NULL,
	input_state_esc_enter_table,
	{ 0 }
};

/* esc_intermediate state definition. */
static struct input_state input_state_esc_intermediate = {
	"esc_intermediate",
	NULL, NULL,
	input_state_esc_intermediate_table,
	{ 0 }
};

/* csi_enter state definition. */
static struct input_state input_state_csi_enter = {
	"csi_enter",
	input_clear, NULL,
	input_state_csi_enter_table,
	{ 0 }
};

/* csi_parameter state definition. */
static struct input_state input_state_csi_parameter = {
	"csi_parameter",
	NULL, NULL,
	input_state_csi_parameter_table,
	{ 0 }
};

/* csi_intermediate state definition. */
static struct input_state input_state_csi_intermediate = {
	"csi_intermediate",
	NULL, NULL,
	input_state_csi_intermediate_table,
	{ 0 }
};

/* csi_ignore state definition. */
static struct input_state input_state_csi_ignore = {
	"csi_ignore",
	NULL, NULL,
	input_state_csi_ignore_table,
	{ 0 }
};

/* dcs_enter state definition. */
static struct input_state input_state_dcs_enter = {
	"dcs_enter",
	input_enter_dcs, NULL,
	input_state_dcs_enter_table,
	{ 0 }
};

/* dcs_parameter state definition. */
static struct input_state input_state_dcs_parameter = {
	"dcs_parameter",
	NULL, NULL,
	input_state_dcs_parameter_table,
	{ 0 }
};

/* dcs_intermediate state definition. */
static struct input_state input_state_dcs_intermediate = {
	"dcs_intermediate",
	NULL, NULL,
	input_state_dcs_intermediate_table,
	{ 0 }
};

/* dcs_handler state definition. */
static struct input_state input_state_dcs_handler = {
	"dcs_handler",
	NULL, NULL,
	input_state_dcs_handler_table,
	{ 0 }
};

/* dcs_escape state definition. */
static struct input_state input_state_dcs_escape = {
	"dcs_escape",
	NULL, NULL,
	input_state_dcs_escape_table,
	{ 0 }
};

/* dcs_ignore state definition. */
static struct input_state input_state_dcs_ignore = {
	"dcs_ignore",
	NULL, NULL,
	input_state_dcs_ignore_table,
	{ 0 }
};

/* osc_string state definition. */
static struct input_state input_state_osc_string = {
	"osc_string",
	input_enter_osc, input_exit_osc,
	input_state_osc_string_table,
	{ 0 }
};

/* apc_string state definition. */
static struct input_state input_state_apc_string = {
	"apc_string",
	input_enter_apc, input_exit_apc,
	input_state_apc_string_table,
	{ 0 }
};

/* rename_string state definition. */
static struct input_state input_state_rename_string = {
	"rename_string",
	input_enter_rename, input_exit_rename,
	input_state_rename_string_table,
	{ 0 }
};

/* consume_st state definition. */
static struct input_state input_state_consume_st = {
	"consume_st",
	input_enter_rename, NULL, /* rename also waits for ST */
	input_state_consume_st_table,
	{ 0 }
};

/* ground state table. */
static const struct input_transition input_state_ground_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch, NULL },
	{ 0x19, 0x19, input_c0_dispatch, NULL },
	{ 0x1c, 0x1f, input_c0_dispatch, NULL },
	{ 0x20, 0x7e, input_print,	 NULL },
	{ 0x7f, 0x7f, NULL,		 NULL },
	{ 0x80, 0xff, input_top_bit_set, NULL },

	{ -1, -1, NULL, NULL }
};

/* esc_enter state table. */
static const struct input_transition input_state_esc_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_esc_intermediate },
	{ 0x30, 0x4f, input_esc_dispatch, &input_state_ground },
	{ 0x50, 0x50, NULL,		  &input_state_dcs_enter },
	{ 0x51, 0x57, input_esc_dispatch, &input_state_ground },
	{ 0x58, 0x58, NULL,		  &input_state_consume_st },
	{ 0x59, 0x59, input_esc_dispatch, &input_state_ground },
	{ 0x5a, 0x5a, input_esc_dispatch, &input_state_ground },
	{ 0x5b, 0x5b, NULL,		  &input_state_csi_enter },
	{ 0x5c, 0x5c, input_esc_dispatch, &input_state_ground },
	{ 0x5d, 0x5d, NULL,		  &input_state_osc_string },
	{ 0x5e, 0x5e, NULL,		  &input_state_consume_st },
	{ 0x5f, 0x5f, NULL,		  &input_state_apc_string },
	{ 0x60, 0x6a, input_esc_dispatch, &input_state_ground },
	{ 0x6b, 0x6b, NULL,		  &input_state_rename_string },
	{ 0x6c, 0x7e, input_esc_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* esc_intermediate state table. */
static const struct input_transition input_state_esc_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x7e, input_esc_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_enter state table. */
static const struct input_transition input_state_csi_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_csi_intermediate },
	{ 0x30, 0x39, input_parameter,	  &input_state_csi_parameter },
	{ 0x3a, 0x3a, input_parameter,	  &input_state_csi_parameter },
	{ 0x3b, 0x3b, input_parameter,	  &input_state_csi_parameter },
	{ 0x3c, 0x3f, input_intermediate, &input_state_csi_parameter },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_parameter state table. */
static const struct input_transition input_state_csi_parameter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_csi_intermediate },
	{ 0x30, 0x39, input_parameter,	  NULL },
	{ 0x3a, 0x3a, input_parameter,	  NULL },
	{ 0x3b, 0x3b, input_parameter,	  NULL },
	{ 0x3c, 0x3f, NULL,		  &input_state_csi_ignore },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_intermediate state table. */
static const struct input_transition input_state_csi_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch,  NULL },
	{ 0x19, 0x19, input_c0_dispatch,  NULL },
	{ 0x1c, 0x1f, input_c0_dispatch,  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x3f, NULL,		  &input_state_csi_ignore },
	{ 0x40, 0x7e, input_csi_dispatch, &input_state_ground },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* csi_ignore state table. */
static const struct input_transition input_state_csi_ignore_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, input_c0_dispatch, NULL },
	{ 0x19, 0x19, input_c0_dispatch, NULL },
	{ 0x1c, 0x1f, input_c0_dispatch, NULL },
	{ 0x20, 0x3f, NULL,		 NULL },
	{ 0x40, 0x7e, NULL,		 &input_state_ground },
	{ 0x7f, 0xff, NULL,		 NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_enter state table. */
static const struct input_transition input_state_dcs_enter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_dcs_intermediate },
	{ 0x30, 0x39, input_parameter,	  &input_state_dcs_parameter },
	{ 0x3a, 0x3a, NULL,		  &input_state_dcs_ignore },
	{ 0x3b, 0x3b, input_parameter,	  &input_state_dcs_parameter },
	{ 0x3c, 0x3f, input_intermediate, &input_state_dcs_parameter },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_parameter state table. */
static const struct input_transition input_state_dcs_parameter_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, &input_state_dcs_intermediate },
	{ 0x30, 0x39, input_parameter,	  NULL },
	{ 0x3a, 0x3a, NULL,		  &input_state_dcs_ignore },
	{ 0x3b, 0x3b, input_parameter,	  NULL },
	{ 0x3c, 0x3f, NULL,		  &input_state_dcs_ignore },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_intermediate state table. */
static const struct input_transition input_state_dcs_intermediate_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,		  NULL },
	{ 0x19, 0x19, NULL,		  NULL },
	{ 0x1c, 0x1f, NULL,		  NULL },
	{ 0x20, 0x2f, input_intermediate, NULL },
	{ 0x30, 0x3f, NULL,		  &input_state_dcs_ignore },
	{ 0x40, 0x7e, input_input,	  &input_state_dcs_handler },
	{ 0x7f, 0xff, NULL,		  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_handler state table. */
static const struct input_transition input_state_dcs_handler_table[] = {
	/* No INPUT_STATE_ANYWHERE */

	{ 0x00, 0x1a, input_input,  NULL },
	{ 0x1b, 0x1b, NULL,	    &input_state_dcs_escape },
	{ 0x1c, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* dcs_escape state table. */
static const struct input_transition input_state_dcs_escape_table[] = {
	/* No INPUT_STATE_ANYWHERE */

	{ 0x00, 0x5b, input_input,	  &input_state_dcs_handler },
	{ 0x5c, 0x5c, input_dcs_dispatch, &input_state_ground },
	{ 0x5d, 0xff, input_input,	  &input_state_dcs_handler },

	{ -1, -1, NULL, NULL }
};

/* dcs_ignore state table. */
static const struct input_transition input_state_dcs_ignore_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, NULL,	    NULL },

	{ -1, -1, NULL, NULL }
};

/* osc_string state table. */
static const struct input_transition input_state_osc_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x06, NULL,	     NULL },
	{ 0x07, 0x07, input_end_bel, &input_state_ground },
	{ 0x08, 0x17, NULL,	     NULL },
	{ 0x19, 0x19, NULL,	     NULL },
	{ 0x1c, 0x1f, NULL,	     NULL },
	{ 0x20, 0xff, input_input,   NULL },

	{ -1, -1, NULL, NULL }
};

/* apc_string state table. */
static const struct input_transition input_state_apc_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* rename_string state table. */
static const struct input_transition input_state_rename_string_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, input_input,  NULL },

	{ -1, -1, NULL, NULL }
};

/* consume_st state table. */
static const struct input_transition input_state_consume_st_table[] = {
	INPUT_STATE_ANYWHERE,

	{ 0x00, 0x17, NULL,	    NULL },
	{ 0x19, 0x19, NULL,	    NULL },
	{ 0x1c, 0x1f, NULL,	    NULL },
	{ 0x20, 0xff, NULL,	    NULL },

	{ -1, -1, NULL, NULL }
};

/* Maximum of bytes allowed to read in a single input. */
static size_t input_buffer_size = INPUT_BUF_DEFAULT_SIZE;

/* Input table compare. */
static int
input_table_compare(const void *key, const void *value)
{
	const struct input_ctx		*ictx = key;
	const struct input_table_entry	*entry = value;

	if (ictx->ch != entry->ch)
		return (ictx->ch - entry->ch);
	return (strcmp(ictx->interm_buf, entry->interm));
}

/* Stop UTF-8 and enter an invalid character. */
static void
input_stop_utf8(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	static struct utf8_data	 rc = { "\357\277\275", 3, 3, 1 };

	if (ictx->utf8started) {
		utf8_copy(&ictx->cell.cell.data, &rc);
		screen_write_collect_add(sctx, &ictx->cell.cell);
	}
	ictx->utf8started = 0;
}

/* Fire a title changed event. */
static void
input_fire_pane_title_changed(struct window_pane *wp, const char *title)
{
	struct event_payload	*ep;
	struct cmd_find_state	 fs;

	ep = event_payload_create();
	cmd_find_from_pane(&fs, wp, 0);
	event_payload_set_target(ep, &fs);
	event_payload_set_pane(ep, "pane", wp);
	event_payload_set_window(ep, "window", wp->window);
	event_payload_set_string(ep, "new_title", "%s", title);
	events_fire("pane-title-changed", ep);
}

/*
 * Timer - if this expires then have been waiting for a terminator for too
 * long, so reset to ground.
 */
static void
input_ground_timer_callback(__unused int fd, __unused short events, void *arg)
{
	struct input_ctx	*ictx = arg;

	log_debug("%s: %s expired" , __func__, ictx->state->name);
	ictx->ground_timer_armed = 0;
	input_reset(ictx, 0);
}

/* Start the timer. */
static void
input_start_ground_timer(struct input_ctx *ictx)
{
	struct timeval	tv = { .tv_sec = 5, .tv_usec = 0 };

	event_add(&ictx->ground_timer, &tv);
	ictx->ground_timer_armed = 1;
}

/* Reset cell state to default. */
static void
input_reset_cell(struct input_ctx *ictx)
{
	memcpy(&ictx->cell.cell, &grid_default_cell, sizeof ictx->cell.cell);
	ictx->cell.set = 0;
	ictx->cell.g0set = ictx->cell.g1set = 0;

	memcpy(&ictx->old_cell, &ictx->cell, sizeof ictx->old_cell);
	ictx->old_cx = 0;
	ictx->old_cy = 0;
}

/* Save screen state. */
static void
input_save_state(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct screen		*s = sctx->s;

	memcpy(&ictx->old_cell, &ictx->cell, sizeof ictx->old_cell);
	ictx->old_cx = s->cx;
	ictx->old_cy = s->cy;
	ictx->old_mode = s->mode;
}

/* Restore screen state. */
static void
input_restore_state(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;

	memcpy(&ictx->cell, &ictx->old_cell, sizeof ictx->cell);
	if (ictx->old_mode & MODE_ORIGIN)
		screen_write_mode_set(sctx, MODE_ORIGIN);
	else
		screen_write_mode_clear(sctx, MODE_ORIGIN);
	screen_write_cursormove(sctx, ictx->old_cx, ictx->old_cy, 0);
}

/* Preserve the first matching transition for every state and byte. */
static void
input_init_lookup(void)
{
	static int			 initialized;
	struct input_state		*states[] = {
		&input_state_ground,
		&input_state_esc_enter,
		&input_state_esc_intermediate,
		&input_state_csi_enter,
		&input_state_csi_parameter,
		&input_state_csi_intermediate,
		&input_state_csi_ignore,
		&input_state_dcs_enter,
		&input_state_dcs_parameter,
		&input_state_dcs_intermediate,
		&input_state_dcs_handler,
		&input_state_dcs_escape,
		&input_state_dcs_ignore,
		&input_state_osc_string,
		&input_state_apc_string,
		&input_state_rename_string,
		&input_state_consume_st
	};
	const struct input_transition	*itr;
	u_int				 i, ch;

	if (initialized)
		return;
	for (i = 0; i < nitems(states); i++) {
		for (ch = 0; ch < 256; ch++) {
			itr = states[i]->transitions;
			while (itr->first != -1 && itr->last != -1) {
				if (ch >= (u_int)itr->first &&
				    ch <= (u_int)itr->last)
					break;
				itr++;
			}
			if (itr->first == -1 || itr->last == -1)
				fatalx("no transition from state");
			states[i]->lookup[ch] = itr;
			if (itr->state == NULL) {
				if (itr->handler == NULL)
					states[i]->collect[ch] = 1;
				else if (itr->handler == input_parameter)
					states[i]->collect[ch] = 2;
				else if (itr->handler == input_intermediate)
					states[i]->collect[ch] = 3;
				else if (itr->handler == input_input)
					states[i]->collect[ch] = 4;
			}
		}
	}
	initialized = 1;
}

/* Initialise input parser. */
struct input_ctx *
input_init(struct window_pane *wp, struct colour_palette *palette)
{
	struct input_ctx	*ictx;

	input_init_lookup();
	ictx = xcalloc(1, sizeof *ictx);
	ictx->wp = wp;
	ictx->palette = palette;

	ictx->input_space = INPUT_BUF_START;
	ictx->input_buf = xmalloc(INPUT_BUF_START);

	ictx->since_ground = evbuffer_new();
	if (ictx->since_ground == NULL)
		fatalx("out of memory");
	evtimer_set(&ictx->ground_timer, input_ground_timer_callback, ictx);

	TAILQ_INIT(&ictx->requests);
	evtimer_set(&ictx->request_timer, input_request_timer_callback, ictx);

	input_reset(ictx, 0);
	return (ictx);
}

/* Destroy input parser. */
void
input_free(struct input_ctx *ictx)
{
	struct input_request	*ir, *ir1;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING)
			free(ictx->param_list[i].str);
	}

	TAILQ_FOREACH_SAFE(ir, &ictx->requests, entry, ir1)
		input_free_request(ir);
	event_del(&ictx->request_timer);

	free(ictx->input_buf);
#ifdef ENABLE_IMAGES
	kitty_free_state(ictx->kitty_state);
#endif
	evbuffer_free(ictx->since_ground);
	event_del(&ictx->ground_timer);

	screen_write_stop_sync(ictx->wp);

	free(ictx);
}

/* Reset input state and clear screen. */
void
input_reset(struct input_ctx *ictx, int clear)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;

	input_reset_cell(ictx);

	if (clear && wp != NULL) {
		if (TAILQ_EMPTY(&wp->modes))
			screen_write_start_pane(sctx, wp, &wp->base);
		else
			screen_write_start(sctx, &wp->base);
		sctx->flags |= SCREEN_WRITE_INPUT;
		screen_write_reset(sctx);
		screen_write_stop(sctx);
	}

	input_clear(ictx);

	ictx->state = &input_state_ground;
	ictx->flags = 0;
}

/* Return pending data. */
struct evbuffer *
input_pending(struct input_ctx *ictx)
{
	return (ictx->since_ground);
}

/* Change input state. */
static void
input_set_state(struct input_ctx *ictx, const struct input_transition *itr)
{
	if (ictx->state->exit != NULL)
		ictx->state->exit(ictx);
	ictx->state = itr->state;
	if (ictx->state->enter != NULL)
		ictx->state->enter(ictx);
}

/* Completed colour SGR parameters are independent of a pane's current style. */
struct input_csi_cache {
	u_char key[64];
	u_int length, count;
	int compiled;
	union {
		int values[24];
		struct {
			u_int attr_and, attr_or;
			int fg, bg, us;
			u_char colours, reset;
		} op;
	} u;
};
static struct input_csi_cache input_csi_cache[512];
static u_int input_csi_cache_attempts, input_csi_cache_hits;
static u_int input_csi_cache_bypass;

/* Unrepeated colour streams use the ordinary parser after a bounded probe. */
static void
input_csi_cache_note(int hit)
{
	input_csi_cache_hits += hit;
	if (++input_csi_cache_attempts == 512) {
		if (input_csi_cache_hits < 256)
			input_csi_cache_bypass = 8192;
		input_csi_cache_attempts = input_csi_cache_hits = 0;
	}
}

/* Numeric SGR consists of attribute bit operations and field assignments. */
static void
input_csi_cache_compile(struct input_csi_cache *cache)
{
	struct input_ctx scratch;
	struct grid_cell zero, ones;

	/* This handler reads and changes only cell.cell. Evaluate both bit poles. */
	memset(&scratch.cell.cell, 0, sizeof scratch.cell.cell);
	input_csi_dispatch_sgr_numbers(&scratch, cache->u.values, cache->count);
	zero = scratch.cell.cell;
	memset(&scratch.cell.cell, 0xff, sizeof scratch.cell.cell);
	input_csi_dispatch_sgr_numbers(&scratch, cache->u.values, cache->count);
	ones = scratch.cell.cell;
	cache->u.op.attr_and = zero.attr ^ ones.attr;
	cache->u.op.attr_or = zero.attr;
	cache->u.op.colours = (zero.fg == ones.fg ? 1 : 0) |
	    (zero.bg == ones.bg ? 2 : 0) | (zero.us == ones.us ? 4 : 0);
	cache->u.op.fg = zero.fg;
	cache->u.op.bg = zero.bg;
	cache->u.op.us = zero.us;
	cache->u.op.reset = zero.flags == ones.flags;
	cache->compiled = 1;
}

static void
input_csi_cache_apply(struct input_ctx *ictx, struct input_csi_cache *cache)
{
	struct grid_cell *gc = &ictx->cell.cell;
	u_int link;

	/* All cached keys have parameters; explicit SGR0 preserves the link. */
	if (cache->u.op.reset) {
		link = gc->link;
		*gc = grid_default_cell;
		gc->link = link;
	}
	gc->attr = (gc->attr & cache->u.op.attr_and) | cache->u.op.attr_or;
	if (cache->u.op.colours & 1)
		gc->fg = cache->u.op.fg;
	if (cache->u.op.colours & 2)
		gc->bg = cache->u.op.bg;
	if (cache->u.op.colours & 4)
		gc->us = cache->u.op.us;
}

/* Return a bounded cache candidate; a complete key comparison proves a hit. */
static struct input_csi_cache *
input_csi_cache_find(const u_char *buf, size_t len, size_t *length)
{
	const u_char *end;
	uint32_t a, b, c, d, hash;

	if (len < 12 || (buf[2] != '3' && buf[2] != '4' && buf[2] != '5') ||
	    buf[3] != '8' || buf[4] != ';' || log_get_level() != 0)
		return (NULL);
	if (input_csi_cache_bypass != 0) {
		input_csi_cache_bypass--;
		return (NULL);
	}
	if (len > sizeof input_csi_cache[0].key)
		len = sizeof input_csi_cache[0].key;
	end = memchr(buf + 2, 'm', len - 2);
	if (end == NULL)
		return (NULL);
	*length = end - buf + 1;
	if (*length < 12)
		return (NULL);
	/* Unaligned-safe samples; collisions are checked against every key byte. */
	memcpy(&a, buf + 4, sizeof a);
	memcpy(&b, buf + (*length - 4) / 2, sizeof b);
	memcpy(&c, buf + *length - 8, sizeof c);
	memcpy(&d, buf + *length / 4, sizeof d);
	hash = a + b * UINT32_C(0x9e3779b1) + *length;
	hash = (hash ^ c) * UINT32_C(0x85ebca6b);
	hash = (hash ^ d) * UINT32_C(0xc2b2ae35);
	hash ^= hash >> 16;
	return (&input_csi_cache[hash & (nitems(input_csi_cache) - 1)]);
}

/*
 * Parse complete ordinary SGR and CUP parameters while recognizing them.
 * Leave incomplete, complex and invalid sequences to the state machine,
 * without changing its state while looking ahead. With logging enabled,
 * use the state machine so its parameter diagnostics remain available.
 */
static size_t
input_try_csi(struct input_ctx *ictx, const u_char *buf, size_t len)
{
	int	 numbers[nitems(ictx->param_list)], present = 0;
	const int *values = numbers;
	struct input_csi_cache *cache;
	size_t cache_length = 0;
	int cache_hit = 0;
	uint64_t n = 0;
	u_char	 ch;
	u_int	 count = 0, i;
	size_t	 end;

	if (len < 3 || buf[1] != '[')
		return (0);
	cache = input_csi_cache_find(buf, len, &cache_length);
	if (cache != NULL) {
		cache_hit = cache->length == cache_length &&
		    memcmp(cache->key, buf, cache_length) == 0;
		input_csi_cache_note(cache_hit);
	}
	if (cache_hit) {
		if (!cache->compiled)
			input_csi_cache_compile(cache);
		end = cache_length - 1;
		ch = 'm';
		goto parsed;
	}
	for (end = 2; end < len; end++) {
		ch = buf[end];
		if (ch >= '0' && ch <= '9') {
			n = n * 10 + (ch - '0');
			if (n > INT_MAX)
				return (0);
			present = 1;
		} else if (ch == ';' || ch == 'm' || ch == 'H' || ch == 'f') {
			if (end != 2 || ch == ';') {
				if (count == nitems(numbers) - 1)
					return (0);
				numbers[count] = present ? (int)n : -1;
				count++;
			}
			if (ch != ';')
				break;
			n = 0;
			present = 0;
		} else
			return (0);
		if (end - 2 == sizeof ictx->param_buf - 1)
			return (0);
	}
	if (end == len || log_get_level() != 0)
		return (0);

	if (cache != NULL && ch == 'm' && end + 1 == cache_length) {
		cache->length = cache_length;
		cache->count = count;
		cache->compiled = 0;
		memcpy(cache->key, buf, cache_length);
		memcpy(cache->u.values, numbers, count * sizeof *numbers);
	}
parsed:
	if (ch == 'm')
		screen_write_collect_end(&ictx->ctx);
	input_clear(ictx);
	ictx->ch = ch;
	/* The raw parameter buffer is only needed by the state machine. */
	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING)
			free(ictx->param_list[i].str);
	}
	/* Completed numeric commands do not need persistent split parameters. */
	ictx->param_list_len = 0;
	if (cache_hit)
		input_csi_cache_apply(ictx, cache);
	else if (ch == 'm')
		input_csi_dispatch_sgr_numbers(ictx, values, count);
	else
		input_csi_dispatch_cup_args(ictx,
		    count > 0 && values[0] > 1 ? values[0] : 1,
		    count > 1 && values[1] > 1 ? values[1] : 1);
	ictx->flags &= ~INPUT_LAST;
	input_ground(ictx);
	return (end + 1);
}

/* Leave combining, joiner, modifier and Jamo cases to the scalar path. */
static int
input_utf8_independent(u_int cp, u_int width)
{
	if (width == 0 || width > 2)
		return (0);
	if (cp < 0x800)
		return (1);
	if ((cp >= 0x1100 && cp <= 0x11ff) || cp == 0x200d ||
	    cp == 0x3164 || cp == 0xfe0f ||
	    (cp >= 0xa960 && cp <= 0xa97c) ||
	    (cp >= 0xd7b0 && cp <= 0xd7fb) ||
	    (cp >= 0x1f1e6 && cp <= 0x1f1ff) ||
	    (cp >= 0x1f3fb && cp <= 0x1f3ff))
		return (0);
	return (1);
}

/* Decode a run once and write its ordinary cells as one screen operation. */
static size_t
input_try_utf8(struct input_ctx *ictx, const u_char *buf, size_t len)
{
	struct screen		*s = ictx->ctx.s;
	utf8_char		 data[128];
	size_t			 off = 0;
	u_int			 n = 0, width = 0, available, cp, columns;

	if ((s->mode & MODE_INSERT) || s->sel != NULL || log_debug_enabled)
		return (0);
	screen_write_collect_end(&ictx->ctx);
	available = screen_size_x(s);
	if (~s->mode & MODE_WRAP)
		available--;
	if (s->cx >= available && (~s->mode & MODE_WRAP))
		return (0);
	available = s->cx < available ? available - s->cx : 0;
	while (off < len && n < nitems(data)) {
		data[n] = utf8_decode_inline(buf + off, len - off, &cp);
		if (data[n] == 0)
			break;
		columns = UTF8_GET_WIDTH(data[n]);
		if (n == 0 && columns > available && (s->mode & MODE_WRAP))
			available = screen_size_x(s);
		if (!input_utf8_independent(cp, columns) ||
		    columns > available - width)
			break;
		width += columns;
		off += UTF8_GET_SIZE(data[n]);
		n++;
	}
	if (n == 0 || !screen_write_collect_add_utf8(&ictx->ctx,
	    &ictx->cell.cell, data, n, width))
		return (0);
	ictx->ch = buf[off - 1];
	utf8_to_data(data[n - 1], &ictx->utf8data);
	utf8_copy(&ictx->cell.cell.data, &ictx->utf8data);
	utf8_copy(&ictx->last, &ictx->cell.cell.data);
	ictx->flags |= INPUT_LAST;
	return (off);
}

/* Recognize complete ordinary ASCII lines without crossing a control. */
static size_t
input_try_lines(struct input_ctx *ictx, const u_char *buf, size_t len,
    size_t *retry)
{
	struct screen	*s = ictx->ctx.s;
	size_t		 off = 0, start, complete = 0, size, first_size = 0;
	u_int		 lines = 0, width = screen_size_x(s);
	u_char		 last = 0;
	int		 uniform = 1;

	if (s->cx != 0 ||
	    s->cy != s->rlower || len < 4 * (size_t)screen_size_y(s) ||
	    (s->mode & MODE_INSERT) || s->sel != NULL || log_debug_enabled)
		return (0);
	while (off < len) {
		start = off;
		while (off < len && buf[off] >= 0x20 && buf[off] < 0x7f)
			off++;
		if (off - start > width)
			break;
		size = off - start;
		if (off < len && buf[off] == '\r' &&
		    off + 1 < len && buf[off + 1] == '\n') {
			if (off > start)
				last = buf[off - 1];
			off += 2;
		} else if (off < len && buf[off] == '\n' &&
		    (s->mode & MODE_CRLF)) {
			if (off > start)
				last = buf[off - 1];
			off++;
		} else
			break;
		if (lines == 0)
			first_size = size;
		else if (size != first_size || memcmp(buf, buf + start, size) != 0)
			uniform = 0;
		complete = off;
		lines++;
	}
	/*
	 * No suffix of these ordinary lines can fill the same scroll region.
	 * Do not recognize the same bytes again after consuming one line.
	 * The retry point stops before any other control, which may change
	 * the region or interpretation. It never survives this input buffer.
	 */
	if (lines < s->rlower - s->rupper + 1) {
		*retry = off;
		return (0);
	}
	if (lines == 0 || !screen_write_collect_lines(&ictx->ctx,
	    &ictx->cell.cell, buf, complete, lines, uniform)) {
		/* The scalar path still consumes every byte. Avoid rescanning
		 * this rejected ordinary-line prefix at every subsequent row.
		 * Stop suppression before any unrecognized control or tail. */
		*retry = complete;
		return (0);
	}
	if (last != 0) {
		utf8_set(&ictx->cell.cell.data, last);
		utf8_copy(&ictx->last, &ictx->cell.cell.data);
	}
	ictx->ch = '\n';
	ictx->flags &= ~INPUT_LAST;
	return (complete);
}

/*
 * Fuse self transitions whose actions only append bytes or ignore them.
 * Enter/exit actions, UTF-8 decoding and terminal effects remain ordered in
 * the ordinary dispatcher. Classes come from each byte's winning transition,
 * so equal classes have equal actions without changing state.
 */
static size_t
input_collect_span(struct input_ctx *ictx, const struct input_transition *itr,
    const u_char *buf, size_t len)
{
	size_t		 used = 1, copy, room, available;
	u_char		*dst;
	size_t		*length;
	u_char		 type = ictx->state->collect[buf[0]];

	if (type == 0)
		return (0);
	while (used < len && ictx->state->collect[buf[used]] == type)
		used++;
	if (used == 1)
		return (0);

	if (itr->handler == input_parameter ||
	    itr->handler == input_intermediate) {
		if (itr->handler == input_parameter) {
			dst = ictx->param_buf;
			length = &ictx->param_len;
			room = sizeof ictx->param_buf - 1 - *length;
		} else {
			dst = ictx->interm_buf;
			length = &ictx->interm_len;
			room = sizeof ictx->interm_buf - 1 - *length;
		}
		copy = used < room ? used : room;
		memcpy(dst + *length, buf, copy);
		*length += copy;
		dst[*length] = '\0';
		if (copy != used)
			ictx->flags |= INPUT_DISCARD;
	} else if (itr->handler == input_input) {
		available = ictx->input_space;
		while (used >= available - ictx->input_len) {
			if (available * 2 > input_buffer_size)
				break;
			available *= 2;
		}
		if (available != ictx->input_space) {
			ictx->input_buf = xrealloc(ictx->input_buf, available);
			ictx->input_space = available;
		}
		room = available - 1 - ictx->input_len;
		copy = used < room ? used : room;
		memcpy(ictx->input_buf + ictx->input_len, buf, copy);
		ictx->input_len += copy;
		ictx->input_buf[ictx->input_len] = '\0';
		if (copy != used)
			ictx->flags |= INPUT_DISCARD;
	}
	ictx->ch = buf[used - 1];
	return (used);
}

/* Build the two ASCII state values without copying just-written data. */
static void
input_set_ascii_last(struct input_ctx *ictx, u_char ch)
{
 static const struct utf8_data empty = { { 0 }, 1, 1, 1 };

 /* Build both equal values directly. Copying the just-written35-byte
  * object creates overlapping loads across utf8_set's tail stores. */
 ictx->ch = ch;
 ictx->cell.cell.data = empty;
 ictx->last = empty;
 ictx->cell.cell.data.data[0] = ictx->last.data[0] = ch;
 ictx->flags |= INPUT_LAST;
}

/* Parse data. */
static void
input_parse(struct input_ctx* ictx, const u_char* buf, size_t len)
{
	struct screen_write_ctx* sctx = &ictx->ctx;
	const struct input_transition* itr = NULL;
	size_t off = 0, since = 0, used;
	size_t line_retry = 0, retry;

	/* Parse the input. */
	while (off < len) {
		if (off >= line_retry && ictx->ctx.s->cx == 0
		    && ictx->ctx.s->cy == ictx->ctx.s->rlower
		    && ictx->state == &input_state_ground && buf[off] < 0x80
		    && !ictx->utf8started
		    && (ictx->cell.set == 0 ? ictx->cell.g0set
					    : ictx->cell.g1set)
			!= 1) {
			retry = 0;
			used = input_try_lines(ictx, buf + off, len - off, &retry);
			line_retry = off + retry;
			if (used != 0) {
				off += used;
				since = off;
				continue;
			}
		}
		if (ictx->state == &input_state_ground && !ictx->utf8started &&
		    buf[off] >= 0xc2 && buf[off] <= 0xf4 &&
		    (ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set) != 1) {
			used = input_try_utf8(ictx, buf + off, len - off);
			if (used != 0) {
				off += used;
				since = off;
				continue;
			}
		}
		if (ictx->state == &input_state_ground &&
		    buf[off] >= 0x20 && buf[off] < 0x7f &&
		    !ictx->utf8started &&
		    (ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set) != 1) {
			used = screen_write_collect_add_ascii(sctx, &ictx->cell.cell,
			    buf + off, len - off);
			if (used != 0) {
				off += used;
				since = off;
				input_set_ascii_last(ictx, buf[off - 1]);
				continue;
			}
		}
		if (buf[off] == '\033' && ictx->state == &input_state_ground) {
			used = input_try_csi(ictx, buf + off, len - off);
			if (used != 0) {
				off += used;
				since = off;
				continue;
			}
		}
		ictx->ch = buf[off++];

		/* Find the transition directly from the current state and byte. */
		itr = ictx->state->lookup[ictx->ch];

		/*
		 * Any state except print stops the current collection. This is
		 * an optimization to avoid checking if the attributes have
		 * changed for every character. It will stop unnecessarily for
		 * sequences that don't make a terminal change, but they should
		 * be the minority.
		 */
		if (itr->handler != input_print)
			screen_write_collect_end(sctx);

		if (log_get_level() == 0 &&
		    (used = input_collect_span(ictx, itr, buf + off - 1,
		    len - off + 1)) != 0) {
			off += used - 1;
			if (ictx->state == &input_state_ground)
				since = off;
			continue;
		}

		/*
		 * Execute the handler, if any. Don't switch state if it
		 * returns non-zero.
		 */
		if (itr->handler != NULL && itr->handler(ictx) != 0) {
			if (ictx->state != &input_state_ground && off - 1 > since) {
				evbuffer_add(ictx->since_ground, buf + since,
				    off - 1 - since);
				ictx->since_ground_pending = 1;
			}
			since = off;
			continue;
		}

		/* And switch state, if necessary. */
		if (itr->state != NULL)
			input_set_state(ictx, itr);

		/* Only the unfinished sequence is needed after this parse. */
		if (ictx->state == &input_state_ground)
			since = off;
	}
	if (off > since) {
		evbuffer_add(ictx->since_ground, buf + since, off - since);
		ictx->since_ground_pending = 1;
	}
}

static void input_parse_vectors(struct window_pane *,
				const struct evbuffer_iovec *, int);

/* Parse input from pane. */
void
input_parse_pane(struct window_pane *wp)
{
	struct evbuffer_iovec vec[32];
	struct evbuffer_ptr pos;
	const void *fallback;
	size_t used = wp->offset.used - wp->base_offset;
	size_t size = EVBUFFER_LENGTH(wp->input) - used;
	int n;

	if (size == 0)
		return;
	wp->last_output_time = time(NULL);
	if (evbuffer_ptr_set(wp->input, &pos, used, EVBUFFER_PTR_SET) != 0)
		fatalx("cannot locate pane input");
	n = evbuffer_peek(wp->input, -1, &pos, vec, nitems(vec));
	if (n > 0 && n <= (int)nitems(vec))
		input_parse_vectors(wp, vec, n);
	else {
		fallback = window_pane_get_new_data(wp, &wp->offset, &size);
		input_parse_buffer(wp, fallback, size);
	}
	window_pane_update_used_data(wp, &wp->offset, size);
}

/* Parse given input. */
void
input_parse_buffer(struct window_pane *wp, const u_char *buf, size_t len)
{
	struct evbuffer_iovec vec = {.iov_base = (void *)buf, .iov_len = len};
	if (len != 0)
		input_parse_vectors(wp, &vec, 1);
}

/* Keep one collection open while consuming borrowed evbuffer chains. */
void
input_parse_buffers(struct window_pane *wp, const u_char *a, size_t na,
    const u_char *b, size_t nb)
{
	struct evbuffer_iovec vec[2] = {
		{.iov_base = (void *)a, .iov_len = na},
		{.iov_base = (void *)b, .iov_len = nb}
	};
	if (na != 0)
		input_parse_vectors(wp, vec, nb != 0 ? 2 : 1);
}

/* Keep one collection open while consuming borrowed evbuffer chains. */
static void
input_parse_vectors(struct window_pane *wp, const struct evbuffer_iovec *vec,
		    int count)
{
	struct input_ctx *ictx = wp->ictx;
	struct screen_write_ctx *sctx = &ictx->ctx;

	int n;

	wp->output_generation++;
	window_update_activity(wp->window);
	if (~wp->flags & PANE_ACTIVITY) {
		wp->flags |= PANE_ACTIVITY;
		events_fire_pane("pane-activity", wp);
	}
	wp->flags |= PANE_CHANGED;

	/* Flag new input while in a mode. */
	if (!TAILQ_EMPTY(&wp->modes))
		wp->flags |= PANE_UNSEENCHANGES;

	/* NULL wp if there is a mode set as don't want to update the tty. */
	if (TAILQ_EMPTY(&wp->modes))
		screen_write_start_pane(sctx, wp, &wp->base);
	else
		screen_write_start(sctx, &wp->base);
	sctx->flags |= SCREEN_WRITE_INPUT;

	for (n = 0; n < count; n++) {
		log_debug("%s: %%%u %s, %zu bytes: %.*s", __func__, wp->id,
			  ictx->state->name, vec[n].iov_len,
			  (int)vec[n].iov_len, (const char *)vec[n].iov_base);
		input_parse(ictx, vec[n].iov_base, vec[n].iov_len);
	}
	screen_write_stop(sctx);
}

/* Parse given input for screen. */
void
input_parse_screen(struct input_ctx *ictx, struct screen *s,
		   screen_write_init_ctx_cb cb, void *arg, const u_char *buf,
		   size_t len)
{
	struct screen_write_ctx *sctx = &ictx->ctx;

	if (len == 0)
		return;

	screen_write_start_callback(sctx, s, cb, arg);
	sctx->flags |= SCREEN_WRITE_INPUT;
	input_parse(ictx, buf, len);
	screen_write_stop(sctx);
}

/* Split the parameter list (if any). */
static int
input_split(struct input_ctx *ictx)
{
	const char *errstr;
	char *ptr, *out, *cp;
	struct input_param *ip;
	u_int i;
	int n, digit;

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING)
			free(ictx->param_list[i].str);
	}
	ictx->param_list_len = 0;

	if (ictx->param_len == 0)
		return (0);
	ip = &ictx->param_list[0];

	ptr = ictx->param_buf;
	while ((out = strsep(&ptr, ";")) != NULL) {
		if (*out == '\0')
			ip->type = INPUT_MISSING;
		else {
			/* Avoid general-purpose conversion for decimal parameters. */
			n = 0;
			for (cp = out; *cp >= '0' && *cp <= '9'; cp++) {
				digit = *cp - '0';
				if (n > INT_MAX / 10 ||
				    (n == INT_MAX / 10 && digit > INT_MAX % 10))
					break;
				n = n * 10 + digit;
			}
			if (*cp == '\0') {
				ip->type = INPUT_NUMBER;
				ip->num = n;
			} else if (strchr(out, ':') != NULL) {
				ip->type = INPUT_STRING;
				ip->str = xstrdup(out);
			} else {
				ip->type = INPUT_NUMBER;
				ip->num = strtonum(out, 0, INT_MAX, &errstr);
				if (errstr != NULL)
					return (-1);
			}
		}
		ip = &ictx->param_list[++ictx->param_list_len];
		if (ictx->param_list_len == nitems(ictx->param_list))
			return (-1);
	}

	if (log_get_level() == 0)
		return (0);

	for (i = 0; i < ictx->param_list_len; i++) {
		ip = &ictx->param_list[i];
		if (ip->type == INPUT_MISSING)
			log_debug("parameter %u: missing", i);
		else if (ip->type == INPUT_STRING)
			log_debug("parameter %u: string %s", i, ip->str);
		else if (ip->type == INPUT_NUMBER)
			log_debug("parameter %u: number %d", i, ip->num);
	}

	return (0);
}

/* Get an argument or return default value. */
static int
input_get(struct input_ctx *ictx, u_int validx, int minval, int defval)
{
	struct input_param	*ip;
	int			 retval;

	if (validx >= ictx->param_list_len)
	    return (defval);
	ip = &ictx->param_list[validx];
	if (ip->type == INPUT_MISSING)
		return (defval);
	if (ip->type == INPUT_STRING)
		return (-1);
	retval = ip->num;
	if (retval < minval)
		return (minval);
	return (retval);
}

/* Get the pane's bufferevent to send replies to. */
static struct bufferevent *
input_get_event(struct input_ctx *ictx)
{
	if (ictx->wp == NULL)
		return (NULL);
	return (ictx->wp->event);
}

/* Send reply. */
static void
input_send_reply(struct input_ctx *ictx, const char *reply)
{
	struct bufferevent	*bev = input_get_event(ictx);

	if (bev != NULL) {
		log_debug("%s: %s", __func__, reply);
		bufferevent_write(bev, reply, strlen(reply));
	}
}

/* Reply to terminal query. */
void printflike(3, 4)
input_reply(struct input_ctx *ictx, int add, const char *fmt, ...)
{
	struct input_request	*ir;
	va_list			 ap;
	char			*reply;

	va_start(ap, fmt);
	xvasprintf(&reply, fmt, ap);
	va_end(ap);

	if (add && !TAILQ_EMPTY(&ictx->requests)) {
		ir = input_make_request(ictx, INPUT_REQUEST_QUEUE);
		ir->data = reply;
	} else {
		input_send_reply(ictx, reply);
		free(reply);
	}
}

/* Clear saved state. */
static void
input_clear(struct input_ctx *ictx)
{
	if (ictx->ground_timer_armed) {
		event_del(&ictx->ground_timer);
		ictx->ground_timer_armed = 0;
	}

	*ictx->interm_buf = '\0';
	ictx->interm_len = 0;

	*ictx->param_buf = '\0';
	ictx->param_len = 0;

	*ictx->input_buf = '\0';
	ictx->input_len = 0;

	ictx->input_end = INPUT_END_ST;

	ictx->flags &= ~INPUT_DISCARD;
}

/* Reset for ground state. */
static void
input_ground(struct input_ctx *ictx)
{
	if (ictx->ground_timer_armed) {
		event_del(&ictx->ground_timer);
		ictx->ground_timer_armed = 0;
	}
	if (ictx->since_ground_pending) {
		evbuffer_drain(ictx->since_ground,
		    EVBUFFER_LENGTH(ictx->since_ground));
		ictx->since_ground_pending = 0;
	}

	if (ictx->input_space > INPUT_BUF_START) {
		ictx->input_space = INPUT_BUF_START;
		ictx->input_buf = xrealloc(ictx->input_buf, INPUT_BUF_START);
	}
}

/* Output this character to the screen. */
static int
input_print(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	int			 set;

	input_stop_utf8(ictx); /* can't be valid UTF-8 */

	set = ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set;
	if (set == 1)
		ictx->cell.cell.attr |= GRID_ATTR_CHARSET;
	else
		ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;
	utf8_set(&ictx->cell.cell.data, ictx->ch);
	screen_write_collect_add(sctx, &ictx->cell.cell);

	utf8_copy(&ictx->last, &ictx->cell.cell.data);
	ictx->flags |= INPUT_LAST;

	ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;

	return (0);
}

/* Collect intermediate string. */
static int
input_intermediate(struct input_ctx *ictx)
{
	if (ictx->interm_len == (sizeof ictx->interm_buf) - 1)
		ictx->flags |= INPUT_DISCARD;
	else {
		ictx->interm_buf[ictx->interm_len++] = ictx->ch;
		ictx->interm_buf[ictx->interm_len] = '\0';
	}

	return (0);
}

/* Collect parameter string. */
static int
input_parameter(struct input_ctx *ictx)
{
	if (ictx->param_len == (sizeof ictx->param_buf) - 1)
		ictx->flags |= INPUT_DISCARD;
	else {
		ictx->param_buf[ictx->param_len++] = ictx->ch;
		ictx->param_buf[ictx->param_len] = '\0';
	}

	return (0);
}

/* Collect input string. */
static int
input_input(struct input_ctx *ictx)
{
	size_t available;

	available = ictx->input_space;
	while (ictx->input_len + 1 >= available) {
		available *= 2;
		if (available > input_buffer_size) {
			ictx->flags |= INPUT_DISCARD;
			return (0);
		}
		ictx->input_buf = xrealloc(ictx->input_buf, available);
		ictx->input_space = available;
	}
	ictx->input_buf[ictx->input_len++] = ictx->ch;
	ictx->input_buf[ictx->input_len] = '\0';

	return (0);
}

/* Execute C0 control sequence. */
static int
input_c0_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	struct screen		*s = sctx->s;
	struct grid_cell	 gc, first_gc;
	u_int			 cx, line;
	u_int			 width;
	int			 has_content = 0;

	input_stop_utf8(ictx); /* can't be valid UTF-8 */

	log_debug("%s: '%c'", __func__, ictx->ch);

	switch (ictx->ch) {
	case '\000':	/* NUL */
		break;
	case '\007':	/* BEL */
		if (wp != NULL) {
			events_fire_pane("pane-bell", wp);
			alerts_queue(wp->window, WINDOW_BELL);
		}
		break;
	case '\010':	/* BS */
		screen_write_backspace(sctx);
		break;
	case '\011':	/* HT */
		/* Don't tab beyond the end of the line. */
		cx = s->cx;
		if (cx >= screen_size_x(s) - 1)
			break;

		/* Find the next tab point, or use the last column if none. */
		line = s->cy + s->grid->hsize;
		grid_get_cell(s->grid, cx, line, &first_gc);
		do {
			if (!has_content) {
				grid_get_cell(s->grid, cx, line, &gc);
				if (gc.data.size != 1 ||
				    *gc.data.data != ' ' ||
				    !grid_cells_look_equal(&gc, &first_gc))
					has_content = 1;
			}
			cx++;
			if (bit_test(s->tabs, cx))
				break;
		} while (cx < screen_size_x(s) - 1);

		width = cx - s->cx;
		if (has_content || width > sizeof gc.data.data)
			s->cx = cx;
		else {
			grid_get_cell(s->grid, s->cx, line, &gc);
			grid_set_tab(&gc, width);
			screen_write_collect_add(sctx, &gc);
		}
		break;
	case '\012':	/* LF */
	case '\013':	/* VT */
	case '\014':	/* FF */
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		if (s->mode & MODE_CRLF)
			screen_write_carriagereturn(sctx);
		break;
	case '\015':	/* CR */
		screen_write_carriagereturn(sctx);
		break;
	case '\016':	/* SO */
		ictx->cell.set = 1;
		break;
	case '\017':	/* SI */
		ictx->cell.set = 0;
		break;
	default:
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		break;
	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Execute escape sequence. */
static int
input_esc_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx		*sctx = &ictx->ctx;
	struct screen			*s = sctx->s;
	const struct input_table_entry	*entry;

	if (ictx->flags & INPUT_DISCARD)
		return (0);
	log_debug("%s: '%c', %s", __func__, ictx->ch, ictx->interm_buf);

	entry = bsearch(ictx, input_esc_table, nitems(input_esc_table),
	    sizeof input_esc_table[0], input_table_compare);
	if (entry == NULL) {
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		return (0);
	}

	switch (entry->type) {
	case INPUT_ESC_RIS:
		colour_palette_clear(ictx->palette);
		input_reset_cell(ictx);
		screen_write_reset(sctx);
		screen_write_fullredraw(sctx);
		break;
	case INPUT_ESC_IND:
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_NEL:
		screen_write_carriagereturn(sctx);
		screen_write_linefeed(sctx, 0, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_HTS:
		if (s->cx < screen_size_x(s))
			bit_set(s->tabs, s->cx);
		break;
	case INPUT_ESC_RI:
		screen_write_reverseindex(sctx, ictx->cell.cell.bg);
		break;
	case INPUT_ESC_DECKPAM:
		screen_write_mode_set(sctx, MODE_KKEYPAD);
		break;
	case INPUT_ESC_DECKPNM:
		screen_write_mode_clear(sctx, MODE_KKEYPAD);
		break;
	case INPUT_ESC_DECSC:
		input_save_state(ictx);
		break;
	case INPUT_ESC_DECRC:
		input_restore_state(ictx);
		break;
	case INPUT_ESC_DECALN:
		screen_write_alignmenttest(sctx);
		break;
	case INPUT_ESC_SCSG0_ON:
		ictx->cell.g0set = 1;
		break;
	case INPUT_ESC_SCSG0_OFF:
		ictx->cell.g0set = 0;
		break;
	case INPUT_ESC_SCSG1_ON:
		ictx->cell.g1set = 1;
		break;
	case INPUT_ESC_SCSG1_OFF:
		ictx->cell.g1set = 0;
		break;
	case INPUT_ESC_ST:
		/* ST terminates OSC but the state transition already did it. */
		break;
	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Execute control sequence. */
static int
input_csi_dispatch(struct input_ctx *ictx)
{
	struct screen_write_ctx		*sctx = &ictx->ctx;
	struct screen			*s = sctx->s;
	const struct input_table_entry	*entry;
	struct options			*oo;
	const char			*clip;
	int				 i, n, m, ek, set, p;
	u_int				 cx, bg = ictx->cell.cell.bg;
	int64_t				 v;

	if (ictx->flags & INPUT_DISCARD)
		return (0);

	log_debug("%s: '%c' \"%s\" \"%s\"", __func__, ictx->ch,
	    ictx->interm_buf, ictx->param_buf);

	if (input_split(ictx) != 0)
		return (0);

	entry = bsearch(ictx, input_csi_table, nitems(input_csi_table),
	    sizeof input_csi_table[0], input_table_compare);
	if (entry == NULL) {
		log_debug("%s: unknown '%c'", __func__, ictx->ch);
		return (0);
	}

	switch (entry->type) {
	case INPUT_CSI_CBT:
		/* Find the previous tab point, n times. */
		cx = s->cx;
		if (cx > screen_size_x(s) - 1)
			cx = screen_size_x(s) - 1;
		n = input_get(ictx, 0, 1, 1);
		if (n == -1)
			break;
		while (cx > 0 && n-- > 0) {
			do
				cx--;
			while (cx > 0 && !bit_test(s->tabs, cx));
		}
		s->cx = cx;
		break;
	case INPUT_CSI_CUB:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorleft(sctx, n);
		break;
	case INPUT_CSI_CUD:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursordown(sctx, n);
		break;
	case INPUT_CSI_CUF:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorright(sctx, n);
		break;
	case INPUT_CSI_CUP:
		input_csi_dispatch_cup(ictx);
		break;
	case INPUT_CSI_MODSET:
		n = input_get(ictx, 0, 0, 0);
		if (n != 4)
			break;
		m = input_get(ictx, 1, 0, 0);

		ek = options_get_number(global_options, "extended-keys");
		if (ek == 0)
			break;
		screen_write_mode_clear(sctx, EXTENDED_KEY_MODES);
		if (m == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED_2);
		else if (m == 1 || ek == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED);
		break;
	case INPUT_CSI_MODOFF:
		n = input_get(ictx, 0, 0, 0);
		if (n != 4)
			break;

		screen_write_mode_clear(sctx, MODE_KEYS_EXTENDED|
		    MODE_KEYS_EXTENDED_2);
		if (options_get_number(global_options, "extended-keys") == 2)
			screen_write_mode_set(sctx, MODE_KEYS_EXTENDED);
		break;
	case INPUT_CSI_WINOPS:
		input_csi_dispatch_winops(ictx);
		break;
	case INPUT_CSI_CUU:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursorup(sctx, n);
		break;
	case INPUT_CSI_CNL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1) {
			screen_write_carriagereturn(sctx);
			screen_write_cursordown(sctx, n);
		}
		break;
	case INPUT_CSI_CPL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1) {
			screen_write_carriagereturn(sctx);
			screen_write_cursorup(sctx, n);
		}
		break;
	case INPUT_CSI_DA:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			v = options_get_number(global_options, "set-clipboard");
			if (v == 2)
				clip = ";52";
			else
				clip = "";
#ifdef ENABLE_IMAGES
			input_reply(ictx, 1, "\033[?1;2;4%sc", clip);
#else
			input_reply(ictx, 1, "\033[?1;2%sc", clip);
#endif
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_DA_TWO:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			input_reply(ictx, 1, "\033[>84;0;0c");
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_ECH:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_clearcharacter(sctx, n, bg);
		break;
	case INPUT_CSI_DCH:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_deletecharacter(sctx, n, bg);
		break;
	case INPUT_CSI_DECSTBM:
		n = input_get(ictx, 0, 1, 1);
		m = input_get(ictx, 1, 1, screen_size_y(s));
		if (n != -1 && m != -1)
			screen_write_scrollregion(sctx, n - 1, m - 1);
		break;
	case INPUT_CSI_DL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_deleteline(sctx, n, bg);
		break;
	case INPUT_CSI_DSR_PRIVATE:
		switch (input_get(ictx, 0, 0, 0)) {
		case 996:
			input_report_current_theme(ictx);
			break;
		}
		break;
	case INPUT_CSI_QUERY:
		m = input_get(ictx, 0, 0, 0);
		switch (m) {
		case 4:		/* IRM */
			n = (s->mode & MODE_INSERT) ? 1 : 2;
			break;
		default:
			n = 0;
			break;
		}
		if (m > 0)
			input_reply(ictx, 1, "\033[%d;%d$y", m, n);
		break;
	case INPUT_CSI_QUERY_PRIVATE:
		m = input_get(ictx, 0, 0, 0);
		switch (m) {
		case 1:		/* DECCKM */
			n = (s->mode & MODE_KCURSOR) ? 1 : 2;
			break;
		case 3:		/* DECCOLM: always reset */
			n = 4;
			break;
		case 6:		/* DECOM */
			n = (s->mode & MODE_ORIGIN) ? 1 : 2;
			break;
		case 7:		/* DECAWM */
			n = (s->mode & MODE_WRAP) ? 1 : 2;
			break;
		case 12: /* cursor blink: 1 = blink, 2 = steady */
			if (s->cstyle != SCREEN_CURSOR_DEFAULT ||
			    s->mode & MODE_CURSOR_BLINKING_SET)
				n = (s->mode & MODE_CURSOR_BLINKING) ? 1 : 2;
			else {
				if (ictx->wp != NULL)
					oo = ictx->wp->options;
				else
					oo = global_w_options;
				p = options_get_number(oo, "cursor-style");

				/* blink for 1,3,5; steady for 0,2,4,6 */
				n = (p == 1 || p == 3 || p == 5) ? 1 : 2;
			}
			break;
		case 25:	/* DECTCEM */
			n = (s->mode & MODE_CURSOR) ? 1 : 2;
			break;
		case 47:
		case 1047:
		case 1049:	/* alternate screen */
			n = SCREEN_IS_ALTERNATE(s) ? 1 : 2;
			break;
		case 1000:	/* mouse: normal tracking */
			n = (s->mode & MODE_MOUSE_STANDARD) ? 1 : 2;
			break;
		case 1002:	/* mouse: button-event tracking */
			n = (s->mode & MODE_MOUSE_BUTTON) ? 1 : 2;
			break;
		case 1003:	/* mouse: any-event tracking */
			n = (s->mode & MODE_MOUSE_ALL) ? 1 : 2;
			break;
		case 1004:	/* focus reporting */
			n = (s->mode & MODE_FOCUSON) ? 1 : 2;
			break;
		case 1005:	/* mouse: UTF-8 */
			n = (s->mode & MODE_MOUSE_UTF8) ? 1 : 2;
			break;
		case 1006:	/* mouse: SGR */
			n = (s->mode & MODE_MOUSE_SGR) ? 1 : 2;
			break;
		case 2004:	/* bracketed paste */
			n = (s->mode & MODE_BRACKETPASTE) ? 1 : 2;
			break;
		case 2026:	/* synchronized output */
			n = (s->mode & MODE_SYNC) ? 1 : 2;
			break;
		case 2031:	/* theme update notifications */
			n = (s->mode & MODE_THEME_UPDATES) ? 1 : 2;
			break;
		default:
			n = 0;
			break;
		}
		if (m > 0)
			input_reply(ictx, 1, "\033[?%d;%d$y", m, n);
		break;
	case INPUT_CSI_DSR:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 5:
			input_reply(ictx, 1, "\033[0n");
			break;
		case 6:
			input_reply(ictx, 1, "\033[%u;%uR", s->cy + 1,
			    s->cx + 1);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_ED:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			screen_write_clearendofscreen(sctx, bg);
			break;
		case 1:
			screen_write_clearstartofscreen(sctx, bg);
			break;
		case 2:
			screen_write_clearscreen(sctx, bg);
			break;
		case 3:
			if (input_get(ictx, 1, 0, 0) == 0) {
				/*
				 * Linux console extension to clear history
				 * (for example before locking the screen).
				 */
				screen_write_clearhistory(sctx);
			}
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_EL:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			screen_write_clearendofline(sctx, bg);
			break;
		case 1:
			screen_write_clearstartofline(sctx, bg);
			break;
		case 2:
			screen_write_clearline(sctx, bg);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_HPA:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursormove(sctx, n - 1, -1, 1);
		break;
	case INPUT_CSI_ICH:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_insertcharacter(sctx, n, bg);
		break;
	case INPUT_CSI_IL:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_insertline(sctx, n, bg);
		break;
	case INPUT_CSI_REP:
		n = input_get(ictx, 0, 1, 1);
		if (n == -1)
			break;

		m = screen_size_x(s) - s->cx;
		if (s->mode & MODE_WRAP)
			m += screen_size_x(s);
		if (n > m)
			n = m;

		if (~ictx->flags & INPUT_LAST)
			break;

		set = ictx->cell.set == 0 ? ictx->cell.g0set : ictx->cell.g1set;
		if (set == 1)
			ictx->cell.cell.attr |= GRID_ATTR_CHARSET;
		else
			ictx->cell.cell.attr &= ~GRID_ATTR_CHARSET;
		utf8_copy(&ictx->cell.cell.data, &ictx->last);
		for (i = 0; i < n; i++)
			screen_write_collect_add(sctx, &ictx->cell.cell);
		break;
	case INPUT_CSI_RCP:
		input_restore_state(ictx);
		break;
	case INPUT_CSI_RM:
		input_csi_dispatch_rm(ictx);
		break;
	case INPUT_CSI_RM_PRIVATE:
		input_csi_dispatch_rm_private(ictx);
		break;
	case INPUT_CSI_SCP:
		input_save_state(ictx);
		break;
	case INPUT_CSI_SGR:
		input_csi_dispatch_sgr(ictx);
		break;
	case INPUT_CSI_SM:
		input_csi_dispatch_sm(ictx);
		break;
	case INPUT_CSI_SM_PRIVATE:
		input_csi_dispatch_sm_private(ictx);
		break;
	case INPUT_CSI_SM_GRAPHICS:
		input_csi_dispatch_sm_graphics(ictx);
		break;
	case INPUT_CSI_SU:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrollup(sctx, n, bg);
		break;
	case INPUT_CSI_SD:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_scrolldown(sctx, n, bg);
		break;
	case INPUT_CSI_TBC:
		switch (input_get(ictx, 0, 0, 0)) {
		case -1:
			break;
		case 0:
			if (s->cx < screen_size_x(s))
				bit_clear(s->tabs, s->cx);
			break;
		case 3:
			bit_nclear(s->tabs, 0, screen_size_x(s) - 1);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		break;
	case INPUT_CSI_VPA:
		n = input_get(ictx, 0, 1, 1);
		if (n != -1)
			screen_write_cursormove(sctx, -1, n - 1, 1);
		break;
	case INPUT_CSI_DECSCUSR:
		n = input_get(ictx, 0, 0, 0);
		if (n == -1)
			break;
		screen_set_cursor_style(n, &s->cstyle, &s->mode);
		if (n == 0) {
			/* Go back to default blinking state. */
			screen_write_mode_clear(sctx, MODE_CURSOR_BLINKING_SET);
		}
		break;
	case INPUT_CSI_XDA:
		n = input_get(ictx, 0, 0, 0);
		if (n == 0) {
			input_reply(ictx, 1, "\033P>|tmux %s\033\\",
			    getversion());
		}
		break;

	}

	ictx->flags &= ~INPUT_LAST;
	return (0);
}

/* Handle CUP through the same implementation for scalar and bulk parsing. */
static void
input_csi_dispatch_cup_args(struct input_ctx *ictx, int n, int m)
{
	if (n != -1 && m != -1)
		screen_write_collect_cursormove(&ictx->ctx, m - 1, n - 1, 1);
}

static void
input_csi_dispatch_cup(struct input_ctx *ictx)
{
	input_csi_dispatch_cup_args(ictx, input_get(ictx, 0, 1, 1),
	    input_get(ictx, 1, 1, 1));
}

/* Handle CSI RM. */
static void
input_csi_dispatch_rm(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 4:		/* IRM */
			screen_write_mode_clear(sctx, MODE_INSERT);
			break;
		case 34:
			screen_write_mode_set(sctx, MODE_CURSOR_VERY_VISIBLE);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI private RM. */
static void
input_csi_dispatch_rm_private(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 1:		/* DECCKM */
			screen_write_mode_clear(sctx, MODE_KCURSOR);
			break;
		case 3:		/* DECCOLM */
			screen_write_cursormove(sctx, 0, 0, 1);
			screen_write_clearscreen(sctx, gc->bg);
			break;
		case 6:		/* DECOM */
			screen_write_mode_clear(sctx, MODE_ORIGIN);
			screen_write_cursormove(sctx, 0, 0, 1);
			break;
		case 7:		/* DECAWM */
			screen_write_mode_clear(sctx, MODE_WRAP);
			break;
		case 12:
			screen_write_mode_clear(sctx, MODE_CURSOR_BLINKING);
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING_SET);
			break;
		case 25:	/* TCEM */
			screen_write_mode_clear(sctx, MODE_CURSOR);
			break;
		case 1000:
		case 1001:
		case 1002:
		case 1003:
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			break;
		case 1004:
			screen_write_mode_clear(sctx, MODE_FOCUSON);
			break;
		case 1005:
			screen_write_mode_clear(sctx, MODE_MOUSE_UTF8);
			break;
		case 1006:
			screen_write_mode_clear(sctx, MODE_MOUSE_SGR);
			break;
		case 47:
		case 1047:
			screen_write_alternateoff(sctx, gc, 0);
			break;
		case 1049:
			screen_write_alternateoff(sctx, gc, 1);
			break;
		case 2004:
			screen_write_mode_clear(sctx, MODE_BRACKETPASTE);
			break;
		case 2026:
			screen_write_end_sync(sctx);
			break;
		case 2031:
			screen_write_mode_clear(sctx, MODE_THEME_UPDATES);
			if (ictx->wp != NULL)
				ictx->wp->flags &= ~PANE_THEMECHANGED;
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI SM. */
static void
input_csi_dispatch_sm(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 4:		/* IRM */
			screen_write_mode_set(sctx, MODE_INSERT);
			break;
		case 34:
			screen_write_mode_clear(sctx, MODE_CURSOR_VERY_VISIBLE);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI private SM. */
static void
input_csi_dispatch_sm_private(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i;

	for (i = 0; i < ictx->param_list_len; i++) {
		switch (input_get(ictx, i, 0, -1)) {
		case -1:
			break;
		case 1:		/* DECCKM */
			screen_write_mode_set(sctx, MODE_KCURSOR);
			break;
		case 3:		/* DECCOLM */
			screen_write_cursormove(sctx, 0, 0, 1);
			screen_write_clearscreen(sctx, ictx->cell.cell.bg);
			break;
		case 6:		/* DECOM */
			screen_write_mode_set(sctx, MODE_ORIGIN);
			screen_write_cursormove(sctx, 0, 0, 1);
			break;
		case 7:		/* DECAWM */
			screen_write_mode_set(sctx, MODE_WRAP);
			break;
		case 12:
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING);
			screen_write_mode_set(sctx, MODE_CURSOR_BLINKING_SET);
			break;
		case 25:	/* TCEM */
			screen_write_mode_set(sctx, MODE_CURSOR);
			break;
		case 1000:
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_STANDARD);
			break;
		case 1002:
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_BUTTON);
			break;
		case 1003:
			screen_write_mode_clear(sctx, ALL_MOUSE_MODES);
			screen_write_mode_set(sctx, MODE_MOUSE_ALL);
			break;
		case 1004:
			screen_write_mode_set(sctx, MODE_FOCUSON);
			break;
		case 1005:
			screen_write_mode_set(sctx, MODE_MOUSE_UTF8);
			break;
		case 1006:
			screen_write_mode_set(sctx, MODE_MOUSE_SGR);
			break;
		case 47:
		case 1047:
			screen_write_alternateon(sctx, gc, 0);
			break;
		case 1049:
			screen_write_alternateon(sctx, gc, 1);
			break;
		case 2004:
			screen_write_mode_set(sctx, MODE_BRACKETPASTE);
			break;
		case 2031:
			screen_write_mode_set(sctx, MODE_THEME_UPDATES);
			if (wp != NULL) {
				wp->last_theme = window_pane_get_theme(wp);
				wp->flags &= ~PANE_THEMECHANGED;
			}
			break;
		case 2026:
			screen_write_start_sync(wp);
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
	}
}

/* Handle CSI graphics SM. */
static void
input_csi_dispatch_sm_graphics(__unused struct input_ctx *ictx)
{
#ifdef ENABLE_IMAGES
	int	n, m, o;

	if (ictx->param_list_len > 3)
		return;
	n = input_get(ictx, 0, 0, 0);
	m = input_get(ictx, 1, 0, 0);
	o = input_get(ictx, 2, 0, 0);

	if (n == 1 && (m == 1 || m == 2 || m == 4)) {
		input_reply(ictx, 1, "\033[?%d;0;%uS", n,
		    SIXEL_COLOUR_REGISTERS);
	} else
		input_reply(ictx, 1, "\033[?%d;3;%dS", n, o);
#endif
}

/* Handle CSI window operations. */
static void
input_csi_dispatch_winops(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct screen		*s = sctx->s;
	struct window_pane	*wp = ictx->wp;
	struct window		*w = NULL;
	u_int			 x = screen_size_x(s), y = screen_size_y(s);
	int			 n, m;

	if (wp != NULL)
		w = wp->window;

	m = 0;
	while ((n = input_get(ictx, m, 0, -1)) != -1) {
		switch (n) {
		case 1:
		case 2:
		case 5:
		case 6:
		case 7:
		case 11:
		case 13:
		case 20:
		case 21:
		case 24:
			break;
		case 3:
		case 4:
		case 8:
			m++;
			if (input_get(ictx, m, 0, -1) == -1)
				return;
			/* FALLTHROUGH */
		case 9:
		case 10:
			m++;
			if (input_get(ictx, m, 0, -1) == -1)
				return;
			break;
		case 14:
			if (w == NULL)
				break;
			input_reply(ictx, 1, "\033[4;%u;%ut", y * w->ypixel,
			    x * w->xpixel);
			break;
		case 15:
			if (w == NULL)
				break;
			input_reply(ictx, 1, "\033[5;%u;%ut", y * w->ypixel,
			    x * w->xpixel);
			break;
		case 16:
			if (w == NULL)
				break;
			input_reply(ictx, 1, "\033[6;%u;%ut", w->ypixel,
			    w->xpixel);
			break;
		case 18:
			input_reply(ictx, 1, "\033[8;%u;%ut", y, x);
			break;
		case 19:
			input_reply(ictx, 1, "\033[9;%u;%ut", y, x);
			break;
		case 22:
			m++;
			switch (input_get(ictx, m, 0, -1)) {
			case -1:
				return;
			case 0:
			case 2:
				screen_push_title(sctx->s);
				break;
			}
			break;
		case 23:
			m++;
			switch (input_get(ictx, m, 0, -1)) {
			case -1:
				return;
			case 0:
			case 2:
				screen_pop_title(sctx->s);
				if (wp == NULL)
					break;
				input_fire_pane_title_changed(wp, sctx->s->title);
				server_redraw_window_borders(w);
				server_status_window(w);
				break;
			}
			break;
		default:
			log_debug("%s: unknown '%c'", __func__, ictx->ch);
			break;
		}
		m++;
	}
}

/* Helper for 256 colour SGR. */
static int
input_csi_dispatch_sgr_256_do(struct input_ctx *ictx, int fgbg, int c)
{
	struct grid_cell	*gc = &ictx->cell.cell;

	if (c == -1 || c > 255) {
		if (fgbg == 38)
			gc->fg = 8;
		else if (fgbg == 48)
			gc->bg = 8;
	} else {
		if (fgbg == 38)
			gc->fg = c | COLOUR_FLAG_256;
		else if (fgbg == 48)
			gc->bg = c | COLOUR_FLAG_256;
		else if (fgbg == 58)
			gc->us = c | COLOUR_FLAG_256;
	}
	return (1);
}

/* Handle CSI SGR for 256 colours. */
static void
input_csi_dispatch_sgr_256(struct input_ctx *ictx, int fgbg, u_int *i)
{
	int	c;

	c = input_get(ictx, (*i) + 1, 0, -1);
	if (input_csi_dispatch_sgr_256_do(ictx, fgbg, c))
		(*i)++;
}

/* Helper for RGB colour SGR. */
static int
input_csi_dispatch_sgr_rgb_do(struct input_ctx *ictx, int fgbg, int r, int g,
    int b)
{
	struct grid_cell	*gc = &ictx->cell.cell;

	if (r == -1 || r > 255)
		return (0);
	if (g == -1 || g > 255)
		return (0);
	if (b == -1 || b > 255)
		return (0);

	if (fgbg == 38)
		gc->fg = colour_join_rgb(r, g, b);
	else if (fgbg == 48)
		gc->bg = colour_join_rgb(r, g, b);
	else if (fgbg == 58)
		gc->us = colour_join_rgb(r, g, b);
	return (1);
}

/* Handle CSI SGR for RGB colours. */
static void
input_csi_dispatch_sgr_rgb(struct input_ctx *ictx, int fgbg, u_int *i)
{
	int	r, g, b;

	r = input_get(ictx, (*i) + 1, 0, -1);
	g = input_get(ictx, (*i) + 2, 0, -1);
	b = input_get(ictx, (*i) + 3, 0, -1);
	if (input_csi_dispatch_sgr_rgb_do(ictx, fgbg, r, g, b))
		(*i) += 3;
}

/* Handle CSI SGR with a ISO parameter. */
static void
input_csi_dispatch_sgr_colon(struct input_ctx *ictx, u_int i)
{
	struct grid_cell	*gc = &ictx->cell.cell;
	char			*s = ictx->param_list[i].str, *copy, *ptr, *out;
	int			 p[8];
	u_int			 n;
	const char		*errstr;

	for (n = 0; n < nitems(p); n++)
		p[n] = -1;
	n = 0;

	ptr = copy = xstrdup(s);
	while ((out = strsep(&ptr, ":")) != NULL) {
		if (*out != '\0') {
			p[n++] = strtonum(out, 0, INT_MAX, &errstr);
			if (errstr != NULL || n == nitems(p)) {
				free(copy);
				return;
			}
		} else {
			n++;
			if (n == nitems(p)) {
				free(copy);
				return;
			}
		}
		log_debug("%s: %u = %d", __func__, n - 1, p[n - 1]);
	}
	free(copy);

	if (n == 0)
		return;
	if (p[0] == 4) {
		if (n != 2)
			return;
		switch (p[1]) {
		case 0:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			break;
		case 1:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE;
			break;
		case 2:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_2;
			break;
		case 3:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_3;
			break;
		case 4:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_4;
			break;
		case 5:
			gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
			gc->attr |= GRID_ATTR_UNDERSCORE_5;
			break;
		}
		return;
	}
	if (n < 2 || (p[0] != 38 && p[0] != 48 && p[0] != 58))
		return;
	switch (p[1]) {
	case 2:
		if (n < 3)
			break;
		if (n == 5)
			i = 2;
		else
			i = 3;
		if (n < i + 3)
			break;
		input_csi_dispatch_sgr_rgb_do(ictx, p[0], p[i], p[i + 1],
		    p[i + 2]);
		break;
	case 5:
		if (n < 3)
			break;
		input_csi_dispatch_sgr_256_do(ictx, p[0], p[2]);
		break;
	}
}

/* Apply an ordinary SGR value identically for either parser. */
static void
input_csi_dispatch_sgr_basic(struct grid_cell *gc, int n)
{
	u_int link;

	switch (n) {
	case 0:
		link = gc->link;
		memcpy(gc, &grid_default_cell, sizeof *gc);
		gc->link = link;
		break;
	case 1:
		gc->attr |= GRID_ATTR_BRIGHT;
		break;
	case 2:
		gc->attr |= GRID_ATTR_DIM;
		break;
	case 3:
		gc->attr |= GRID_ATTR_ITALICS;
		break;
	case 4:
		gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
		gc->attr |= GRID_ATTR_UNDERSCORE;
		break;
	case 5:
	case 6:
		gc->attr |= GRID_ATTR_BLINK;
		break;
	case 7:
		gc->attr |= GRID_ATTR_REVERSE;
		break;
	case 8:
		gc->attr |= GRID_ATTR_HIDDEN;
		break;
	case 9:
		gc->attr |= GRID_ATTR_STRIKETHROUGH;
		break;
	case 21:
		gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
		gc->attr |= GRID_ATTR_UNDERSCORE_2;
		break;
	case 22:
		gc->attr &= ~(GRID_ATTR_BRIGHT|GRID_ATTR_DIM);
		break;
	case 23:
		gc->attr &= ~GRID_ATTR_ITALICS;
		break;
	case 24:
		gc->attr &= ~GRID_ATTR_ALL_UNDERSCORE;
		break;
	case 25:
		gc->attr &= ~GRID_ATTR_BLINK;
		break;
	case 27:
		gc->attr &= ~GRID_ATTR_REVERSE;
		break;
	case 28:
		gc->attr &= ~GRID_ATTR_HIDDEN;
		break;
	case 29:
		gc->attr &= ~GRID_ATTR_STRIKETHROUGH;
		break;
	case 30:
	case 31:
	case 32:
	case 33:
	case 34:
	case 35:
	case 36:
	case 37:
		gc->fg = n - 30;
		break;
	case 39:
		gc->fg = 8;
		break;
	case 40:
	case 41:
	case 42:
	case 43:
	case 44:
	case 45:
	case 46:
	case 47:
		gc->bg = n - 40;
		break;
	case 49:
		gc->bg = 8;
		break;
	case 53:
		gc->attr |= GRID_ATTR_OVERLINE;
		break;
	case 55:
		gc->attr &= ~GRID_ATTR_OVERLINE;
		break;
	case 59:
		gc->us = 8;
		break;
	case 90:
	case 91:
	case 92:
	case 93:
	case 94:
	case 95:
	case 96:
	case 97:
		gc->fg = n;
		break;
	case 100:
	case 101:
	case 102:
	case 103:
	case 104:
	case 105:
	case 106:
	case 107:
		gc->bg = n - 10;
		break;
	}
}

/* Numeric complete SGR parameters, without split-parameter allocations/copies. */
static void
input_csi_dispatch_sgr_numbers(struct input_ctx *ictx, const int *values,
    u_int count)
{
	struct grid_cell *gc = &ictx->cell.cell;
	u_int i;
	int n, mode, r, g, b, c;

	if (count == 0) {
		memcpy(gc, &grid_default_cell, sizeof *gc);
		return;
	}
	for (i = 0; i < count; i++) {
		n = values[i] == -1 ? 0 : values[i];
		if (n != 38 && n != 48 && n != 58) {
			input_csi_dispatch_sgr_basic(gc, n);
			continue;
		}
		i++;
		mode = i < count ? values[i] : -1;
		if (mode == 2) {
			r = i + 1 < count ? values[i + 1] : -1;
			g = i + 2 < count ? values[i + 2] : -1;
			b = i + 3 < count ? values[i + 3] : -1;
			if (input_csi_dispatch_sgr_rgb_do(ictx, n, r, g, b))
				i += 3;
		} else if (mode == 5) {
			c = i + 1 < count ? values[i + 1] : -1;
			if (input_csi_dispatch_sgr_256_do(ictx, n, c))
				i++;
		}
	}
}

/* Handle CSI SGR. */
static void
input_csi_dispatch_sgr(struct input_ctx *ictx)
{
	struct grid_cell	*gc = &ictx->cell.cell;
	u_int			 i;
	int			 n;

	if (ictx->param_list_len == 0) {
		memcpy(gc, &grid_default_cell, sizeof *gc);
		return;
	}

	for (i = 0; i < ictx->param_list_len; i++) {
		if (ictx->param_list[i].type == INPUT_STRING) {
			input_csi_dispatch_sgr_colon(ictx, i);
			continue;
		}
		n = input_get(ictx, i, 0, 0);
		if (n == -1)
			continue;

		if (n == 38 || n == 48 || n == 58) {
			i++;
			switch (input_get(ictx, i, 0, -1)) {
			case 2:
				input_csi_dispatch_sgr_rgb(ictx, n, &i);
				break;
			case 5:
				input_csi_dispatch_sgr_256(ictx, n, &i);
				break;
			}
			continue;
		}

		input_csi_dispatch_sgr_basic(gc, n);
	}
}

/* End of input with BEL. */
static int
input_end_bel(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	ictx->input_end = INPUT_END_BEL;

	return (0);
}

/* DCS string started. */
static void
input_enter_dcs(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_ground_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* Handle DECRQSS query. */
static int
input_handle_decrqss(struct input_ctx *ictx)
{
	struct window_pane	*wp = ictx->wp;
	struct options		*oo;
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_char			*buf = ictx->input_buf;
	size_t			 len = ictx->input_len;
	struct screen		*s = sctx->s;
	int			 ps, opt_ps, blinking;

	if (len < 3 || buf[1] != ' ' || buf[2] != 'q')
		goto not_recognized;

	/*
	 * Cursor style query: DCS $ q SP q
	 * Reply: DCS 1 $ r SP q <Ps> SP q ST
	 */
	if (s->cstyle == SCREEN_CURSOR_BLOCK ||
	    s->cstyle == SCREEN_CURSOR_UNDERLINE ||
	    s->cstyle == SCREEN_CURSOR_BAR) {
		blinking = (s->mode & MODE_CURSOR_BLINKING) != 0;
		switch (s->cstyle) {
		case SCREEN_CURSOR_BLOCK:
			ps = blinking ? 1 : 2;
			break;
		case SCREEN_CURSOR_UNDERLINE:
			ps = blinking ? 3 : 4;
			break;
		case SCREEN_CURSOR_BAR:
			ps = blinking ? 5 : 6;
			break;
		default:
			ps = 0;
			break;
		}
	} else {
		/*
		 * No explicit runtime style: fall back to the configured
		 * cursor-style option (integer Ps 0..6). Pane options inherit.
		 */
		if (wp != NULL)
			oo = wp->options;
		else
			oo = global_w_options;
		opt_ps = options_get_number(oo, "cursor-style");

		/* Sanity clamp: valid Ps are 0..6 per DECSCUSR. */
		if (opt_ps < 0 || opt_ps > 6)
			opt_ps = 0;
		ps = opt_ps;
	}

	log_debug("%s: DECRQSS cursor -> Ps=%d (cstyle=%d mode=%#x)", __func__,
	    ps, s->cstyle, s->mode);

	input_reply(ictx, 1, "\033P1$r q%d q\033\\", ps);
	return (0);

not_recognized:
	/* Unrecognized DECRQSS: send DCS 0 $ r Pt ST. */
	input_reply(ictx, 1, "\033P0$r\033\\");
	return (0);
}

/* DCS terminator (ST) received. */
static int
input_dcs_dispatch(struct input_ctx *ictx)
{
	struct window_pane	*wp = ictx->wp;
	struct options		*oo;
	struct screen_write_ctx	*sctx = &ictx->ctx;
	u_char			*buf = ictx->input_buf;
	size_t			 len = ictx->input_len;
	const char		 prefix[] = "tmux;";
	const u_int		 prefixlen = (sizeof prefix) - 1;
	long long		 allow_passthrough = 0;
#ifdef ENABLE_IMAGES
	struct window		*w;
	struct sixel_image	*si;
	int			 p2;
#endif

	if (wp == NULL)
		oo = global_w_options;
	else
		oo = wp->options;

	if (ictx->flags & INPUT_DISCARD) {
		log_debug("%s: %zu bytes (discard)", __func__, len);
		return (0);
	}

#ifdef ENABLE_IMAGES
	if (wp != NULL && buf[0] == 'q' && ictx->interm_len == 0) {
		w = wp->window;
		if (input_split(ictx) != 0)
			return (0);
		p2 = input_get(ictx, 1, 0, 0);
		if (p2 == -1)
			p2 = 0;
		si = sixel_parse(buf, len, p2, w->xpixel, w->ypixel);
		if (si != NULL)
			sixel_write(sctx, si, ictx->cell.cell.bg);
	}
#endif

	/* DCS sequences with intermediate byte '$' (includes DECRQSS). */
	if (ictx->interm_len == 1 && ictx->interm_buf[0] == '$') {
		/* DECRQSS is DCS $ q Pt ST. */
		if (len >= 1 && buf[0] == 'q')
			return (input_handle_decrqss(ictx));

		/*
		 * Not DECRQSS. DCS '$' is currently only used by DECRQSS, but
		 * leave other '$' DCS (if any appear in future) to existing
		 * handlers.
		 */
	}

	allow_passthrough = options_get_number(oo, "allow-passthrough");
	if (!allow_passthrough)
		return (0);
	log_debug("%s: \"%s\"", __func__, buf);

	if (len >= prefixlen && strncmp(buf, prefix, prefixlen) == 0) {
		screen_write_rawstring(sctx, buf + prefixlen, len - prefixlen,
		    allow_passthrough == 2);
	}

	return (0);
}

/* OSC string started. */
static void
input_enter_osc(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_ground_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* OSC terminator (ST) received. */
static void
input_exit_osc(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	u_char			*p = ictx->input_buf;
	u_int			 option;

	if (ictx->flags & INPUT_DISCARD)
		return;
	if (ictx->input_len < 1 || *p < '0' || *p > '9')
		return;

	log_debug("%s: \"%s\" (end %s)", __func__, p,
	    ictx->input_end == INPUT_END_ST ? "ST" : "BEL");

	option = 0;
	while (*p >= '0' && *p <= '9')
		option = option * 10 + *p++ - '0';
	if (*p != ';' && *p != '\0')
		return;
	if (*p == ';')
		p++;

	switch (option) {
	case 0:
	case 2:
		if (wp != NULL &&
		    options_get_number(wp->options, "allow-set-title") &&
		    screen_set_title(sctx->s, p, 1)) {
			input_fire_pane_title_changed(wp, p);
			server_redraw_window_borders(wp->window);
			server_status_window(wp->window);
		}
		break;
	case 4:
		input_osc_4(ictx, p);
		break;
	case 7:
		if (wp != NULL && screen_set_path(sctx->s, p, 1)) {
			server_redraw_window_borders(wp->window);
			server_status_window(wp->window);
		}
		break;
	case 8:
		input_osc_8(ictx, p);
		break;
	case 9:
		input_osc_9(ictx, p);
		break;
	case 10:
		input_osc_10(ictx, p);
		break;
	case 11:
		input_osc_11(ictx, p);
		break;
	case 12:
		input_osc_12(ictx, p);
		break;
	case 52:
		input_osc_52(ictx, p);
		break;
	case 104:
		input_osc_104(ictx, p);
		break;
	case 110:
		input_osc_110(ictx, p);
		break;
	case 111:
		input_osc_111(ictx, p);
		break;
	case 112:
		input_osc_112(ictx, p);
		break;
	case 133:
		input_osc_133(ictx, p);
		break;
	default:
		log_debug("%s: unknown '%u'", __func__, option);
		break;
	}
}

/* APC string started. */
static void
input_enter_apc(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_ground_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* APC terminator (ST) received. */
static void
input_exit_apc(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct window_pane	*wp = ictx->wp;
	u_char			*buf = ictx->input_buf;
	size_t			 len = ictx->input_len;

	if (ictx->flags & INPUT_DISCARD)
		return;
	log_debug("%s: \"%s\" (size %zu)", __func__, buf, len);

#ifdef ENABLE_IMAGES
	if (wp != NULL && len > 1 && buf[0] == 'G') {
		kitty_parse_input(ictx, wp, sctx, ictx->cell.cell.bg,
		    &ictx->kitty_state, buf, len);
		return;
	}
#endif

	if (wp != NULL &&
	    options_get_number(wp->options, "allow-set-title") &&
	    screen_set_title(sctx->s, buf, 1)) {
		input_fire_pane_title_changed(wp, buf);
		server_redraw_window_borders(wp->window);
		server_status_window(wp->window);
	}
}

/* Rename string started. */
static void
input_enter_rename(struct input_ctx *ictx)
{
	log_debug("%s", __func__);

	input_clear(ictx);
	input_start_ground_timer(ictx);
	ictx->flags &= ~INPUT_LAST;
}

/* Rename terminator (ST) received. */
static void
input_exit_rename(struct input_ctx *ictx)
{
	struct window_pane	*wp = ictx->wp;
	struct window		*w;
	struct options_entry	*o;

	if (wp == NULL)
		return;
	if (ictx->flags & INPUT_DISCARD)
		return;
	if (!options_get_number(ictx->wp->options, "allow-rename"))
		return;
	log_debug("%s: \"%s\"", __func__, ictx->input_buf);

	if (!utf8_isvalid(ictx->input_buf))
		return;
	w = wp->window;

	if (ictx->input_len == 0) {
		o = options_get_only(w->options, "automatic-rename");
		if (o != NULL)
			options_remove_or_default(o, NULL, NULL);
		if (!options_get_number(w->options, "automatic-rename"))
			window_set_name(w, "", 1);
	} else {
		options_set_number(w->options, "automatic-rename", 0);
		window_set_name(w, ictx->input_buf, 1);
	}
	server_redraw_window_borders(w);
	server_status_window(w);
}

/* Open UTF-8 character. */
static int
input_top_bit_set(struct input_ctx *ictx)
{
	struct screen_write_ctx	*sctx = &ictx->ctx;
	struct utf8_data	*ud = &ictx->utf8data;

	ictx->flags &= ~INPUT_LAST;

	if (!ictx->utf8started) {
		ictx->utf8started = 1;
		if (utf8_open(ud, ictx->ch) != UTF8_MORE)
			input_stop_utf8(ictx);
		return (0);
	}

	switch (utf8_append(ud, ictx->ch)) {
	case UTF8_MORE:
		return (0);
	case UTF8_ERROR:
		input_stop_utf8(ictx);
		return (0);
	case UTF8_DONE:
		break;
	}
	ictx->utf8started = 0;

	log_debug("%s %hhu '%*s' (width %hhu)", __func__, ud->size,
	    (int)ud->size, ud->data, ud->width);

	utf8_copy(&ictx->cell.cell.data, ud);
	screen_write_collect_add(sctx, &ictx->cell.cell);

	utf8_copy(&ictx->last, &ictx->cell.cell.data);
	ictx->flags |= INPUT_LAST;

	return (0);
}

/* Reply to a colour request. */
static void
input_osc_colour_reply(struct input_ctx *ictx, int add, u_int n, int idx, int c,
    enum input_end_type end_type)
{
	u_char		 r, g, b;
	const char	*end;

	if (c != -1)
		c = colour_force_rgb(c);
	if (c == -1)
	    return;
	colour_split_rgb(c, &r, &g, &b);

	if (end_type == INPUT_END_BEL)
		end = "\007";
	else
		end = "\033\\";

	if (n == 4) {
		input_reply(ictx, add,
		    "\033]%u;%d;rgb:%02hhx%02hhx/%02hhx%02hhx/%02hhx%02hhx%s",
		    n, idx, r, r, g, g, b, b, end);
	} else {
		input_reply(ictx, add,
		    "\033]%u;rgb:%02hhx%02hhx/%02hhx%02hhx/%02hhx%02hhx%s",
		    n, r, r, g, g, b, b, end);
	}
}

/* Handle the OSC 4 sequence for setting (multiple) palette entries. */
static void
input_osc_4(struct input_ctx *ictx, const char *p)
{
	char			*copy, *s, *next = NULL;
	long			 idx;
	int			 c, bad = 0, redraw = 0;
	struct colour_palette	*palette = ictx->palette;

	copy = s = xstrdup(p);
	while (s != NULL && *s != '\0') {
		idx = strtol(s, &next, 10);
		if (*next++ != ';') {
			bad = 1;
			break;
		}
		if (idx < 0 || idx >= 256) {
			bad = 1;
			break;
		}

		s = strsep(&next, ";");
		if (strcmp(s, "?") == 0) {
			c = colour_palette_get(palette, idx|COLOUR_FLAG_256);
			if (c != -1) {
				input_osc_colour_reply(ictx, 1, 4, idx, c,
				    ictx->input_end);
				s = next;
				continue;
			}
			input_add_request(ictx, INPUT_REQUEST_PALETTE, idx);
			s = next;
			continue;
		}
		if ((c = colour_parseX11(s)) == -1) {
			s = next;
			continue;
		}
		if (colour_palette_set(palette, idx, c))
			redraw = 1;
		s = next;
	}
	if (bad)
		log_debug("bad OSC 4: %s", p);
	if (redraw)
		screen_write_fullredraw(&ictx->ctx);
	free(copy);
}

/* Handle the OSC 8 sequence for embedding hyperlinks. */
static void
input_osc_8(struct input_ctx *ictx, const char *p)
{
	struct hyperlinks	*hl = ictx->ctx.s->hyperlinks;
	struct grid_cell	*gc = &ictx->cell.cell;
	const char		*start, *end, *uri;
	char			*id = NULL;

	for (start = p; (end = strpbrk(start, ":;")) != NULL; start = end + 1) {
		if (end - start >= 4 && strncmp(start, "id=", 3) == 0) {
			if (id != NULL)
				goto bad;
			id = xstrndup(start + 3, end - start - 3);
		}

		/* The first ; is the end of parameters and start of the URI. */
		if (*end == ';')
			break;
	}
	if (end == NULL || *end != ';')
		goto bad;
	uri = end + 1;
	if (*uri == '\0') {
		gc->link = 0;
		free(id);
		return;
	}
	gc->link = hyperlinks_put(hl, uri, id);
	if (id == NULL)
		log_debug("hyperlink (anonymous) %s = %u", uri, gc->link);
	else
		log_debug("hyperlink (id=%s) %s = %u", id, uri, gc->link);
	free(id);
	return;

bad:
	log_debug("bad OSC 8 %s", p);
	free(id);
}

/* Helper to handle setting the progress bar and redrawing. */
static void
input_set_progress_bar(struct input_ctx *ictx, enum progress_bar_state state,
    int p)
{
	screen_set_progress_bar(ictx->ctx.s, state, p);
	if (ictx->wp != NULL) {
		server_redraw_window_borders(ictx->wp->window);
		server_status_window(ictx->wp->window);
	}
}

/* Handle the OSC 9;4 sequence for progress bars. */
static void
input_osc_9(struct input_ctx *ictx, const char *p)
{
	const char		*pb = p;
	enum progress_bar_state	 state;
	int			 progress = 0;

	if (*pb++ != '4')
		return;
	if (*pb == '\0' || (*pb == ';' && pb[1] == '\0'))
		return;

	if (*pb++ != ';')
		return;
	if (*pb < '0' || *pb > '4')
		goto bad;
	state = *pb++ - '0';

	if (*pb == '\0' || (*pb == ';' && pb[1] == '\0')) {
		input_set_progress_bar(ictx, state, -1);
		return;
	}

	if (*pb++ != ';')
		goto bad;
	while (*pb >= '0' && *pb <= '9') {
		if (progress > 100)
			goto bad;
		progress = progress * 10 + *pb++ - '0';
	}
	if (*pb != '\0' || progress < 0 || progress > 100)
		goto bad;
	input_set_progress_bar(ictx, state, progress);
	return;

bad:
	log_debug("bad OSC 9;4 %s", p);
}

/* Handle the OSC 10 sequence for setting and querying foreground colour. */
static void
input_osc_10(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	struct grid_cell	 defaults;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp == NULL)
			return;
		c = window_pane_get_fg_control_client(wp);
		if (c == -1) {
			tty_default_colours(&defaults, wp, NULL);
			if (COLOUR_DEFAULT(defaults.fg))
				c = window_pane_get_fg(wp);
			else
				c = defaults.fg;
		}
		input_osc_colour_reply(ictx, 1, 10, 0, c, ictx->input_end);
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 10: %s", p);
		return;
	}
	if (ictx->palette != NULL) {
		ictx->palette->fg = c;
		if (wp != NULL)
			wp->flags |= PANE_STYLECHANGED;
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 110 sequence for resetting foreground colour. */
static void
input_osc_110(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;

	if (*p != '\0')
		return;
	if (ictx->palette != NULL) {
		ictx->palette->fg = 8;
		if (wp != NULL)
			wp->flags |= PANE_STYLECHANGED;
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 11 sequence for setting and querying background colour. */
static void
input_osc_11(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp == NULL)
			return;
		c = window_pane_get_bg(wp);
		input_osc_colour_reply(ictx, 1, 11, 0, c, ictx->input_end);
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 11: %s", p);
		return;
	}
	if (ictx->palette != NULL) {
		ictx->palette->bg = c;
		if (wp != NULL)
			wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 111 sequence for resetting background colour. */
static void
input_osc_111(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;

	if (*p != '\0')
		return;
	if (ictx->palette != NULL) {
		ictx->palette->bg = 8;
		if (wp != NULL)
			wp->flags |= (PANE_STYLECHANGED|PANE_THEMECHANGED);
		screen_write_fullredraw(&ictx->ctx);
	}
}

/* Handle the OSC 12 sequence for setting and querying cursor colour. */
static void
input_osc_12(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	int			 c;

	if (strcmp(p, "?") == 0) {
		if (wp != NULL) {
			c = ictx->ctx.s->ccolour;
			if (c == -1)
				c = ictx->ctx.s->default_ccolour;
			input_osc_colour_reply(ictx, 1, 12, 0, c,
			    ictx->input_end);
		}
		return;
	}

	if ((c = colour_parseX11(p)) == -1) {
		log_debug("bad OSC 12: %s", p);
		return;
	}
	screen_set_cursor_colour(ictx->ctx.s, c);
}

/* Handle the OSC 112 sequence for resetting cursor colour. */
static void
input_osc_112(struct input_ctx *ictx, const char *p)
{
	if (*p == '\0') /* no arguments allowed */
		screen_set_cursor_colour(ictx->ctx.s, -1);
}

/* Parse the OSC 133 D exit status. */
static int
input_osc_133_exit_status(const char *p)
{
	const char	*end;
	char		*copy;
	const char	*errstr;
	long long	 status;

	if (p[1] != ';' || p[2] == '\0' || strchr(p + 2, '=') == p + 2)
		return (0);
	end = strchr(p + 2, ';');
	if (end == p + 2)
		return (0);
	if (end == NULL)
		copy = xstrdup(p + 2);
	else
		copy = xstrndup(p + 2, end - (p + 2));
	if (strchr(copy, '=') != NULL) {
		free(copy);
		return (0);
	}
	status = strtonum(copy, 0, 255, &errstr);
	free(copy);
	if (errstr != NULL)
		return (255);
	return (status);
}

/* Parse and clean the URL-encoded command line from OSC 133 C. */
static char *
input_osc_133_command_line(const char *p)
{
	const char	*end;
	char		 line[16385], h[3] = { 0 };
	size_t		 size = 0;
	u_char		 ch;

	if (p[1] != ';')
		return (NULL);
	p += 2;
	for (;;) {
		end = strchr(p, ';');
		if (end == NULL)
			end = p + strlen(p);
		if (end - p >= 12 && strncmp(p, "cmdline_url=", 12) == 0)
			break;
		if (*end == '\0')
			return (NULL);
		p = end + 1;
	}
	p += 12;
	while (p < end) {
		if (size == sizeof line - 1)
			return (NULL);
		ch = *p++;
		if (ch == '%') {
			if (end - p < 2 ||
			    !isxdigit((u_char)p[0]) ||
			    !isxdigit((u_char)p[1]))
				return (NULL);
			h[0] = p[0];
			h[1] = p[1];

			ch = strtoul(h, NULL, 16);
			if (ch == '\0')
				return (NULL);
			p += 2;
		}
		line[size++] = ch;
	}
	line[size] = '\0';
	return (clean_name(line, 1));
}

/* Fire an OSC 133 command event. */
static void
input_fire_command_event(struct window_pane *wp, const char *name)
{
	struct event_payload	*ep;
	struct cmd_find_state	 fs;
	time_t			 tstart = wp->cmd_start_time, end;
	time_t			 tend = wp->cmd_end_time;

	ep = event_payload_create();
	cmd_find_from_pane(&fs, wp, 0);
	event_payload_set_target(ep, &fs);
	if (fs.s != NULL)
		event_payload_set_session(ep, "session", fs.s);
	if (fs.wl != NULL)
		event_payload_set_int(ep, "window_index", fs.wl->idx);
	event_payload_set_window(ep, "window", wp->window);
	event_payload_set_pane(ep, "pane", wp);

	if (wp->cmd_line != NULL)
		event_payload_set_string(ep, "command_line", "%s", wp->cmd_line);
	if (wp->cmd_status != -1)
		event_payload_set_int(ep, "command_status", wp->cmd_status);
	if (tstart != 0)
		event_payload_set_time(ep, "command_start_time", tstart);
	if (tend != 0)
		event_payload_set_time(ep, "command_end_time", tend);

	if (tstart != 0) {
		if (wp->flags & PANE_CMDRUNNING)
			end = time(NULL);
		else
			end = tend;
		if (end < tstart)
			end = tstart;
		end -= tstart;
		event_payload_set_uint(ep, "command_duration", end);
	}

	events_fire(name, ep);
}

/* Handle the OSC 133 sequence. */
static void
input_osc_133(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	struct screen		*s = ictx->ctx.s;
	struct grid		*gd = s->grid;
	u_int			 line = s->cy + gd->hsize;
	struct grid_line	*gl = NULL;
	const char		*cp;
	int			 status;

	if (line < gd->hsize + gd->sy)
		gl = grid_get_line(gd, line);

	switch (*p) {
	case 'A':
	case 'N':
		if (gl != NULL) {
			gl->osc133_data.prompt_col = s->cx;
			gl->flags |= GRID_LINE_START_PROMPT;
		}
		if (wp != NULL) {
			wp->last_prompt_time = time(NULL);
			events_fire_pane("pane-shell-prompt", wp);
		}
		break;
	case 'P':
		if (gl != NULL) {
			cp = strstr(p, ";k=s");
			if (cp != NULL && (cp[4] == ';' || cp[4] == '\0'))
				gl->flags |= GRID_LINE_SECOND_PROMPT;
			else
				gl->flags |= GRID_LINE_START_PROMPT;
			gl->osc133_data.prompt_col = s->cx;
		}
		break;
	case 'B':
	case 'I':
		if (gl != NULL) {
			gl->flags |= GRID_LINE_START_COMMAND;
			gl->osc133_data.cmd_col = s->cx;
		}
		break;
	case 'C':
		if (gl != NULL) {
			gl->flags |= GRID_LINE_START_OUTPUT;
			gl->osc133_data.out_start_col = s->cx;
		}
		if (wp != NULL) {
			free(wp->cmd_line);
			wp->cmd_line = input_osc_133_command_line(p);
			wp->cmd_start_time = time(NULL);
			wp->cmd_end_time = 0;
			wp->flags |= PANE_CMDRUNNING;
			wp->cmd_status = -1;
			input_fire_command_event(wp, "pane-command-started");
		}
		break;
	case 'D':
		status = input_osc_133_exit_status(p);
		if (wp != NULL) {
			wp->cmd_end_time = time(NULL);
			wp->flags &= ~PANE_CMDRUNNING;
			wp->cmd_status = status;
			input_fire_command_event(wp, "pane-command-finished");
		}
		if (gl != NULL) {
			gl->flags |= GRID_LINE_END_OUTPUT;
			gl->osc133_data.out_end_col = s->cx;
			gl->osc133_data.exit_status = status;
		}
		break;
	}
}

/* Handle OSC 52 reply. */
static void
input_osc_52_reply(struct input_ctx *ictx, char clip)
{
	struct bufferevent	*ev = input_get_event(ictx);
	struct paste_buffer	*pb;
	int			 state;
	const char		*buf;
	size_t			 len;

	state = options_get_number(global_options, "get-clipboard");
	if (state == 0)
		return;
	if (state == 1) {
		if ((pb = paste_get_top(NULL)) == NULL)
			return;
		buf = paste_buffer_data(pb, &len);
		if (ictx->input_end == INPUT_END_BEL)
			input_reply_clipboard(ev, buf, len, "\007", clip);
		else
			input_reply_clipboard(ev, buf, len, "\033\\", clip);
		return;
	}
	input_add_request(ictx, INPUT_REQUEST_CLIPBOARD, ictx->input_end);
}

/*
 * Parse and decode OSC 52 clipboard data. Returns 0 on failure or if handled
 * as a query. On success, returns 1 and sets *out, *outlen, and *flags (caller
 * must free *out).
 */
static int
input_osc_52_parse(struct input_ctx *ictx, const char *p, u_char **out,
    int *outlen, char *clip)
{
	const char	*end;
	size_t		 len;
	const char	*allow = "cpqs01234567";
	u_int		 i, j = 0;

	if (options_get_number(global_options, "set-clipboard") != 2)
		return (0);

	if ((end = strchr(p, ';')) == NULL)
		return (0);
	end++;
	if (*end == '\0')
		return (0);
	log_debug("%s: %s", __func__, end);

	for (i = 0; p + i != end; i++) {
		if (strchr(allow, p[i]) != NULL && strchr(clip, p[i]) == NULL)
			clip[j++] = p[i];
	}
	log_debug("%s: %.*s %s", __func__, (int)(end - p - 1), p, clip);

	if (strcmp(end, "?") == 0) {
		input_osc_52_reply(ictx, *clip);
		return (0);
	}

	len = ((strlen(end) + 3) / 4) * 3;
	if (len == 0)
		return (0);

	*out = xmalloc(len);
	if ((*outlen = b64_pton(end, *out, len)) == -1) {
		free(*out);
		*out = NULL;
		return (0);
	}

	return (1);
}

/* Handle the OSC 52 sequence for setting the clipboard. */
static void
input_osc_52(struct input_ctx *ictx, const char *p)
{
	struct window_pane	*wp = ictx->wp;
	struct screen_write_ctx	 ctx;
	u_char			*out;
	int			 outlen;
	char			 clip[sizeof "cpqs01234567"] = "";

	if (!input_osc_52_parse(ictx, p, &out, &outlen, clip))
		return;

	if (wp == NULL) {
		free(out);
		return;
	} else {
		screen_write_start_pane(&ctx, wp, NULL);
		screen_write_setselection(&ctx, clip, out, outlen);
		screen_write_stop(&ctx);
		events_fire_pane("pane-set-clipboard", wp);
		paste_add(NULL, out, outlen);
	}
}

/* Handle the OSC 104 sequence for unsetting (multiple) palette entries. */
static void
input_osc_104(struct input_ctx *ictx, const char *p)
{
	char	*copy, *s;
	long	 idx;
	int	 bad = 0, redraw = 0;

	if (*p == '\0') {
		colour_palette_clear(ictx->palette);
		screen_write_fullredraw(&ictx->ctx);
		return;
	}

	copy = s = xstrdup(p);
	while (*s != '\0') {
		idx = strtol(s, &s, 10);
		if (*s != '\0' && *s != ';') {
			bad = 1;
			break;
		}
		if (idx < 0 || idx >= 256) {
			bad = 1;
			break;
		}
		if (colour_palette_set(ictx->palette, idx, -1))
			redraw = 1;
		if (*s == ';')
			s++;
	}
	if (bad)
		log_debug("bad OSC 104: %s", p);
	if (redraw)
		screen_write_fullredraw(&ictx->ctx);
	free(copy);
}

/* Send a clipboard reply. */
void
input_reply_clipboard(struct bufferevent *bev, const char *buf, size_t len,
    const char *end, char clip)
{
	char	*out = NULL;
	int	 outlen = 0;

	if (bev == NULL)
		return;

	if (buf != NULL && len != 0) {
		if (len >= ((size_t)INT_MAX * 3 / 4) - 1)
			return;
		outlen = 4 * ((len + 2) / 3) + 1;
		out = xmalloc(outlen);
		if ((outlen = b64_ntop(buf, len, out, outlen)) == -1) {
			free(out);
			return;
		}
	}

	bufferevent_write(bev, "\033]52;", 5);
	if (clip != 0)
		bufferevent_write(bev, &clip, 1);
	bufferevent_write(bev, ";", 1);
	if (outlen != 0)
		bufferevent_write(bev, out, outlen);
	bufferevent_write(bev, end, strlen(end));
	free(out);
}

/* Set input buffer size. */
void
input_set_buffer_size(size_t buffer_size)
{
	log_debug("%s: %lu -> %lu", __func__, input_buffer_size, buffer_size);
	input_buffer_size = buffer_size;
}

/* Request timer. Remove any requests that are too old. */
static void
input_request_timer_callback(__unused int fd, __unused short events, void *arg)
{
	struct input_ctx	*ictx = arg;
	struct input_request	*ir, *ir1;
	uint64_t		 t = get_timer();

	TAILQ_FOREACH_SAFE(ir, &ictx->requests, entry, ir1) {
		if (ir->t >= t - INPUT_REQUEST_TIMEOUT)
			continue;
		if (ir->type == INPUT_REQUEST_QUEUE)
			input_send_reply(ir->ictx, ir->data);
		input_free_request(ir);
	}
	if (ictx->request_count != 0)
		input_start_request_timer(ictx);
}

/* Start the request timer. */
static void
input_start_request_timer(struct input_ctx *ictx)
{
	struct timeval	tv = { .tv_sec = 0, .tv_usec = 100000 };

	event_add(&ictx->request_timer, &tv);
}

/* Create a request. */
static struct input_request *
input_make_request(struct input_ctx *ictx, enum input_request_type type)
{
	struct input_request	*ir;

	ir = xcalloc (1, sizeof *ir);
	ir->type = type;
	ir->ictx = ictx;
	ir->t = get_timer();

	if (++ictx->request_count == 1)
		input_start_request_timer(ictx);
	TAILQ_INSERT_TAIL(&ictx->requests, ir, entry);

	return (ir);
}

/* Free a request. */
static void
input_free_request(struct input_request *ir)
{
	struct input_ctx	*ictx = ir->ictx;

	if (ir->c != NULL)
		TAILQ_REMOVE(&ir->c->input_requests, ir, centry);

	ictx->request_count--;
	TAILQ_REMOVE(&ictx->requests, ir, entry);

	free(ir->data);
	free(ir);
}

/* Add a request. */
static int
input_add_request(struct input_ctx *ictx, enum input_request_type type, int idx)
{
	struct window_pane	*wp = ictx->wp;
	struct window		*w;
	struct client		*c = NULL, *loop;
	struct input_request	*ir;
	char			 s[64];

	if (wp == NULL)
		return (-1);
	w = wp->window;

	TAILQ_FOREACH(loop, &clients, entry) {
		if (loop->flags & CLIENT_UNATTACHEDFLAGS)
			continue;
		if (loop->session == NULL || !session_has(loop->session, w))
			continue;
		if (~loop->tty.flags & TTY_STARTED)
			continue;
		if (c == NULL)
			c = loop;
		else if (timercmp(&loop->activity_time, &c->activity_time, >))
			c = loop;
	}
	if (c == NULL)
		return (-1);

	ir = input_make_request(ictx, type);
	ir->c = c;
	ir->idx = idx;
	ir->end = ictx->input_end;
	TAILQ_INSERT_TAIL(&c->input_requests, ir, centry);

	switch (type) {
	case INPUT_REQUEST_PALETTE:
		xsnprintf(s, sizeof s, "\033]4;%d;?\033\\", idx);
		tty_puts(&c->tty, s);
		break;
	case INPUT_REQUEST_CLIPBOARD:
		tty_putcode_ss(&c->tty, TTYC_MS, "", "?");
		break;
	case INPUT_REQUEST_QUEUE:
		break;
	}

	return (0);
}

/* Handle a palette reply. */
static void
input_request_palette_reply(struct input_request *ir, void *data)
{
	struct input_request_palette_data	*pd = data;

	input_osc_colour_reply(ir->ictx, 0, 4, pd->idx, pd->c, ir->end);
}

/* Handle a clipboard reply. */
static void
input_request_clipboard_reply(struct input_request *ir, void *data)
{
	struct input_ctx			*ictx = ir->ictx;
	struct bufferevent			*ev = input_get_event(ictx);
	struct input_request_clipboard_data	*cd = data;
	int					 state;
	char					*copy;

	state = options_get_number(global_options, "get-clipboard");
	if (state == 0 || state == 1)
		return;
	if (state == 3) {
		copy = xmalloc(cd->len);
		memcpy(copy, cd->buf, cd->len);
		paste_add(NULL, copy, cd->len);
	}

	if (ir->idx == INPUT_END_BEL)
		input_reply_clipboard(ev, cd->buf, cd->len, "\007", cd->clip);
	else
		input_reply_clipboard(ev, cd->buf, cd->len, "\033\\", cd->clip);
}

/* Handle a reply to a request. */
void
input_request_reply(struct client *c, enum input_request_type type, void *data)
{
	struct input_request			*ir, *ir1, *found = NULL;
	struct input_request_palette_data	*pd = data;
	int					 complete = 0;

	TAILQ_FOREACH_SAFE(ir, &c->input_requests, centry, ir1) {
		if (ir->type != type) {
			input_free_request(ir);
			continue;
		}
		if (type == INPUT_REQUEST_PALETTE) {
			if (pd->idx != ir->idx) {
				input_free_request(ir);
				continue;
			}
			found = ir;
			break;
		}
		if (type == INPUT_REQUEST_CLIPBOARD) {
			found = ir;
			break;
		}
	}
	if (found == NULL)
		return;

	TAILQ_FOREACH_SAFE(ir, &found->ictx->requests, entry, ir1) {
		if (complete && ir->type != INPUT_REQUEST_QUEUE)
			break;
		if (ir->type == INPUT_REQUEST_QUEUE)
			input_send_reply(ir->ictx, ir->data);
		else if (ir == found) {
			if (ir->type == INPUT_REQUEST_PALETTE)
				input_request_palette_reply(ir, data);
			else if (ir->type == INPUT_REQUEST_CLIPBOARD)
				input_request_clipboard_reply(ir, data);
			complete = 1;
		}
		input_free_request(ir);
	}
}

/* Cancel pending requests for client. */
void
input_cancel_requests(struct client *c)
{
	struct input_request	*ir, *ir1;

	TAILQ_FOREACH_SAFE(ir, &c->input_requests, centry, ir1)
		input_free_request(ir);
}

/* Report current theme. */
static void
input_report_current_theme(struct input_ctx *ictx)
{
	struct window_pane	*wp = ictx->wp;

	if (wp != NULL) {
		wp->last_theme = window_pane_get_theme(wp);
		wp->flags &= ~PANE_THEMECHANGED;

		switch (wp->last_theme) {
		case THEME_DARK:
			log_debug("%s: %%%u dark theme", __func__, wp->id);
			input_reply(ictx, 0, "\033[?997;1n");
			break;
		case THEME_LIGHT:
			log_debug("%s: %%%u light theme", __func__, wp->id);
			input_reply(ictx, 0, "\033[?997;2n");
			break;
		case THEME_UNKNOWN:
			log_debug("%s: %%%u unknown theme", __func__, wp->id);
			break;
		}
	}
}
