/* $OpenBSD: grid.c,v 1.161 2026/10/12 15:40:17 nicm Exp $ */

/*
 * Copyright (c) 2008 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Grid data. This is the basic data structure that represents what is shown on
 * screen.
 *
 * A grid is a grid of cells (struct grid_cell). Lines are not allocated until
 * cells in that line are written to. The grid is split into history and
 * viewable data with the history starting at row (line) 0 and extending to
 * (hsize - 1); from hsize to hsize + (sy - 1) is the viewable data. All
 * functions in this file work on absolute coordinates, grid-view.c has
 * functions which work on the screen data.
 */

/* Default grid cell data. */
const struct grid_cell grid_default_cell = {
	{ { ' ' }, 0, 1, 1 }, 0, 0, 8, 8, 8, 0
};

/*
 * Padding grid cell data. Padding cells are the only zero width cell that
 * appears in the grid.
 */
static const struct grid_cell grid_padding_cell = {
	{ { '!' }, 0, 0, 0 }, 0, GRID_FLAG_PADDING, 8, 8, 8, 0
};

/* Cleared grid cell data. */
static const struct grid_cell grid_cleared_cell = {
	{ { ' ' }, 0, 1, 1 }, 0, GRID_FLAG_CLEARED, 8, 8, 8, 0
};
static const struct grid_cell_entry grid_cleared_entry = {
	{ .data = { 0, 8, 8, ' ' } }, GRID_FLAG_CLEARED
};

/* Store cell in entry. */
static void
grid_store_cell(struct grid_cell_entry *gce, const struct grid_cell *gc,
    u_char c)
{
	gce->flags = (gc->flags & ~GRID_FLAG_CLEARED);

	gce->data.fg = gc->fg & 0xff;
	if (gc->fg & COLOUR_FLAG_256)
		gce->flags |= GRID_FLAG_FG256;

	gce->data.bg = gc->bg & 0xff;
	if (gc->bg & COLOUR_FLAG_256)
		gce->flags |= GRID_FLAG_BG256;

	gce->data.attr = gc->attr;
	gce->data.data = c;
}

/* Check if a cell should be an extended cell. */
static int
grid_need_extended_cell(const struct grid_cell_entry *gce,
    const struct grid_cell *gc)
{
	if (gce->flags & GRID_FLAG_EXTENDED)
		return (1);
	if (gc->attr > 0xff)
		return (1);
	if (gc->data.size > 1 || gc->data.width > 1)
		return (1);
	if ((gc->fg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME)) ||
	    (gc->bg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME)))
		return (1);
	if (gc->us != 8) /* only supports 256 or RGB */
		return (1);
	if (gc->link != 0)
		return (1);
	if (gc->flags & GRID_FLAG_TAB)
		return (1);
	return (0);
}

/* Get an extended cell. */
static void
grid_get_extended_cell(struct grid_line *gl, struct grid_cell_entry *gce,
    int flags)
{
	u_int at, space;

	if (gl->extdsize == UINT_MAX)
		fatalx("too many extended cells");
	at = gl->extdsize + 1;
	if (at > gl->extdallocated) {
		if (gl->extdallocated < 16)
			space = 16;
		else if (gl->extdallocated > UINT_MAX / 2)
			space = UINT_MAX;
		else
			space = gl->extdallocated * 2;
		gl->extddata = xpool_reallocarray(gl->extddata, space,
		    sizeof *gl->extddata);
		gl->extdallocated = space;
	}
	gl->extdsize = at;

	gce->offset = at - 1;
	gce->flags = (flags | GRID_FLAG_EXTENDED);
}

/* Set cell as extended. */
static struct grid_extd_entry *
grid_extended_cell(struct grid_line *gl, struct grid_cell_entry *gce,
    const struct grid_cell *gc)
{
	struct grid_extd_entry	*gee;
	int			 flags = (gc->flags & ~GRID_FLAG_CLEARED);
	utf8_char		 uc;

	if (~gce->flags & GRID_FLAG_EXTENDED)
		grid_get_extended_cell(gl, gce, flags);
	else if (gce->offset >= gl->extdsize)
		fatalx("offset too big");
	gl->flags |= GRID_LINE_EXTENDED;
	if (gc->link != 0)
		gl->flags |= GRID_LINE_HYPERLINK;

	if (gc->flags & GRID_FLAG_TAB)
		uc = gc->data.width;
	else
		utf8_from_data(&gc->data, &uc);

	gee = &gl->extddata[gce->offset];
	gee->data = uc;
	gee->attr = gc->attr;
	gee->flags = flags;
	gee->fg = gc->fg;
	gee->bg = gc->bg;
	gee->us = gc->us;
	gee->link = gc->link;
	return (gee);
}

/* Free up unused extended cells. */
static void
grid_compact_line(struct grid_line *gl)
{
	int			 new_extdsize = 0;
	struct grid_extd_entry	*new_extddata;
	struct grid_cell_entry	*gce;
	struct grid_extd_entry	*gee;
	u_int			 px, idx;

	if (gl->extdsize == 0)
		return;

	for (px = 0; px < gl->cellsize; px++) {
		gce = &gl->celldata[px];
		if (gce->flags & GRID_FLAG_EXTENDED)
			new_extdsize++;
	}

	if (new_extdsize == 0) {
		xpool_free(gl->extddata);
		gl->extddata = NULL;
		gl->extdsize = 0;
		gl->extdallocated = 0;
		return;
	}
	new_extddata = xpool_reallocarray(NULL, new_extdsize, sizeof *gl->extddata);

	idx = 0;
	for (px = 0; px < gl->cellsize; px++) {
		gce = &gl->celldata[px];
		if (gce->flags & GRID_FLAG_EXTENDED) {
			gee = &gl->extddata[gce->offset];
			memcpy(&new_extddata[idx], gee, sizeof *gee);
			gce->offset = idx++;
		}
	}

	xpool_free(gl->extddata);
	gl->extddata = new_extddata;
	gl->extdsize = new_extdsize;
	gl->extdallocated = new_extdsize;
}

/* Get line data. */
struct grid_line *
grid_get_line(struct grid *gd, u_int line)
{
	return (&gd->linedata[line]);
}

/* Get line time. */
time_t
grid_line_time(const struct grid_line *gl)
{
	if (gl->time == 0)
		return (0);
	return (start_time.tv_sec + gl->time - 1);
}

/* Set line time. */
static void
grid_line_set_time(struct grid_line *gl)
{
	if (current_time == 0)
		gl->time = 0;
	else
		gl->time = current_time - start_time.tv_sec + 1;
}

/* Adjust number of lines. */
void
grid_adjust_lines(struct grid *gd, u_int lines)
{
	u_int space;

	/* Keep append and reflow growth amortized, but release large shrinks. */
	if (lines <= gd->lineallocated) {
		if (lines >= gd->lineallocated / 4)
			return;
		space = lines;
	} else {
		space = gd->lineallocated;
		if (space < 64)
			space = 64;
		while (space < lines) {
			if (space > UINT_MAX / 2) {
				space = lines;
				break;
			}
			space *= 2;
		}
	}
	gd->linedata = xreallocarray(gd->linedata, space, sizeof *gd->linedata);
	gd->lineallocated = space;
}

/* Copy default into a cell. */
static void
grid_clear_cell(struct grid *gd, u_int px, u_int py, u_int bg, int moved)
{
	struct grid_line	*gl = &gd->linedata[py];
	struct grid_cell_entry	*gce = &gl->celldata[px];
	struct grid_extd_entry	*gee;
	u_int			 old_offset = gce->offset;
	int			 had_extd = (gce->flags & GRID_FLAG_EXTENDED);

	memcpy(gce, &grid_cleared_entry, sizeof *gce);
	if (!moved && had_extd && old_offset < gl->extdsize) {
		gce->flags |= GRID_FLAG_EXTENDED;
		gce->offset = old_offset;
		gee = grid_extended_cell(gl, gce, &grid_cleared_cell);
		if (bg != 8)
			gee->bg = bg;
	} else if (bg != 8) {
		if (bg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME)) {
			grid_get_extended_cell(gl, gce, gce->flags);
			gee = grid_extended_cell(gl, gce, &grid_cleared_cell);
			gee->bg = bg;
		} else {
			if (bg & COLOUR_FLAG_256)
				gce->flags |= GRID_FLAG_BG256;
			gce->data.bg = bg;
		}
	}
}

/* Check grid y position. */
static int
grid_check_y(struct grid *gd, const char *from, u_int py)
{
	if (py >= gd->hsize + gd->sy) {
		log_debug("%s: y out of range: %u", from, py);
		return (-1);
	}
	return (0);
}

/* Check if two styles are (visibly) the same. */
int
grid_cells_look_equal(const struct grid_cell *gc1, const struct grid_cell *gc2)
{
	int flags1 = gc1->flags, flags2 = gc2->flags;

	if (gc1->fg != gc2->fg || gc1->bg != gc2->bg)
		return (0);
	if (gc1->attr != gc2->attr)
		return (0);
	if ((flags1 & ~GRID_FLAG_CLEARED) != (flags2 & ~GRID_FLAG_CLEARED))
		return (0);
	if (gc1->link != gc2->link)
		return (0);
	return (1);
}

/* Compare grid cells. Return 1 if equal, 0 if not. */
int
grid_cells_equal(const struct grid_cell *gc1, const struct grid_cell *gc2)
{
	if (!grid_cells_look_equal(gc1, gc2))
		return (0);
	if (gc1->data.width != gc2->data.width)
		return (0);
	if (gc1->data.size != gc2->data.size)
		return (0);
	return (memcmp(gc1->data.data, gc2->data.data, gc1->data.size) == 0);
}

/* Set grid cell to a tab. */
void
grid_set_tab(struct grid_cell *gc, u_int width)
{
	memset(gc->data.data, 0, sizeof gc->data.data);
	gc->flags |= GRID_FLAG_TAB;
	gc->flags &= ~GRID_FLAG_PADDING;
	gc->data.width = gc->data.size = gc->data.have = width;
	memset(gc->data.data, ' ', gc->data.size);
}

/* Free one line. */
static void
grid_free_line(struct grid *gd, u_int py)
{
	struct grid_line	*gl = &gd->linedata[py];

#ifdef ENABLE_IMAGES
	image_grid_free_line(gd, gl);
#endif
	xpool_free(gl->celldata);
	xpool_free(gl->extddata);
	memset(gl, 0, sizeof *gl);
}

/* Free several lines. */
void
grid_free_lines(struct grid *gd, u_int py, u_int ny)
{
	u_int	yy;

	for (yy = py; yy < py + ny; yy++)
		grid_free_line(gd, yy);
}

/* Create a new grid. */
struct grid *
grid_create(u_int sx, u_int sy, u_int hlimit)
{
	struct grid	*gd;

	gd = xcalloc(1, sizeof *gd);
	gd->sx = sx;
	gd->sy = sy;

	if (hlimit != 0)
		gd->flags = GRID_HISTORY;
	gd->hlimit = hlimit;

	if (gd->sy != 0)
		gd->linedata = xcalloc(gd->sy, sizeof *gd->linedata);
	gd->lineallocated = gd->sy;
	return (gd);
}

/* Destroy grid. */
void
grid_destroy(struct grid *gd)
{
	grid_free_lines(gd, 0, gd->hsize + gd->sy);
	xpool_free(gd->spare.celldata);
	xpool_free(gd->spare.extddata);
#ifdef ENABLE_IMAGES
	image_grid_free(gd);
#endif
	free(gd->linedata);
	free(gd);
}

/* Compare grids. */
int
grid_compare(struct grid *ga, struct grid *gb)
{
	struct grid_line	*gla, *glb;
	struct grid_cell	 gca, gcb;
	u_int			 xx, yy;

	if (ga->sx != gb->sx || ga->sy != gb->sy)
		return (1);

	for (yy = 0; yy < ga->sy; yy++) {
		gla = &ga->linedata[yy];
		glb = &gb->linedata[yy];
		if (gla->cellsize != glb->cellsize)
			return (1);
		for (xx = 0; xx < gla->cellsize; xx++) {
			grid_get_cell(ga, xx, yy, &gca);
			grid_get_cell(gb, xx, yy, &gcb);
			if (!grid_cells_equal(&gca, &gcb))
				return (1);
		}
	}

	return (0);
}

/* Trim lines from the history. */
static void
grid_trim_history(struct grid *gd, u_int ny)
{
	u_int	remaining;

	grid_free_lines(gd, 0, ny);
	remaining = gd->hsize + gd->sy - ny;
	memmove(&gd->linedata[0], &gd->linedata[ny],
	    remaining * (sizeof *gd->linedata));
	memset(&gd->linedata[remaining], 0, ny * (sizeof *gd->linedata));
}

/*
 * Collect lines from the history if at the limit. Free the top (oldest) 10%
 * and shift up.
 */
void
grid_collect_history(struct grid *gd, int all)
{
	u_int	ny;

	if (gd->hsize == 0 || gd->hsize < gd->hlimit)
		return;

	if (all)
		ny = gd->hsize - gd->hlimit;
	else
		ny = gd->hlimit / 10;
	if (ny < 1)
		ny = 1;
	if (ny > gd->hsize)
		ny = gd->hsize;

	/*
	 * Free the lines from 0 to ny then move the remaining lines over
	 * them.
	 */
	grid_trim_history(gd, ny);

	gd->hsize -= ny;
	gd->scroll_collected += ny;
	if (gd->hscrolled > gd->hsize)
		gd->hscrolled = gd->hsize;
}

/* Remove lines from the bottom of the history. */
void
grid_remove_history(struct grid *gd, u_int ny)
{
	u_int	yy, start;

	if (ny > gd->hsize)
		return;
	start = gd->hsize + gd->sy - ny;
	for (yy = 0; yy < ny; yy++)
		grid_free_line(gd, start + yy);
	memset(&gd->linedata[start], 0, ny * sizeof *gd->linedata);
	gd->hsize -= ny;
}

/*
 * Scroll the entire visible screen, moving one line into the history. Just
 * allocate a new line at the bottom and move the history size indicator.
 */
void
grid_scroll_history(struct grid *gd, u_int bg)
{
	u_int	yy;

	yy = gd->hsize + gd->sy;
	grid_adjust_lines(gd, yy + 1);
	grid_empty_line(gd, yy, bg);

	gd->hscrolled++;
	grid_compact_line(&gd->linedata[gd->hsize]);
	grid_line_set_time(&gd->linedata[gd->hsize]);
	gd->hsize++;
	gd->scroll_added++;
}

/* Copy a complete line template, reusing storage from a discarded line. */
static void
grid_reuse_line(struct grid_line *dst, const struct grid_line *src,
    struct grid_line *old)
{
	struct grid_cell_entry	*cells = old->celldata;
	struct grid_extd_entry	*extended = old->extddata;
	size_t			 size;
	int			 same;

	if (src->cellsize == 0) {
		xpool_free(cells);
		cells = NULL;
	} else {
		same = old->cellsize == src->cellsize;
		if (!same)
			cells = xpool_reallocarray(cells, src->cellsize, sizeof *cells);
		size = src->cellsize * sizeof *cells;
		if (!same || memcmp(cells, src->celldata, size) != 0)
			memcpy(cells, src->celldata, size);
	}
	if (src->extdsize == 0) {
		xpool_free(extended);
		extended = NULL;
	} else {
		same = old->extdallocated == src->extdsize;
		if (!same)
			extended = xpool_reallocarray(extended, src->extdsize,
			    sizeof *extended);
		size = src->extdsize * sizeof *extended;
		if (!same || old->extdsize != src->extdsize ||
		    memcmp(extended, src->extddata, size) != 0)
			memcpy(extended, src->extddata, size);
	}
	memcpy(dst, src, sizeof *dst);
	dst->celldata = cells;
	dst->extddata = extended;
	dst->extdallocated = src->extdsize;
}

/*
 * Scroll repeated complete lines into history. The caller has written the
 * first line at the bottom already. Simulate collection counters exactly,
 * retain old rows, and reuse storage from rows which the normal 10% history
 * collection would discard. Only the final retained new rows are materialized.
 */
void
grid_scroll_history_lines(struct grid *gd, const struct grid_cell *gc,
    const char *line, size_t len, u_int lines)
{
	struct grid		 template;
	struct grid_line		 text, blank, *pool = NULL, *old, empty = { 0 };
	struct grid_cell		 cell;
	u_int			 old_hsize = gd->hsize;
	u_int			 old_size = gd->hsize + gd->sy, hsize = gd->hsize;
	u_int			 hscrolled = gd->hscrolled, collected = 0;
	u_int			 n, i, drop, remaining, pool_size, size, first;

	for (i = 0; i < lines; i++) {
		if (hsize != 0 && hsize >= gd->hlimit) {
			n = gd->hlimit / 10;
			if (n == 0)
				n = 1;
			if (n > hsize)
				n = hsize;
			hsize -= n;
			collected += n;
			if (hscrolled > hsize)
				hscrolled = hsize;
		}
		hsize++;
		hscrolled++;
	}

	memset(&template, 0, sizeof template);
	memset(&text, 0, sizeof text);
	memset(&blank, 0, sizeof blank);
	template.sx = gd->sx;
	template.sy = 1;
	template.linedata = &text;
	grid_empty_line(&template, 0, gc->bg);
	memcpy(&cell, gc, sizeof cell);
	utf8_set(&cell.data, ' ');
	grid_set_cells(&template, 0, 0, &cell, line, len);
	grid_compact_line(&text);
	template.linedata = &blank;
	grid_empty_line(&template, 0, gc->bg);
	grid_compact_line(&blank);

	drop = collected < old_size ? collected : old_size;
	if (drop != 0) {
		pool = xreallocarray(NULL, drop, sizeof *pool);
		memcpy(pool, gd->linedata, drop * sizeof *pool);
	}
	remaining = old_size - drop;
	memmove(gd->linedata, gd->linedata + drop,
	    remaining * sizeof *gd->linedata);
	size = hsize + gd->sy;
	grid_adjust_lines(gd, size);
	memset(gd->linedata + remaining, 0,
	    (size - remaining) * sizeof *gd->linedata);
	gd->hsize = hsize;
	gd->hscrolled = hscrolled;
	gd->scroll_added += lines;
	gd->scroll_collected += collected;

	/* Surviving original viewport rows have now entered the history. */
	first = old_hsize > drop ? old_hsize - drop : 0;
	for (i = first; i < remaining; i++) {
		grid_compact_line(&gd->linedata[i]);
		grid_line_set_time(&gd->linedata[i]);
	}
	pool_size = drop;
	for (i = remaining; i < size; i++) {
		old = pool_size != 0 ? &pool[--pool_size] : &empty;
		grid_reuse_line(&gd->linedata[i], i == size - 1 ? &blank : &text,
		    old);
		if (i < hsize)
			grid_line_set_time(&gd->linedata[i]);
	}
	for (i = 0; i < pool_size; i++) {
		xpool_free(pool[i].celldata);
		xpool_free(pool[i].extddata);
	}
	free(pool);
	xpool_free(text.celldata);
	xpool_free(text.extddata);
	xpool_free(blank.celldata);
	xpool_free(blank.extddata);
}

/* Clear the history. */
void
grid_clear_history(struct grid *gd)
{
	grid_trim_history(gd, gd->hsize);

	gd->hscrolled = 0;
	gd->hsize = 0;
	gd->scroll_generation++;

	grid_adjust_lines(gd, gd->sy);
}

/* Scroll a region up, moving the top line into the history. */
void
grid_scroll_history_region(struct grid *gd, u_int upper, u_int lower, u_int bg)
{
	struct grid_line	*gl_history, *gl_upper;
	u_int			 yy;

	/* Create a space for a new line. */
	yy = gd->hsize + gd->sy;
	grid_adjust_lines(gd, yy + 1);

	/* Move the entire screen down to free a space for this line. */
	gl_history = &gd->linedata[gd->hsize];
	memmove(gl_history + 1, gl_history, gd->sy * sizeof *gl_history);

	/* Adjust the region and find its start and end. */
	upper++;
	gl_upper = &gd->linedata[upper];
	lower++;

	/* Move the line into the history. */
	memcpy(gl_history, gl_upper, sizeof *gl_history);
	grid_line_set_time(gl_history);

	/* Then move the region up and clear the bottom line. */
	memmove(gl_upper, gl_upper + 1, (lower - upper) * sizeof *gl_upper);
	grid_empty_line(gd, lower, bg);

	/* Move the history offset down over the line. */
	gd->hscrolled++;
	gd->hsize++;
	gd->scroll_added++;
}

/* Initialize newly allocated columns with an extended background colour. */
static void
grid_fill_new_rgb(struct grid_cell_entry *restrict cells,
    struct grid_extd_entry *restrict entries, u_int offset, u_int n, u_int bg)
{
	struct grid_extd_entry	 clear;
	u_int			 i;
	uint64_t		 first, middle, last;

	clear.data = utf8_build_one(' ');
	clear.attr = 0;
	clear.flags = 0;
	clear.fg = 8;
	clear.bg = bg;
	clear.us = 8;
	clear.link = 0;
	/*
	 * Fixed-size copies let the compiler use wide stores without unaligned
	 * casts. The tail overlaps the middle to stay within the packed entry.
	 */
	if (sizeof clear >= 16 && sizeof clear <= 24) {
		memcpy(&first, &clear, sizeof first);
		memcpy(&middle, (const char *)&clear + 8, sizeof middle);
		memcpy(&last, (const char *)&clear + sizeof clear - 8, sizeof last);
		for (i = 0; i < n; i++, cells++, entries++) {
			cells->offset = offset + i;
			cells->flags = GRID_FLAG_CLEARED|GRID_FLAG_EXTENDED;
			memcpy(entries, &first, sizeof first);
			memcpy((char *)entries + 8, &middle, sizeof middle);
			memcpy((char *)entries + sizeof clear - 8, &last, sizeof last);
		}
	} else {
		for (i = 0; i < n; i++, cells++, entries++) {
			cells->offset = offset + i;
			cells->flags = GRID_FLAG_CLEARED|GRID_FLAG_EXTENDED;
			*entries = clear;
		}
	}
}

/* Apply the existing quarter/half/full row-allocation policy. */
static u_int
grid_line_size(struct grid *gd, u_int sx)
{
	if (sx < gd->sx / 4)
		return (gd->sx / 4);
	if (sx < gd->sx / 2)
		return (gd->sx / 2);
	if (gd->sx > sx)
		return (gd->sx);
	return (sx);
}

/* Expand line to fit to cell. */
static void
grid_expand_line(struct grid *gd, u_int py, u_int sx, u_int bg)
{
	struct grid_line	*gl;
	struct grid_cell_entry clear;
	u_int			 xx, space;

	gl = &gd->linedata[py];
	if (sx <= gl->cellsize)
		return;

	sx = grid_line_size(gd, sx);

	/* The spare owns only arrays; a new row keeps its own metadata. */
	if (gl->cellsize == 0 && gl->celldata == NULL &&
	    gl->extddata == NULL && gd->spare.celldata != NULL) {
		gl->celldata = gd->spare.celldata;
		gl->extddata = gd->spare.extddata;
		gl->extdallocated = gd->spare.extdallocated;
		if (gd->spare.cellsize != sx)
			gl->celldata = xpool_reallocarray(gl->celldata, sx,
			    sizeof *gl->celldata);
		memset(&gd->spare, 0, sizeof gd->spare);
	} else
		gl->celldata = xpool_reallocarray(gl->celldata, sx,
		    sizeof *gl->celldata);

	/* RGB background clears need one extended entry per new column. */
	if (bg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME)) {
		if (gl->extdsize > UINT_MAX - (sx - gl->cellsize))
			fatalx("too many extended cells");
		space = gl->extdsize + sx - gl->cellsize;
		if (space > gl->extdallocated) {
			gl->extddata = xpool_reallocarray(gl->extddata, space,
			    sizeof *gl->extddata);
			gl->extdallocated = space;
		}
		grid_fill_new_rgb(gl->celldata + gl->cellsize,
		    gl->extddata + gl->extdsize, gl->extdsize,
		    sx - gl->cellsize, bg);
		gl->extdsize += sx - gl->cellsize;
		gl->flags |= GRID_LINE_EXTENDED;
	} else {
		/* New columns have no extended storage to preserve. */
		clear = grid_cleared_entry;
		clear.data.bg = bg;
		if (bg & COLOUR_FLAG_256)
			clear.flags |= GRID_FLAG_BG256;
		for (xx = gl->cellsize; xx < sx; xx++)
			gl->celldata[xx] = clear;
	}
	gl->cellsize = sx;
}

/* Empty a line and set background colour if needed. */
void
grid_empty_line(struct grid *gd, u_int py, u_int bg)
{
	memset(&gd->linedata[py], 0, sizeof gd->linedata[py]);
	if (!COLOUR_DEFAULT(bg))
		grid_expand_line(gd, py, gd->sx, bg);
}

/* Peek at grid line. */
const struct grid_line *
grid_peek_line(struct grid *gd, u_int py)
{
	if (grid_check_y(gd, __func__, py) != 0)
		return (NULL);
	return (&gd->linedata[py]);
}

/* Get cell from line. */
static void
grid_get_cell1(struct grid_line *gl, u_int px, struct grid_cell *gc)
{
	struct grid_cell_entry	*gce = &gl->celldata[px];
	struct grid_extd_entry	*gee;

	if (gce->flags & GRID_FLAG_EXTENDED) {
		if (gce->offset >= gl->extdsize)
			memcpy(gc, &grid_default_cell, sizeof *gc);
		else {
			gee = &gl->extddata[gce->offset];
			gc->flags = gee->flags;
			gc->attr = gee->attr;
			gc->fg = gee->fg;
			gc->bg = gee->bg;
			gc->us = gee->us;
			gc->link = gee->link;

			if (gc->flags & GRID_FLAG_TAB)
				grid_set_tab(gc, gee->data);
			else
				utf8_to_data(gee->data, &gc->data);
		}
		return;
	}

	gc->flags = gce->flags & ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
	gc->attr = gce->data.attr;
	gc->fg = gce->data.fg;
	if (gce->flags & GRID_FLAG_FG256)
		gc->fg |= COLOUR_FLAG_256;
	gc->bg = gce->data.bg;
	if (gce->flags & GRID_FLAG_BG256)
		gc->bg |= COLOUR_FLAG_256;
	gc->us = 8;
	if (gc->flags & GRID_FLAG_PADDING)
		memcpy(&gc->data, &grid_padding_cell.data, sizeof gc->data);
	else
		utf8_set(&gc->data, gce->data.data);
	gc->link = 0;
}

/* Get cell for reading. */
void
grid_get_cell(struct grid *gd, u_int px, u_int py, struct grid_cell *gc)
{
	if (grid_check_y(gd, __func__, py) != 0 ||
	    px >= gd->linedata[py].cellsize)
		memcpy(gc, &grid_default_cell, sizeof *gc);
	else
		grid_get_cell1(&gd->linedata[py], px, gc);
}

/* Get flags without decoding the cell's character and attributes. */
u_char
grid_get_cell_flags(struct grid *gd, u_int px, u_int py)
{
	struct grid_line		*gl;
	struct grid_cell_entry	*gce;
	u_char			 flags;

	if (grid_check_y(gd, __func__, py) != 0)
		return (grid_default_cell.flags);
	gl = &gd->linedata[py];
	if (px >= gl->cellsize)
		return (grid_default_cell.flags);
	gce = &gl->celldata[px];
	if (~gce->flags & GRID_FLAG_EXTENDED)
		return (gce->flags & ~(GRID_FLAG_FG256|GRID_FLAG_BG256));
	if (gce->offset >= gl->extdsize)
		return (grid_default_cell.flags);
	flags = gl->extddata[gce->offset].flags;
	if (flags & GRID_FLAG_TAB)
		flags &= ~GRID_FLAG_PADDING;
	return (flags);
}

/* Set cell at position. */
void
grid_set_cell(struct grid *gd, u_int px, u_int py, const struct grid_cell *gc)
{
	struct grid_line	*gl;
	struct grid_cell_entry	*gce;

	if (grid_check_y(gd, __func__, py) != 0)
		return;

	grid_expand_line(gd, py, px + 1, 8);

	gl = &gd->linedata[py];
	if (px + 1 > gl->cellused)
		gl->cellused = px + 1;

	gce = &gl->celldata[px];
	if (grid_need_extended_cell(gce, gc))
		grid_extended_cell(gl, gce, gc);
	else
		grid_store_cell(gce, gc, gc->data.data[0]);
}

/* Set padding at position. */
void
grid_set_padding(struct grid *gd, u_int px, u_int py, int bg)
{
	struct grid_cell	gc;

	memcpy(&gc, &grid_padding_cell, sizeof gc);
	gc.bg = bg;
	grid_set_cell(gd, px, py, &gc);
}

/* Set cells at position. */
void
grid_set_cells(struct grid *gd, u_int px, u_int py, const struct grid_cell *gc,
    const char *s, size_t slen)
{
	struct grid_line	*gl;
	struct grid_cell_entry	*gce;
	struct grid_extd_entry	*gee, entry;
	struct grid_cell_entry	 empty = { 0 };
	u_int			 i;
	utf8_char		 ascii;

	if (grid_check_y(gd, __func__, py) != 0)
		return;

	grid_expand_line(gd, py, px + slen, 8);

	gl = &gd->linedata[py];
	if (px + slen > gl->cellused)
		gl->cellused = px + slen;

	/* Every character in this run has the same extended attributes. */
	if (slen != 0 && grid_need_extended_cell(&empty, gc)) {
		ascii = utf8_build_one(0);
		entry.attr = gc->attr;
		entry.flags = gc->flags & ~GRID_FLAG_CLEARED;
		entry.fg = gc->fg;
		entry.bg = gc->bg;
		entry.us = gc->us;
		entry.link = gc->link;
		for (i = 0; i < slen; i++) {
			gce = &gl->celldata[px + i];
			if (~gce->flags & GRID_FLAG_EXTENDED)
				grid_get_extended_cell(gl, gce, entry.flags);
			else if (gce->offset >= gl->extdsize)
				fatalx("offset too big");
			entry.data = ascii | (u_char)s[i];
			gl->extddata[gce->offset] = entry;
		}
		gl->flags |= GRID_LINE_EXTENDED;
		if (gc->link != 0)
			gl->flags |= GRID_LINE_HYPERLINK;
		return;
	}

	for (i = 0; i < slen; i++) {
		gce = &gl->celldata[px + i];
		if (gce->flags & GRID_FLAG_EXTENDED) {
			gee = grid_extended_cell(gl, gce, gc);
			gee->data = utf8_build_one(s[i]);
		} else
			grid_store_cell(gce, gc, s[i]);
	}
}

/* Store an ordinary UTF-8 run with a shared style and explicit wide padding. */
void
grid_set_utf8_cells(struct grid *gd, u_int px, u_int py,
    const struct grid_cell *gc, const utf8_char *data, u_int n,
    u_int width)
{
	struct grid_line		*gl;
	struct grid_cell_entry	*gce, compact_padding;
	struct grid_extd_entry	 entry, padding;
	struct grid_cell		 padding_cell;
	u_int			 i, x, need = 0, space, capacity, request = 0;
	int			 extended_padding;
	utf8_char		 padding_data;

	if (n == 0 || grid_check_y(gd, __func__, py) != 0)
		return;
	gl = &gd->linedata[py];
	/* Match scalar growth boundaries: an exact quarter/half may already
	 * fit a preceding allocation and must not expose extra cleared cells. */
	capacity = gl->cellsize;
	for (i = 0, x = px; i < n; i++) {
		x += UTF8_GET_WIDTH(data[i]);
		if (x > capacity) {
			request = x;
			capacity = grid_line_size(gd, x);
		}
	}
	if (request != 0)
		grid_expand_line(gd, py, request, 8);
	if (px + width > gl->cellused)
		gl->cellused = px + width;
	extended_padding = gc->bg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME);
	for (i = 0, x = px; i < n; i++) {
		if (~gl->celldata[x].flags & GRID_FLAG_EXTENDED)
			need++;
		if (UTF8_GET_WIDTH(data[i]) == 2 && extended_padding &&
		    (~gl->celldata[x + 1].flags & GRID_FLAG_EXTENDED))
			need++;
		x += UTF8_GET_WIDTH(data[i]);
	}
	if (need > UINT_MAX - gl->extdsize)
		fatalx("too many extended cells");
	space = gl->extdsize + need;
	if (space > gl->extdallocated) {
		gl->extddata = xpool_reallocarray(gl->extddata, space,
		    sizeof *gl->extddata);
		gl->extdallocated = space;
	}
	entry.attr = gc->attr;
	entry.flags = gc->flags & ~GRID_FLAG_CLEARED;
	entry.fg = gc->fg;
	entry.bg = gc->bg;
	entry.us = gc->us;
	entry.link = gc->link;
	memcpy(&padding_cell, &grid_padding_cell, sizeof padding_cell);
	padding_cell.bg = gc->bg;
	grid_store_cell(&compact_padding, &padding_cell, '!');
	utf8_from_data(&padding_cell.data, &padding_data);
	padding.data = padding_data;
	padding.attr = 0;
	padding.flags = GRID_FLAG_PADDING;
	padding.fg = 8;
	padding.bg = gc->bg;
	padding.us = 8;
	padding.link = 0;
	for (i = 0, x = px; i < n; i++) {
		if (UTF8_GET_WIDTH(data[i]) == 2) {
			gce = &gl->celldata[x + 1];
			if (extended_padding || (gce->flags & GRID_FLAG_EXTENDED)) {
				if (~gce->flags & GRID_FLAG_EXTENDED) {
					gce->offset = gl->extdsize++;
					gce->flags = GRID_FLAG_EXTENDED|GRID_FLAG_PADDING;
				} else if (gce->offset >= gl->extdsize)
					fatalx("offset too big");
				gl->extddata[gce->offset] = padding;
			} else
				*gce = compact_padding;
		}
		gce = &gl->celldata[x];
		if (~gce->flags & GRID_FLAG_EXTENDED) {
			gce->offset = gl->extdsize++;
			gce->flags = GRID_FLAG_EXTENDED|entry.flags;
		} else if (gce->offset >= gl->extdsize)
			fatalx("offset too big");
		entry.data = data[i];
		gl->extddata[gce->offset] = entry;
		x += UTF8_GET_WIDTH(data[i]);
	}
	gl->flags |= GRID_LINE_EXTENDED;
	if (gc->link != 0)
		gl->flags |= GRID_LINE_HYPERLINK;
}

/* Clear area. */
void
grid_clear(struct grid *gd, u_int px, u_int py, u_int nx, u_int ny, u_int bg)
{
	struct grid_line	*gl;
	u_int			 xx, yy, ox, sx;

	if (nx == 0 || ny == 0)
		return;

	if (px == 0 && nx == gd->sx) {
		grid_clear_lines(gd, py, ny, bg);
		return;
	}

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;
#ifdef ENABLE_IMAGES
	image_grid_overwrite(gd, px, py, nx, ny);
#endif

	for (yy = py; yy < py + ny; yy++) {
		gl = &gd->linedata[yy];

		sx = gd->sx;
		if (sx > gl->cellsize)
			sx = gl->cellsize;
		ox = nx;
		if (COLOUR_DEFAULT(bg)) {
			if (px > sx)
				continue;
			if (px + nx > sx)
				ox = sx - px;
		}

		grid_expand_line(gd, yy, px + ox, 8); /* default bg first */
		for (xx = px; xx < px + ox; xx++)
			grid_clear_cell(gd, xx, yy, bg, 0);
	}
}

/* Clear lines. This just frees and truncates the lines. */
void
grid_clear_lines(struct grid *gd, u_int py, u_int ny, u_int bg)
{
#ifdef ENABLE_IMAGES
	struct image_line	*images;
#endif
	u_int			 yy;

	if (ny == 0)
		return;

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;

	for (yy = py; yy < py + ny; yy++) {
#ifdef ENABLE_IMAGES
		image_grid_overwrite(gd, 0, yy, gd->sx, 1);
		images = gd->linedata[yy].images;
		gd->linedata[yy].images = NULL;
#endif
		grid_free_line(gd, yy);
		grid_empty_line(gd, yy, bg);
#ifdef ENABLE_IMAGES
		gd->linedata[yy].images = images;
#endif
	}
	if (py != 0)
		gd->linedata[py - 1].flags &= ~GRID_LINE_WRAPPED;
}

/* Move a group of lines. */
void
grid_move_lines(struct grid *gd, u_int dy, u_int py, u_int ny, u_int bg)
{
	u_int	yy;

	if (ny == 0 || py == dy)
		return;

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	if (grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;
	if (grid_check_y(gd, __func__, dy) != 0)
		return;
	if (grid_check_y(gd, __func__, dy + ny - 1) != 0)
		return;

	/* Free any lines which are being replaced. */
	for (yy = dy; yy < dy + ny; yy++) {
		if (yy >= py && yy < py + ny)
			continue;
		/* Retain at most one discarded row's arrays, with unique ownership. */
#ifdef ENABLE_IMAGES
		if (gd->images == NULL && gd->linedata[yy].cellsize != 0) {
#else
		if (gd->linedata[yy].cellsize != 0) {
#endif
			xpool_free(gd->spare.celldata);
			xpool_free(gd->spare.extddata);
			gd->spare = gd->linedata[yy];
			memset(&gd->linedata[yy], 0, sizeof gd->linedata[yy]);
		} else
			grid_free_line(gd, yy);
	}
	if (dy != 0)
		gd->linedata[dy - 1].flags &= ~GRID_LINE_WRAPPED;

	memmove(&gd->linedata[dy], &gd->linedata[py],
	    ny * (sizeof *gd->linedata));

	/*
	 * Wipe any lines that have been moved (without freeing them - they are
	 * still present).
	 */
	for (yy = py; yy < py + ny; yy++) {
		if (yy < dy || yy >= dy + ny)
			grid_empty_line(gd, yy, bg);
	}
	if (py != 0 && (py < dy || py >= dy + ny))
		gd->linedata[py - 1].flags &= ~GRID_LINE_WRAPPED;
}

/* Move a group of cells. */
void
grid_move_cells(struct grid *gd, u_int dx, u_int px, u_int py, u_int nx,
    u_int bg)
{
	struct grid_line	*gl;
	u_int			 xx;

	if (nx == 0 || px == dx)
		return;

	if (grid_check_y(gd, __func__, py) != 0)
		return;
	gl = &gd->linedata[py];

	grid_expand_line(gd, py, px + nx, 8);
	grid_expand_line(gd, py, dx + nx, 8);
#ifdef ENABLE_IMAGES
	image_grid_move_cells(gd, dx, px, py, nx);
#endif
	memmove(&gl->celldata[dx], &gl->celldata[px],
	    nx * sizeof *gl->celldata);
	if (dx + nx > gl->cellused)
		gl->cellused = dx + nx;

	/* Wipe any cells that have been moved. */
	for (xx = px; xx < px + nx; xx++) {
		if (xx >= dx && xx < dx + nx)
			continue;
		grid_clear_cell(gd, xx, py, bg, 1);
	}
}

/* Get ANSI foreground sequence. */
static size_t
grid_string_cells_fg(const struct grid_cell *gc, int *values)
{
	size_t	n;
	u_char	r, g, b;
	int	c;

	n = 0;
	if (gc->fg & COLOUR_FLAG_THEME) {
		c = colour_theme_terminal_colour(gc->fg & 0xff);
		if (c == 8)
			values[n++] = 39;
		else
			values[n++] = c + 30;
	} else if (gc->fg & COLOUR_FLAG_256) {
		values[n++] = 38;
		values[n++] = 5;
		values[n++] = gc->fg & 0xff;
	} else if (gc->fg & COLOUR_FLAG_RGB) {
		values[n++] = 38;
		values[n++] = 2;
		colour_split_rgb(gc->fg, &r, &g, &b);
		values[n++] = r;
		values[n++] = g;
		values[n++] = b;
	} else {
		switch (gc->fg) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			values[n++] = gc->fg + 30;
			break;
		case 8:
			values[n++] = 39;
			break;
		case 90:
		case 91:
		case 92:
		case 93:
		case 94:
		case 95:
		case 96:
		case 97:
			values[n++] = gc->fg;
			break;
		}
	}
	return (n);
}

/* Get ANSI background sequence. */
static size_t
grid_string_cells_bg(const struct grid_cell *gc, int *values)
{
	size_t	n;
	u_char	r, g, b;
	int	c;

	n = 0;
	if (gc->bg & COLOUR_FLAG_THEME) {
		c = colour_theme_terminal_colour(gc->bg & 0xff);
		if (c == 8)
			values[n++] = 49;
		else
			values[n++] = c + 40;
	} else if (gc->bg & COLOUR_FLAG_256) {
		values[n++] = 48;
		values[n++] = 5;
		values[n++] = gc->bg & 0xff;
	} else if (gc->bg & COLOUR_FLAG_RGB) {
		values[n++] = 48;
		values[n++] = 2;
		colour_split_rgb(gc->bg, &r, &g, &b);
		values[n++] = r;
		values[n++] = g;
		values[n++] = b;
	} else {
		switch (gc->bg) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 7:
			values[n++] = gc->bg + 40;
			break;
		case 8:
			values[n++] = 49;
			break;
		case 90:
		case 91:
		case 92:
		case 93:
		case 94:
		case 95:
		case 96:
		case 97:
			values[n++] = gc->bg + 10;
			break;
		}
	}
	return (n);
}

/* Get underscore colour sequence. */
static size_t
grid_string_cells_us(const struct grid_cell *gc, int *values)
{
	size_t	n;
	u_char	r, g, b;
	int	c;

	n = 0;
	if (gc->us & COLOUR_FLAG_THEME) {
		c = colour_theme_terminal_colour(gc->us & 0xff);
		if (c == 8)
			values[n++] = 59;
		else {
			values[n++] = 58;
			values[n++] = 5;
			values[n++] = c;
		}
	} else if (gc->us & COLOUR_FLAG_256) {
		values[n++] = 58;
		values[n++] = 5;
		values[n++] = gc->us & 0xff;
	} else if (gc->us & COLOUR_FLAG_RGB) {
		values[n++] = 58;
		values[n++] = 2;
		colour_split_rgb(gc->us, &r, &g, &b);
		values[n++] = r;
		values[n++] = g;
		values[n++] = b;
	}
	return (n);
}

/* Add on SGR code. */
static void
grid_string_cells_add_code(char *buf, size_t len, u_int n, int *s, int *newc,
    int *oldc, size_t nnewc, size_t noldc, int flags)
{
	u_int	i;
	char	tmp[64];
	int	reset = (n != 0 && s[0] == 0);

	if (nnewc == 0)
		return; /* no code to add */
	if (!reset &&
	    nnewc == noldc &&
	    memcmp(newc, oldc, nnewc * sizeof newc[0]) == 0)
		return; /* no reset and colour unchanged */
	if (reset && (newc[0] == 49 || newc[0] == 39))
		return; /* reset and colour default */

	if (flags & GRID_STRING_ESCAPE_SEQUENCES)
		strlcat(buf, "\\033[", len);
	else
		strlcat(buf, "\033[", len);
	for (i = 0; i < nnewc; i++) {
		if (i + 1 < nnewc)
			xsnprintf(tmp, sizeof tmp, "%d;", newc[i]);
		else
			xsnprintf(tmp, sizeof tmp, "%d", newc[i]);
		strlcat(buf, tmp, len);
	}
	strlcat(buf, "m", len);
}

static int
grid_string_cells_add_hyperlink(char *buf, size_t len, const char *id,
    const char *uri, int flags)
{
	char	*tmp;

	if (strlen(uri) + strlen(id) + 17 >= len)
		return (0);

	if (flags & GRID_STRING_ESCAPE_SEQUENCES)
		strlcat(buf, "\\033]8;", len);
	else
		strlcat(buf, "\033]8;", len);
	if (*id != '\0') {
		xasprintf(&tmp, "id=%s;", id);
		strlcat(buf, tmp, len);
		free(tmp);
	} else
		strlcat(buf, ";", len);
	strlcat(buf, uri, len);
	if (flags & GRID_STRING_ESCAPE_SEQUENCES)
		strlcat(buf, "\\033\\\\", len);
	else
		strlcat(buf, "\033\\", len);
	return (1);
}

/*
 * Returns ANSI code to set particular attributes (colour, bold and so on)
 * given a current state.
 */
static void
grid_string_cells_code(const struct grid_cell *lastgc,
    const struct grid_cell *gc, char *buf, size_t len, int flags,
    struct screen *sc, int *has_link)
{
	int			 oldc[64], newc[64], s[128];
	size_t			 noldc, nnewc, n, i;
	u_int			 attr = gc->attr, lastattr = lastgc->attr;
	char			 tmp[64];
	const char		*uri, *id;

	static const struct {
		u_int	mask;
		u_int	code;
	} attrs[] = {
		{ GRID_ATTR_BRIGHT, 1 },
		{ GRID_ATTR_DIM, 2 },
		{ GRID_ATTR_ITALICS, 3 },
		{ GRID_ATTR_UNDERSCORE, 4 },
		{ GRID_ATTR_BLINK, 5 },
		{ GRID_ATTR_REVERSE, 7 },
		{ GRID_ATTR_HIDDEN, 8 },
		{ GRID_ATTR_STRIKETHROUGH, 9 },
		{ GRID_ATTR_UNDERSCORE_2, 42 },
		{ GRID_ATTR_UNDERSCORE_3, 43 },
		{ GRID_ATTR_UNDERSCORE_4, 44 },
		{ GRID_ATTR_UNDERSCORE_5, 45 },
		{ GRID_ATTR_OVERLINE, 53 },
	};
	n = 0;

	/* If any attribute is removed, begin with 0. */
	for (i = 0; i < nitems(attrs); i++) {
		if (((~attr & attrs[i].mask) &&
		    (lastattr & attrs[i].mask)) ||
		    (lastgc->us != 8 && gc->us == 8)) {
			s[n++] = 0;
			lastattr &= GRID_ATTR_CHARSET;
			break;
		}
	}
	/* For each attribute that is newly set, add its code. */
	for (i = 0; i < nitems(attrs); i++) {
		if ((attr & attrs[i].mask) && !(lastattr & attrs[i].mask))
			s[n++] = attrs[i].code;
	}

	/* Write the attributes. */
	*buf = '\0';
	if (n > 0) {
		if (flags & GRID_STRING_ESCAPE_SEQUENCES)
			strlcat(buf, "\\033[", len);
		else
			strlcat(buf, "\033[", len);
		for (i = 0; i < n; i++) {
			if (s[i] < 10)
				xsnprintf(tmp, sizeof tmp, "%d", s[i]);
			else {
				xsnprintf(tmp, sizeof tmp, "%d:%d", s[i] / 10,
				    s[i] % 10);
			}
			strlcat(buf, tmp, len);
			if (i + 1 < n)
				strlcat(buf, ";", len);
		}
		strlcat(buf, "m", len);
	}

	/* If the foreground colour changed, write its parameters. */
	nnewc = grid_string_cells_fg(gc, newc);
	noldc = grid_string_cells_fg(lastgc, oldc);
	grid_string_cells_add_code(buf, len, n, s, newc, oldc, nnewc, noldc,
	    flags);

	/* If the background colour changed, append its parameters. */
	nnewc = grid_string_cells_bg(gc, newc);
	noldc = grid_string_cells_bg(lastgc, oldc);
	grid_string_cells_add_code(buf, len, n, s, newc, oldc, nnewc, noldc,
	    flags);

	/* If the underscore colour changed, append its parameters. */
	nnewc = grid_string_cells_us(gc, newc);
	noldc = grid_string_cells_us(lastgc, oldc);
	grid_string_cells_add_code(buf, len, n, s, newc, oldc, nnewc, noldc,
	    flags);

	/* Append shift in/shift out if needed. */
	if ((attr & GRID_ATTR_CHARSET) && !(lastattr & GRID_ATTR_CHARSET)) {
		if (flags & GRID_STRING_ESCAPE_SEQUENCES)
			strlcat(buf, "\\016", len); /* SO */
		else
			strlcat(buf, "\016", len);  /* SO */
	}
	if (!(attr & GRID_ATTR_CHARSET) && (lastattr & GRID_ATTR_CHARSET)) {
		if (flags & GRID_STRING_ESCAPE_SEQUENCES)
			strlcat(buf, "\\017", len); /* SI */
		else
			strlcat(buf, "\017", len);  /* SI */
	}

	/* Add hyperlink if changed. */
	if (sc != NULL && sc->hyperlinks != NULL && lastgc->link != gc->link) {
		if (hyperlinks_get(sc->hyperlinks, gc->link, &uri, &id, NULL)) {
			*has_link = grid_string_cells_add_hyperlink(buf, len,
			    id, uri, flags);
		} else if (*has_link) {
			grid_string_cells_add_hyperlink(buf, len, "", "",
			    flags);
			*has_link = 0;
		}
	}
}

/* Convert cells into a string. */
char *
grid_string_cells(struct grid *gd, u_int px, u_int py, u_int nx,
    struct grid_cell **lastgc, int flags, struct screen *s)
{
	struct grid_cell	 gc;
	static struct grid_cell	 lastgc1;
	const char		*data;
	char			*buf, code[8192];
	size_t			 len, off, size, codelen;
	u_int			 xx, end;
	int			 has_link = 0;
	const struct grid_line	*gl;

	if (lastgc != NULL && *lastgc == NULL) {
		memcpy(&lastgc1, &grid_default_cell, sizeof lastgc1);
		*lastgc = &lastgc1;
	}

	len = 128;
	buf = xmalloc(len);
	off = 0;

	gl = grid_peek_line(gd, py);
	if (gl == NULL) {
		buf[0] = '\0';
		return (buf);
	}
	if (flags & GRID_STRING_EMPTY_CELLS)
		end = gl->cellsize;
	else
		end = gl->cellused;
	for (xx = px; xx < px + nx; xx++) {
		if (xx >= end)
			break;
		grid_get_cell(gd, xx, py, &gc);
		if (gc.flags & GRID_FLAG_PADDING)
			continue;

		if (lastgc != NULL && (flags & GRID_STRING_WITH_SEQUENCES)) {
			grid_string_cells_code(*lastgc, &gc, code, sizeof code,
			    flags, s, &has_link);
			codelen = strlen(code);
			memcpy(*lastgc, &gc, sizeof **lastgc);
		} else
			codelen = 0;

		if (gc.flags & GRID_FLAG_TAB) {
			data = "\t";
			size = 1;
		} else {
			data = gc.data.data;
			size = gc.data.size;
			if ((flags & GRID_STRING_ESCAPE_SEQUENCES) &&
			    size == 1 &&
			    *data == '\\') {
				data = "\\\\";
				size = 2;
			}
		}

		while (len < off + size + codelen + 1) {
			buf = xreallocarray(buf, 2, len);
			len *= 2;
		}

		if (codelen != 0) {
			memcpy(buf + off, code, codelen);
			off += codelen;
		}
		memcpy(buf + off, data, size);
		off += size;
	}

	if (has_link) {
		grid_string_cells_add_hyperlink(code, sizeof code, "", "",
		    flags);
		codelen = strlen(code);
		while (len < off + size + codelen + 1) {
			buf = xreallocarray(buf, 2, len);
			len *= 2;
		}
		memcpy(buf + off, code, codelen);
		off += codelen;
	}

	if (flags & GRID_STRING_TRIM_SPACES) {
		while (off > 0 && buf[off - 1] == ' ')
			off--;
	}
	buf[off] = '\0';

	return (buf);
}

/*
 * Duplicate a set of lines between two grids. Both source and destination
 * should be big enough.
 */
void
grid_duplicate_lines(struct grid *dst, u_int dy, struct grid *src, u_int sy,
    u_int ny)
{
	struct grid_line	*dstl, *srcl;
	u_int			 yy;
#ifdef ENABLE_IMAGES
	u_int			 ody = dy, osy = sy;
#endif

	if (dy + ny > dst->hsize + dst->sy)
		ny = dst->hsize + dst->sy - dy;
	if (sy + ny > src->hsize + src->sy)
		ny = src->hsize + src->sy - sy;
	grid_free_lines(dst, dy, ny);

	for (yy = 0; yy < ny; yy++) {
		srcl = &src->linedata[sy];
		dstl = &dst->linedata[dy];

		memcpy(dstl, srcl, sizeof *dstl);
#ifdef ENABLE_IMAGES
		dstl->images = NULL;
#endif
		if (srcl->cellsize != 0) {
			dstl->celldata = xpool_reallocarray(NULL,
			    srcl->cellsize, sizeof *dstl->celldata);
			memcpy(dstl->celldata, srcl->celldata,
			    srcl->cellsize * sizeof *dstl->celldata);
		} else
			dstl->celldata = NULL;
		if (srcl->extdsize != 0) {
			dstl->extdsize = srcl->extdsize;
			dstl->extddata = xpool_reallocarray(NULL, dstl->extdsize,
			    sizeof *dstl->extddata);
			memcpy(dstl->extddata, srcl->extddata, dstl->extdsize *
			    sizeof *dstl->extddata);
		} else
			dstl->extddata = NULL;
		dstl->extdallocated = dstl->extdsize;

		sy++;
		dy++;
	}
#ifdef ENABLE_IMAGES
	image_grid_duplicate_lines(dst, ody, src, osy, ny);
#endif
}

/* Mark line as dead. */
static void
grid_reflow_dead(struct grid_line *gl)
{
	memset(gl, 0, sizeof *gl);
	gl->flags = GRID_LINE_DEAD;
}

/* Add lines, return the first new one. */
static struct grid_line *
grid_reflow_add(struct grid *gd, u_int n)
{
	struct grid_line	*gl;
	u_int			 sy = gd->sy + n;

	grid_adjust_lines(gd, sy);
	gl = &gd->linedata[gd->sy];
	memset(gl, 0, n * (sizeof *gl));
	gd->sy = sy;
	return (gl);
}

/* Move a line across. */
static struct grid_line *
grid_reflow_move(struct grid *gd, struct grid_line *from)
{
	struct grid_line	*to;

	to = grid_reflow_add(gd, 1);
	memcpy(to, from, sizeof *to);
	grid_reflow_dead(from);
	return (to);
}

/* Move the OSC 133 markers from one line to another. */
static void
grid_reflow_move_osc133(struct grid_line *to, struct grid_line *from,
    u_int to_col, u_int count, int last)
{
	struct osc133_data	*src = &from->osc133_data;
	struct osc133_data	*dst = &to->osc133_data;
	u_short			*src_col[] = {
		&src->prompt_col,
		&src->cmd_col,
		&src->out_start_col,
		&src->out_end_col
	};
	u_short			*dst_col[] = {
		&dst->prompt_col,
		&dst->cmd_col,
		&dst->out_start_col,
		&dst->out_end_col
	};
	u_int			 flags[] = {
		GRID_LINE_START_PROMPT|GRID_LINE_SECOND_PROMPT,
		GRID_LINE_START_COMMAND,
		GRID_LINE_START_OUTPUT,
		GRID_LINE_END_OUTPUT
	};
	u_int			 i, flag;

	if ((from->flags & GRID_LINE_OSC133_FLAGS) == 0)
		return;
	for (i = 0; i < nitems(flags); i++) {
		flag = from->flags & flags[i];
		if (flag == 0)
			continue;
		if (!last && *src_col[i] >= count) {
			*src_col[i] -= count;
			continue;
		}
		*dst_col[i] = to_col + *src_col[i];
		to->flags |= flag;
		from->flags &= ~flag;
		*src_col[i] = 0;
		if (flag == GRID_LINE_END_OUTPUT) {
			dst->exit_status = src->exit_status;
			src->exit_status = 0;
		}
	}
}

/* Join line below onto this one. */
static void
grid_reflow_join(struct grid *target, struct grid *gd, u_int sx, u_int yy,
    u_int width, int already)
{
	struct grid_line	*gl, *from = NULL, *next;
	struct grid_cell	 gc;
	u_int			 lines, left, i, to, line, want = 0;
	u_int			 at;
	int			 wrapped = 1;

	/*
	 * Add a new target line.
	 */
	if (!already) {
		to = target->sy;
		gl = grid_reflow_move(target, &gd->linedata[yy]);
	} else {
		to = target->sy - 1;
		gl = &target->linedata[to];
	}
	at = gl->cellused;

	/*
	 * Loop until no more to consume or the target line is full.
	 */
	lines = 0;
	for (;;) {
		/*
		 * If this is now the last line, there is nothing more to be
		 * done.
		 */
		if (yy + 1 + lines == gd->hsize + gd->sy)
			break;
		line = yy + 1 + lines;

#ifdef ENABLE_IMAGES
		if (image_grid_line_has_images(&gd->linedata[line]))
			break;
#endif

		/* Consume empty lines, including any markers. */
		if (~gd->linedata[line].flags & GRID_LINE_WRAPPED)
			wrapped = 0;
		if (gd->linedata[line].cellused == 0) {
			next = &gd->linedata[line];
			if (!wrapped &&
			    (next->flags & GRID_LINE_OSC133_FLAGS) == 0)
				break;
			from = next;
			want = 0;
			grid_reflow_move_osc133(gl, from, at, 0, 1);
			lines++;
			if (!wrapped)
				break;
			continue;
		}

		/*
		 * Is the destination line now full? Copy the first character
		 * separately because we need to leave "from" set to the last
		 * line if this line is full.
		 */
		grid_get_cell1(&gd->linedata[line], 0, &gc);
		if (width + gc.data.width > sx)
			break;
		width += gc.data.width;
		grid_set_cell(target, at, to, &gc);
		at++;

		/* Join as much more as possible onto the current line. */
		from = &gd->linedata[line];
		for (want = 1; want < from->cellused; want++) {
			grid_get_cell1(from, want, &gc);
			if (width + gc.data.width > sx)
				break;
			width += gc.data.width;

			grid_set_cell(target, at, to, &gc);
			at++;
		}
		grid_reflow_move_osc133(gl, from, at - want, want,
		    want == from->cellused);
		lines++;

		/*
		 * If this line wasn't wrapped or we didn't consume the entire
		 * line, don't try to join any further lines.
		 */
		if (!wrapped || want != from->cellused || width == sx)
			break;
	}
	if (lines == 0 || from == NULL)
		return;

	/*
	 * If we didn't consume the entire final line, then remove what we did
	 * consume. If we consumed the entire line and it wasn't wrapped,
	 * remove the wrap flag from this line.
	 */
	left = from->cellused - want;
	if (left != 0) {
		grid_move_cells(gd, 0, want, yy + lines, left, 8);
		from->cellsize = from->cellused = left;
		lines--;
	} else if (!wrapped)
		gl->flags &= ~GRID_LINE_WRAPPED;

	/* Remove the lines that were completely consumed. */
	for (i = yy + 1; i < yy + 1 + lines; i++) {
		grid_free_line(gd, i);
		grid_reflow_dead(&gd->linedata[i]);
	}

	/* Adjust scroll position. */
	if (gd->hscrolled > to + lines)
		gd->hscrolled -= lines;
	else if (gd->hscrolled > to)
		gd->hscrolled = to;
}

/* Split this line into several new ones */
static void
grid_reflow_split(struct grid *target, struct grid *gd, u_int sx, u_int yy,
    u_int at)
{
	struct grid_line	*gl = &gd->linedata[yy], *first;
	struct grid_cell	 gc;
	u_int			 line, lines, width, i, xx;
	u_int			 used = gl->cellused;
	int			 flags = gl->flags;

	/* How many lines do we need to insert? We know we need at least two. */
	if (~gl->flags & GRID_LINE_EXTENDED)
		lines = 1 + (gl->cellused - 1) / sx;
	else {
		lines = 2;
		width = 0;
		for (i = at; i < used; i++) {
			grid_get_cell1(gl, i, &gc);
			if (width + gc.data.width > sx) {
				lines++;
				width = 0;
			}
			width += gc.data.width;
		}
	}

	/* Insert new lines. */
	line = target->sy + 1;
	first = grid_reflow_add(target, lines);

	/* The first line keeps only the markers within its first at cells. */
	memcpy(first, gl, sizeof *first);
	first->cellsize = first->cellused = at;
	first->flags &= ~GRID_LINE_OSC133_FLAGS;
	first->flags |= GRID_LINE_WRAPPED;
	memset(&first->osc133_data, 0, sizeof first->osc133_data);
	grid_reflow_move_osc133(first, gl, 0, at, 0);

	/* Copy sections from the original line. */
	width = 0;
	xx = 0;
	for (i = at; i < used; i++) {
		grid_get_cell1(gl, i, &gc);
		if (width + gc.data.width > sx) {
			target->linedata[line].flags |= GRID_LINE_WRAPPED;
			grid_reflow_move_osc133(&target->linedata[line], gl, 0,
			    xx, 0);

			line++;
			width = 0;
			xx = 0;
		}
		width += gc.data.width;
		grid_set_cell(target, xx, line, &gc);
		xx++;
	}
	if (flags & GRID_LINE_WRAPPED)
		target->linedata[line].flags |= GRID_LINE_WRAPPED;
	grid_reflow_move_osc133(&target->linedata[line], gl, 0, xx, 1);
	grid_reflow_dead(gl);

	/* Adjust the scroll position. */
	if (yy <= gd->hscrolled)
		gd->hscrolled += lines - 1;

	/*
	 * If the original line had the wrapped flag and there is still space
	 * in the last new line, try to join with the next lines.
	 */
	if (width < sx && (flags & GRID_LINE_WRAPPED))
		grid_reflow_join(target, gd, sx, yy, width, 1);
}

/* Reflow lines on grid to new width. */
void
grid_reflow(struct grid *gd, u_int sx)
{
	struct grid		*target;
	struct grid_line	*gl;
	struct grid_cell	 gc;
	u_int			 yy, width, i, at;

	/*
	 * Create a destination grid. This is just used as a container for the
	 * line data and may not be fully valid.
	 */
	target = grid_create(gd->sx, 0, 0);

	/*
	 * Loop over each source line.
	 */
	for (yy = 0; yy < gd->hsize + gd->sy; yy++) {
		gl = &gd->linedata[yy];
		if (gl->flags & GRID_LINE_DEAD)
			continue;

#ifdef ENABLE_IMAGES
		if (image_grid_line_has_images(gl)) {
			grid_reflow_move(target, gl);
			continue;
		}
#endif

		/*
		 * Work out the width of this line. at is the point at which
		 * the available width is hit, and width is the full line
		 * width.
		 */
		at = width = 0;
		if (~gl->flags & GRID_LINE_EXTENDED) {
			width = gl->cellused;
			if (width > sx)
				at = sx;
			else
				at = width;
		} else {
			for (i = 0; i < gl->cellused; i++) {
				grid_get_cell1(gl, i, &gc);
				if (at == 0 && width + gc.data.width > sx)
					at = i;
				width += gc.data.width;
			}
		}

		/*
		 * If the line is exactly right, just move it across
		 * unchanged.
		 */
		if (width == sx) {
			grid_reflow_move(target, gl);
			continue;
		}

		/*
		 * If the line is too big, it needs to be split, whether or not
		 * it was previously wrapped.
		 */
		if (width > sx) {
			grid_reflow_split(target, gd, sx, yy, at);
			continue;
		}

		/*
		 * If the line was previously wrapped, join as much as possible
		 * of the next line.
		 */
		if (gl->flags & GRID_LINE_WRAPPED)
			grid_reflow_join(target, gd, sx, yy, width, 0);
		else
			grid_reflow_move(target, gl);
	}

	/*
	 * Replace the old grid with the new.
	 */
	if (target->sy < gd->sy)
		grid_reflow_add(target, gd->sy - target->sy);
	gd->hsize = target->sy - gd->sy;
	if (gd->hscrolled > gd->hsize)
		gd->hscrolled = gd->hsize;
	free(gd->linedata);
	gd->linedata = target->linedata;
	gd->lineallocated = target->lineallocated;
	free(target);
	gd->scroll_generation++;
}

/* Convert to position based on wrapped lines. */
void
grid_wrap_position(struct grid *gd, u_int px, u_int py, u_int *wx, u_int *wy)
{
	u_int	ax = 0, ay = 0, yy;

	for (yy = 0; yy < py; yy++) {
		if (gd->linedata[yy].flags & GRID_LINE_WRAPPED)
			ax += gd->linedata[yy].cellused;
		else {
			ax = 0;
			ay++;
		}
	}
	if (px >= gd->linedata[yy].cellused)
		ax = UINT_MAX;
	else
		ax += px;
	*wx = ax;
	*wy = ay;
}

/* Convert position based on wrapped lines back. */
void
grid_unwrap_position(struct grid *gd, u_int *px, u_int *py, u_int wx, u_int wy)
{
	u_int	yy, ay = 0, ey = gd->hsize + gd->sy - 1;

	for (yy = 0; yy < gd->hsize + gd->sy - 1; yy++) {
		if (ay == wy)
			break;
		if (~gd->linedata[yy].flags & GRID_LINE_WRAPPED)
			ay++;
	}

	/*
	 * yy is now 0 on the unwrapped line which contains wx. Walk forwards
	 * until we find the end or the line now containing wx.
	 */
	if (wx == UINT_MAX) {
		while (yy < ey && gd->linedata[yy].flags & GRID_LINE_WRAPPED)
			yy++;
		wx = gd->linedata[yy].cellused;
	} else {
		while (gd->linedata[yy].flags & GRID_LINE_WRAPPED) {
			if (wx < gd->linedata[yy].cellused)
				break;
			wx -= gd->linedata[yy].cellused;
			yy++;
		}
	}
	*px = wx;
	*py = yy;
}

/* Get length of line. */
u_int
grid_line_length(struct grid *gd, u_int py)
{
	struct grid_cell	gc;
	u_int			px;

	px = grid_get_line(gd, py)->cellsize;
	if (px > gd->sx)
		px = gd->sx;
	while (px > 0) {
		grid_get_cell(gd, px - 1, py, &gc);
		if ((gc.flags & GRID_FLAG_PADDING) ||
		    gc.data.size != 1 ||
		    *gc.data.data != ' ')
			break;
		px--;
	}
	return (px);
}

/* Get last position on line, not including padding. */
u_int
grid_line_limit(struct grid *gd, u_int py)
{
	struct grid_cell	gc;
	u_int			px;

	px = grid_line_length(gd, py);
	if (px == 0)
		return (0);
	px--;
	while (px > 0) {
		grid_get_cell(gd, px, py, &gc);
		if (~gc.flags & GRID_FLAG_PADDING)
			break;
		px--;
	}
	return (px);
}

/* Check if character is in set. */
int
grid_in_set(struct grid *gd, u_int px, u_int py, const char *set)
{
	struct grid_cell	gc, tmp_gc;
	u_int			pxx;
	int			has_tab, has_space;

	has_tab = (strchr(set, '\t') != NULL);
	has_space = (strchr(set, ' ') != NULL);

	grid_get_cell(gd, px, py, &gc);
	if (gc.flags & GRID_FLAG_PADDING) {
		if (!has_tab && !has_space)
			return (0);
		pxx = px;
		do
			grid_get_cell(gd, --pxx, py, &tmp_gc);
		while (pxx > 0 && tmp_gc.flags & GRID_FLAG_PADDING);
		if (((has_tab || has_space) &&
		    (tmp_gc.flags & GRID_FLAG_TAB)) ||
		    (has_space && utf8_has_whitespace(&tmp_gc.data)))
			return (tmp_gc.data.width - (px - pxx));
		return (0);
	}
	if ((has_tab || has_space) && (gc.flags & GRID_FLAG_TAB))
		return (gc.data.width);
	if (has_space && utf8_has_whitespace(&gc.data))
		return (gc.data.width == 0 ? 1 : gc.data.width);
	return (utf8_cstrhas(set, &gc.data));
}

/* Line flags to string. */
const char *
grid_line_flags_string(int flags)
{
	static char	s[128];

	*s = '\0';
	if (flags & GRID_LINE_WRAPPED)
		strlcat(s, "WRAPPED,", sizeof s);
	if (flags & GRID_LINE_EXTENDED)
		strlcat(s, "EXTENDED,", sizeof s);
	if (flags & GRID_LINE_DEAD)
		strlcat(s, "DEAD,", sizeof s);
	if (flags & GRID_LINE_START_PROMPT)
		strlcat(s, "START_PROMPT,", sizeof s);
	if (flags & GRID_LINE_SECOND_PROMPT)
		strlcat(s, "SECOND_PROMPT,", sizeof s);
	if (flags & GRID_LINE_START_COMMAND)
		strlcat(s, "START_COMMAND,", sizeof s);
	if (flags & GRID_LINE_START_OUTPUT)
		strlcat(s, "START_OUTPUT,", sizeof s);
	if (flags & GRID_LINE_END_OUTPUT)
		strlcat(s, "END_OUTPUT,", sizeof s);
	if (flags & GRID_LINE_HYPERLINK)
		strlcat(s, "HYPERLINK,", sizeof s);
	if (*s == '\0')
		return ("NONE");
	s[strlen(s) - 1] = '\0';
	return (s);
}

/* Cell flags to string. */
const char *
grid_cell_flags_string(int flags)
{
	static char	s[128];

	*s = '\0';
	if (flags & GRID_FLAG_FG256)
		strlcat(s, "FG256,", sizeof s);
	if (flags & GRID_FLAG_BG256)
		strlcat(s, "BG256,", sizeof s);
	if (flags & GRID_FLAG_PADDING)
		strlcat(s, "PADDING,", sizeof s);
	if (flags & GRID_FLAG_EXTENDED)
		strlcat(s, "EXTENDED,", sizeof s);
	if (flags & GRID_FLAG_SELECTED)
		strlcat(s, "SELECTED,", sizeof s);
	if (flags & GRID_FLAG_CLEARED)
		strlcat(s, "CLEARED,", sizeof s);
	if (flags & GRID_FLAG_TAB)
		strlcat(s, "TAB,", sizeof s);
	if (flags & GRID_FLAG_NOPALETTE)
		strlcat(s, "NOPALETTE,", sizeof s);
	if (*s == '\0')
		return ("NONE");
	s[strlen(s) - 1] = '\0';
	return (s);
}

/* Cell attributes to string. */
const char *
grid_cell_attr_string(int attr)
{
	static char	s[256];

	*s = '\0';
	if (attr & GRID_ATTR_CHARSET)
		strlcat(s, "CHARSET,", sizeof s);
	if (attr & GRID_ATTR_BRIGHT)
		strlcat(s, "BRIGHT,", sizeof s);
	if (attr & GRID_ATTR_DIM)
		strlcat(s, "DIM,", sizeof s);
	if (attr & GRID_ATTR_UNDERSCORE)
		strlcat(s, "UNDERSCORE,", sizeof s);
	if (attr & GRID_ATTR_BLINK)
		strlcat(s, "BLINK,", sizeof s);
	if (attr & GRID_ATTR_REVERSE)
		strlcat(s, "REVERSE,", sizeof s);
	if (attr & GRID_ATTR_HIDDEN)
		strlcat(s, "HIDDEN,", sizeof s);
	if (attr & GRID_ATTR_ITALICS)
		strlcat(s, "ITALICS,", sizeof s);
	if (attr & GRID_ATTR_STRIKETHROUGH)
		strlcat(s, "STRIKETHROUGH,", sizeof s);
	if (attr & GRID_ATTR_UNDERSCORE_2)
		strlcat(s, "UNDERSCORE_2,", sizeof s);
	if (attr & GRID_ATTR_UNDERSCORE_3)
		strlcat(s, "UNDERSCORE_3,", sizeof s);
	if (attr & GRID_ATTR_UNDERSCORE_4)
		strlcat(s, "UNDERSCORE_4,", sizeof s);
	if (attr & GRID_ATTR_UNDERSCORE_5)
		strlcat(s, "UNDERSCORE_5,", sizeof s);
	if (attr & GRID_ATTR_OVERLINE)
		strlcat(s, "OVERLINE,", sizeof s);
	if (*s == '\0')
		return ("NONE");
	s[strlen(s) - 1] = '\0';
	return (s);
}
