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
		gl->extddata = xpool_reallocarray_private(gl->extddata, space,
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

__thread struct grid_job_metrics *grid_job_metrics;

static void grid_prepare_line(struct grid_line *, u_char);
static void grid_release_payload(void *, int);

/* Free up unused extended cells. */
static void
grid_compact_line(struct grid_line *gl)
{
	u_int			 new_extdsize = 0;
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

	if (new_extdsize == gl->extdsize) {
		for (px = 0, idx = 0; px < gl->cellsize; px++) {
			gce = &gl->celldata[px];
			if ((gce->flags & GRID_FLAG_EXTENDED) && gce->offset != idx++)
				break;
		}
		if (px == gl->cellsize && gl->extdallocated == gl->extdsize)
			return;
	}
	if (new_extdsize == 0) {
		grid_release_payload(gl->extddata, gl->shared & GRID_SHARE_EXTD);
		gl->shared &= ~GRID_SHARE_EXTD;
		gl->extddata = NULL;
		gl->extdsize = 0;
		gl->extdallocated = 0;
		return;
	}
	new_extddata = xpool_reallocarray_private(NULL, new_extdsize, sizeof *gl->extddata);

	grid_prepare_line(gl, GRID_SHARE_CELLS);
	idx = 0;
	for (px = 0; px < gl->cellsize; px++) {
		gce = &gl->celldata[px];
		if (gce->flags & GRID_FLAG_EXTENDED) {
			gee = &gl->extddata[gce->offset];
			memcpy(&new_extddata[idx], gee, sizeof *gee);
			gce->offset = idx++;
		}
	}

	grid_release_payload(gl->extddata, gl->shared & GRID_SHARE_EXTD);
	gl->shared &= ~GRID_SHARE_EXTD;
	gl->extddata = new_extddata;
	gl->extdsize = new_extdsize;
	gl->extdallocated = new_extdsize;
}

/* Reads use absolute coordinates; mutable row headers are viewport-only. */
const struct grid_line *
grid_get_line(struct grid *gd, u_int y)
{
	if (y < gd->hsize)
		return (grid_rows_get(gd, y));
	return (&gd->viewport[y - gd->hsize]);
}

const struct grid_line *
grid_read_line(const struct grid *gd, struct grid_row_cursor *cursor, u_int y)
{
	if (y < gd->hsize)
		return (grid_rows_read(gd, cursor, y));
	return (&gd->viewport[y - gd->hsize]);
}

struct grid_line *
grid_edit_line_record(struct grid *gd, u_int y)
{
	if (y < gd->hsize || y - gd->hsize >= gd->sy)
		fatalx("mutable row outside viewport");
	return (&gd->viewport[y - gd->hsize]);
}

void
grid_clear_wrapped(struct grid *gd, u_int y)
{
	if (y >= gd->hsize)
		gd->viewport[y - gd->hsize].flags &= ~GRID_LINE_WRAPPED;
	else if (y == gd->hsize - 1)
		gd->row_clear_wrap = 1;
}

static void
grid_release_payload(void *buffer, int shared)
{
	if (shared)
		xpool_release(buffer);
	else
		xpool_free_private(buffer);
}

static void
grid_release_line(struct grid_line *gl)
{
	grid_release_payload(gl->celldata, gl->shared & GRID_SHARE_CELLS);
	grid_release_payload(gl->extddata, gl->shared & GRID_SHARE_EXTD);
	memset(gl, 0, sizeof *gl);
}

/* The source stays pinned until all of these retains have completed. */
static void
grid_retain_line(struct grid_line *dst, const struct grid_line *src)
{
	*dst = *src;
	dst->shared = 0;
	dst->job_flags = 0;
	if (dst->celldata != NULL) {
		xpool_retain(dst->celldata);
		dst->shared |= GRID_SHARE_CELLS;
	}
	if (dst->extddata != NULL) {
		xpool_retain(dst->extddata);
		dst->shared |= GRID_SHARE_EXTD;
	}
}

static void *
grid_acquire_payload(void *buffer, size_t used, size_t capacity,
    uint64_t *copied)
{
	void *copy;

	if (xpool_unique(buffer)) {
		if (grid_job_metrics != NULL)
			grid_job_metrics->promotions++;
		return (buffer);
	}
	*copied += used;
	if (capacity == 0) {
		xpool_release(buffer);
		return (NULL);
	}
	copy = xpool_reallocarray_private(NULL, capacity, 1);
	memcpy(copy, buffer, used);
	xpool_release(buffer);
	return (copy);
}

static void
grid_prepare_line(struct grid_line *gl, u_char mask)
{
	uint64_t packed = 0, extended = 0;

	mask &= gl->shared;
	if (mask == 0)
		return;
	if (mask & GRID_SHARE_CELLS) {
		gl->celldata = grid_acquire_payload(gl->celldata,
		    gl->cellsize * sizeof *gl->celldata,
		    gl->cellsize * sizeof *gl->celldata, &packed);
	}
	if (mask & GRID_SHARE_EXTD) {
		gl->extddata = grid_acquire_payload(gl->extddata,
		    gl->extdsize * sizeof *gl->extddata,
		    gl->extdallocated * sizeof *gl->extddata, &extended);
	}
	gl->shared &= ~mask;
	if (grid_job_metrics != NULL) {
		grid_job_metrics->probes += (mask == (GRID_SHARE_CELLS | GRID_SHARE_EXTD)) ? 2 : 1;
		if (!(gl->job_flags & 1)) {
			gl->job_flags |= 1;
			grid_job_metrics->touched++;
		}
		if ((packed != 0 || extended != 0) && !(gl->job_flags & 2)) {
			gl->job_flags |= 2;
			grid_job_metrics->copied_rows++;
		}
		grid_job_metrics->packed_bytes += packed;
		grid_job_metrics->extended_bytes += extended;
	}
}

static void
grid_viewport_allocate(struct grid *gd, u_int sy)
{
	gd->viewport_space = sy <= UINT_MAX / 2 ? sy * 2 : sy;
	gd->viewport_base = sy == 0 ? NULL :
	    xcalloc(gd->viewport_space, sizeof *gd->viewport_base);
	gd->viewport = gd->viewport_base;
}

/* Share scrollback by root and give each grid independent live headers. */
void
grid_share_lines(struct grid *dst, struct grid *src)
{
	u_int y;

	grid_free_lines(dst, dst->hsize, dst->sy);
#ifdef ENABLE_IMAGES
	image_grid_free(dst);
#endif
	if (dst->sy != src->sy) {
		free(dst->viewport_base);
		grid_viewport_allocate(dst, src->sy);
	}
	grid_rows_share(dst, src);
	dst->sx = src->sx;
	dst->sy = src->sy;
	dst->hscrolled = src->hscrolled;
	for (y = 0; y < src->sy; y++) {
		grid_retain_line(&dst->viewport[y], &src->viewport[y]);
		src->viewport[y].shared = dst->viewport[y].shared;
	}
#ifdef ENABLE_IMAGES
	image_grid_duplicate_lines(dst, 0, src, 0, src->hsize + src->sy);
#endif
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
	time_t now = pane_worker_thread ? pane_worker_time : current_time;

	if (now == 0)
		gl->time = 0;
	else
		gl->time = now - start_time.tv_sec + 1;
}

/* Copy default into a cell. */
static void
grid_clear_cell(struct grid_line *gl, u_int px, u_int bg, int moved)
{
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
	struct grid_line	*gl = grid_edit_line_record(gd, py);

#ifdef ENABLE_IMAGES
	if (gd->images != NULL)
		image_grid_free_line(gd, py);
#endif
	grid_release_line(gl);
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
	gd->row_private_tree = 1;

	if (hlimit != 0)
		gd->flags = GRID_HISTORY;
	gd->hlimit = hlimit;
	grid_viewport_allocate(gd, sy);
	return (gd);
}

/* Destroy grid. */
void
grid_destroy(struct grid *gd)
{
	u_int y;

	for (y = 0; y < gd->sy; y++)
		grid_release_line(&gd->viewport[y]);
	grid_rows_release(gd);
	grid_release_line(&gd->spare);
#ifdef ENABLE_IMAGES
	image_grid_free(gd);
#endif
	free(gd->viewport_base);
	free(gd);
}

/* Compare grids. */
int
grid_compare(struct grid *ga, struct grid *gb)
{
	const struct grid_line *gla, *glb;
	struct grid_cell	 gca, gcb;
	u_int			 xx, yy;

	if (ga->sx != gb->sx || ga->sy != gb->sy)
		return (1);

	for (yy = 0; yy < ga->sy; yy++) {
		gla = grid_get_line(ga, yy);
		glb = grid_get_line(gb, yy);
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
void
grid_trim_history(struct grid *gd, u_int ny)
{
	if (ny == 0)
		return;
#ifdef ENABLE_IMAGES
	image_grid_drop_lines(gd, 0, ny);
#endif
	grid_rows_trim(gd, ny);
}

/* Collect the oldest 10% at the limit, or trim exactly to the limit. */
void
grid_collect_history(struct grid *gd, int all)
{
	u_int ny;

	if (gd->hsize == 0 || gd->hsize < gd->hlimit)
		return;
	ny = all ? gd->hsize - gd->hlimit : gd->hlimit / 10;
	if (ny < 1)
		ny = 1;
	if (ny > gd->hsize)
		ny = gd->hsize;
	grid_trim_history(gd, ny);
	gd->scroll_collected += ny;
	if (gd->hscrolled > gd->hsize)
		gd->hscrolled = gd->hsize;
}

/* Promote the history suffix while resizing the viewport. */
void
grid_resize_viewport(struct grid *gd, u_int sy, u_int promote)
{
	struct grid_line *old = gd->viewport, *base = gd->viewport_base;
	u_int keep = sy - promote, n = gd->sy < keep ? gd->sy : keep;
	u_int y;

	grid_free_lines(gd, gd->hsize + n, gd->sy - n);
	if (sy != gd->sy) {
		grid_viewport_allocate(gd, sy);
		if (n != 0)
			memcpy(gd->viewport + promote, old, n * sizeof *old);
		free(base);
	} else if (promote != 0)
		memmove(old + promote, old, n * sizeof *old);

	for (y = 0; y < promote; y++)
		grid_retain_line(&gd->viewport[y],
		    grid_rows_get(gd, gd->hsize - promote + y));
	grid_rows_pop(gd, promote);
	gd->sy = sy;
}

void
grid_remove_history(struct grid *gd, u_int ny)
{
	if (ny == 0 || ny > gd->hsize || ny > gd->sy)
		return;
	grid_resize_viewport(gd, gd->sy, ny);
}

/* Remove the top descriptor without moving any cell payload. */
static struct grid_line
grid_take_top(struct grid *gd)
{
	struct grid_line result = *gd->viewport;
	struct grid_line *end = gd->viewport_base + gd->viewport_space;

	memset(gd->viewport++, 0, sizeof result);
	if (end - gd->viewport < gd->sy) {
		memmove(gd->viewport_base, gd->viewport,
		    (gd->sy - 1) * sizeof result);
		memset(gd->viewport_base + gd->sy - 1, 0,
		    (gd->viewport_space - gd->sy + 1) * sizeof result);
		gd->viewport = gd->viewport_base;
	}
	return (result);
}

/* Move a row into scrollback. Full-screen scroll compacts stale ext cells. */
static void
grid_append_scrolled(struct grid *gd, struct grid_line *gl, int compact)
{
	if (compact)
		grid_compact_line(gl);
	grid_line_set_time(gl);
	grid_rows_append(gd, gl);
	gd->hscrolled++;
	gd->scroll_added++;
}

/* Height shrink transfers the top row without timestamp or scroll counters. */
void
grid_scroll_history_resize(struct grid *gd)
{
	struct grid_line row = grid_take_top(gd);

	grid_rows_append(gd, &row);
	gd->hscrolled++;
}

void
grid_scroll_history(struct grid *gd, u_int bg)
{
	struct grid_line line = grid_take_top(gd);

	grid_append_scrolled(gd, &line, 1);
	grid_empty_line(gd, gd->hsize + gd->sy - 1, bg);
}

/*
 * The repeated rows have identical contents. Keep one frozen payload per
 * template and transfer a reference for each retained occurrence.
 */
void
grid_scroll_history_lines(struct grid *gd, const struct grid_cell *gc,
    const char *line, size_t len, u_int lines)
{
	struct grid template = { 0 };
	struct grid_line text = { 0 }, blank = { 0 }, row;
	struct grid_cell cell = *gc;
	u_int hsize = gd->hsize, hscrolled = gd->hscrolled;
	u_int collected = 0, i, n, drop, first;

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

	template.sx = gd->sx;
	template.sy = 1;
	template.viewport = &text;
	grid_empty_line(&template, 0, gc->bg);
	utf8_set(&cell.data, ' ');
	grid_set_cells(&template, 0, 0, &cell, line, len);
	grid_compact_line(&text);
	grid_line_set_time(&text);
	template.viewport = &blank;
	grid_empty_line(&template, 0, gc->bg);
	grid_compact_line(&blank);

	drop = collected < gd->hsize ? collected : gd->hsize;
	first = collected - drop;
	grid_trim_history(gd, drop);

	for (i = 0; i < gd->sy; i++) {
		row = grid_take_top(gd);
		if (i < first)
			grid_release_line(&row);
		else {
			grid_compact_line(&row);
			grid_line_set_time(&row);
			grid_rows_append(gd, &row);
		}
	}
	/* The fast path only calls this for more new rows than the viewport. */
	if (lines < gd->sy || gd->hsize > hsize)
		fatalx("invalid bulk history scroll");
	while (gd->hsize < hsize) {
		grid_retain_line(&row, &text);
		grid_rows_append(gd, &row);
	}
	text.time = 0;
	for (i = 0; i < gd->sy - 1; i++)
		grid_retain_line(&gd->viewport[i], &text);
	grid_retain_line(&gd->viewport[gd->sy - 1], &blank);
	xpool_release(text.celldata);
	xpool_release(text.extddata);
	xpool_release(blank.celldata);
	xpool_release(blank.extddata);

	gd->hscrolled = hscrolled;
	gd->scroll_added += lines;
	gd->scroll_collected += collected;
}

void
grid_clear_history(struct grid *gd)
{
	grid_trim_history(gd, gd->hsize);
	gd->hscrolled = 0;
	gd->scroll_generation++;
}

void
grid_scroll_history_region(struct grid *gd, u_int upper, u_int lower, u_int bg)
{
	struct grid_line *start = &gd->viewport[upper - gd->hsize];
	struct grid_line line = *start;
	u_int n = lower - upper;

#ifdef ENABLE_IMAGES
	image_grid_scroll_region(gd, upper, lower);
#endif
	memmove(start, start + 1, n * sizeof *start);
	memset(start + n, 0, sizeof *start);
	grid_append_scrolled(gd, &line, 0);
	grid_empty_line(gd, lower + 1, bg);
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
grid_expand_line(struct grid *gd, struct grid_line *gl, u_int sx, u_int bg)
{
	struct grid_cell_entry clear;
	u_int			 xx, space;

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
			gl->celldata = xpool_reallocarray_private(gl->celldata, sx,
			    sizeof *gl->celldata);
		memset(&gd->spare, 0, sizeof gd->spare);
	} else
		gl->celldata = xpool_reallocarray_private(gl->celldata, sx,
		    sizeof *gl->celldata);

	/* RGB background clears need one extended entry per new column. */
	if (bg & (COLOUR_FLAG_RGB|COLOUR_FLAG_THEME)) {
		if (gl->extdsize > UINT_MAX - (sx - gl->cellsize))
			fatalx("too many extended cells");
		space = gl->extdsize + sx - gl->cellsize;
		if (space > gl->extdallocated) {
			gl->extddata = xpool_reallocarray_private(gl->extddata, space,
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
	struct grid_line *gl = grid_edit_line_record(gd, py);

	memset(gl, 0, sizeof *gl);
	if (!COLOUR_DEFAULT(bg))
		grid_expand_line(gd, gl, gd->sx, bg);
}

/* Peek at grid line. */
const struct grid_line *
grid_peek_line(struct grid *gd, u_int py)
{
	if (grid_check_y(gd, __func__, py) != 0)
		return (NULL);
	return (grid_get_line(gd, py));
}

/* Get cell from line. */
static void
grid_get_cell1(const struct grid_line *gl, u_int px, struct grid_cell *gc)
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

void
grid_line_get_cell(const struct grid_line *gl, u_int px, struct grid_cell *gc)
{
	if (px < gl->cellsize)
		grid_get_cell1(gl, px, gc);
	else
		*gc = grid_default_cell;
}

/* Get cell for reading. */
void
grid_get_cell(struct grid *gd, u_int px, u_int py, struct grid_cell *gc)
{
	const struct grid_line *gl;

	if (grid_check_y(gd, __func__, py) == 0) {
		gl = grid_get_line(gd, py);
		grid_line_get_cell(gl, px, gc);
		return;
	}
	memcpy(gc, &grid_default_cell, sizeof *gc);
}

/* Get flags without decoding the cell's character and attributes. */
u_char
grid_get_cell_flags(struct grid *gd, u_int px, u_int py)
{
	const struct grid_line	*gl;
	struct grid_cell_entry	*gce;
	u_char			 flags;

	if (grid_check_y(gd, __func__, py) != 0)
		return (grid_default_cell.flags);
	gl = grid_get_line(gd, py);
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

	gl = grid_edit_line_record(gd, py);
	grid_prepare_line(gl, GRID_SHARE_CELLS);
	grid_expand_line(gd, gl, px + 1, 8);
	if (px + 1 > gl->cellused)
		gl->cellused = px + 1;

	gce = &gl->celldata[px];
	if (grid_need_extended_cell(gce, gc)) {
		grid_prepare_line(gl, GRID_SHARE_EXTD);
		grid_extended_cell(gl, gce, gc);
	} else
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
	int			 extended = slen != 0 &&
	    grid_need_extended_cell(&empty, gc);
	utf8_char		 ascii;

	if (grid_check_y(gd, __func__, py) != 0)
		return;

	gl = grid_edit_line_record(gd, py);
	grid_prepare_line(gl, GRID_SHARE_CELLS);
	if (extended || (gl->flags & GRID_LINE_EXTENDED))
		grid_prepare_line(gl, GRID_SHARE_EXTD);
	grid_expand_line(gd, gl, px + slen, 8);
	if (px + slen > gl->cellused)
		gl->cellused = px + slen;

	/* Every character in this run has the same extended attributes. */
	if (extended) {
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
	gl = grid_edit_line_record(gd, py);
	grid_prepare_line(gl, GRID_SHARE_CELLS | GRID_SHARE_EXTD);
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
		grid_expand_line(gd, gl, request, 8);
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
		gl->extddata = xpool_reallocarray_private(gl->extddata, space,
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
		gl = grid_edit_line_record(gd, yy);

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

		grid_prepare_line(gl, GRID_SHARE_CELLS);
		if ((gl->flags & GRID_LINE_EXTENDED) ||
		    (bg & (COLOUR_FLAG_RGB | COLOUR_FLAG_THEME)))
			grid_prepare_line(gl, GRID_SHARE_EXTD);
		grid_expand_line(gd, gl, px + ox, 8); /* default bg first */
		for (xx = px; xx < px + ox; xx++)
			grid_clear_cell(gl, xx, bg, 0);
	}
}

/* Clear lines. This just frees and truncates the lines. */
void
grid_clear_lines(struct grid *gd, u_int py, u_int ny, u_int bg)
{
	u_int yy;

	if (ny == 0 || grid_check_y(gd, __func__, py) != 0 ||
	    grid_check_y(gd, __func__, py + ny - 1) != 0)
		return;

#ifdef ENABLE_IMAGES
	image_grid_overwrite(gd, 0, py, gd->sx, ny);
#endif
	for (yy = py; yy < py + ny; yy++) {
		struct grid_line *gl = grid_edit_line_record(gd, yy);

		if (gl->shared != 0 && grid_job_metrics != NULL)
			grid_job_metrics->full_clears++;
		grid_release_line(gl);
		grid_empty_line(gd, yy, bg);
	}
	if (py != 0)
		grid_clear_wrapped(gd, py - 1);
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

#ifdef ENABLE_IMAGES
	image_grid_shift_rows(gd, dy, py, ny);
#endif
	/* Replaced owners outside the source span are released once. */
	for (yy = dy; yy < dy + ny; yy++) {
		struct grid_line *gl = grid_edit_line_record(gd, yy);

		if (yy >= py && yy < py + ny)
			continue;
		if (gl->shared == 0 && gl->cellsize != 0) {
			grid_release_line(&gd->spare);
			gd->spare = *gl;
			memset(gl, 0, sizeof *gl);
		} else
			grid_release_line(gl);
	}
	if (dy != 0)
		grid_clear_wrapped(gd, dy - 1);
	memmove(grid_edit_line_record(gd, dy), grid_edit_line_record(gd, py),
	    ny * sizeof(struct grid_line));

	/*
	 * Wipe any lines that have been moved (without freeing them - they are
	 * still present).
	 */
	for (yy = py; yy < py + ny; yy++) {
		if (yy < dy || yy >= dy + ny)
			grid_empty_line(gd, yy, bg);
	}
	if (py != 0 && (py < dy || py >= dy + ny))
		grid_clear_wrapped(gd, py - 1);
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
	gl = grid_edit_line_record(gd, py);
	grid_prepare_line(gl, GRID_SHARE_CELLS);
	if (bg & (COLOUR_FLAG_RGB | COLOUR_FLAG_THEME))
		grid_prepare_line(gl, GRID_SHARE_EXTD);

	grid_expand_line(gd, gl, px + nx, 8);
	grid_expand_line(gd, gl, dx + nx, 8);
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
		grid_clear_cell(gl, xx, bg, 1);
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
grid_string_line(const struct grid_line *gl, u_int px, u_int nx,
    struct grid_cell **lastgc, int flags, struct screen *s)
{
	struct grid_cell	 gc;
	static struct grid_cell	 lastgc1;
	const char		*data;
	char			*buf, code[8192];
	size_t			 len, off, size, codelen;
	u_int			 xx, end;
	int			 has_link = 0;

	if (lastgc != NULL && *lastgc == NULL) {
		memcpy(&lastgc1, &grid_default_cell, sizeof lastgc1);
		*lastgc = &lastgc1;
	}

	len = 128;
	buf = xmalloc(len);
	off = 0;

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
		grid_line_get_cell(gl, xx, &gc);
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

char *
grid_string_cells(struct grid *gd, u_int px, u_int py, u_int nx,
    struct grid_cell **lastgc, int flags, struct screen *s)
{
	return (grid_string_line(grid_peek_line(gd, py), px, nx, lastgc,
	    flags, s));
}

/*
 * Duplicate a set of lines between two grids. Both source and destination
 * should be big enough.
 */
void
grid_duplicate_lines(struct grid *dst, u_int dy, struct grid *src, u_int sy,
    u_int ny)
{
	struct grid_line	*dstl;
	const struct grid_line	*srcl;
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
		srcl = grid_get_line(src, sy);
		dstl = grid_edit_line_record(dst, dy);

		grid_copy_line_data(dstl, srcl);

		sy++;
		dy++;
	}
#ifdef ENABLE_IMAGES
	image_grid_duplicate_lines(dst, ody, src, osy, ny);
#endif
}

/* Copy a row and its text arrays into a distinct owner. */
void
grid_copy_line_data(struct grid_line *dstl, const struct grid_line *srcl)
{
	memcpy(dstl, srcl, sizeof *dstl);
	dstl->shared = 0;
	if (srcl->cellsize != 0) {
		dstl->celldata = xpool_reallocarray_private(NULL,
		    srcl->cellsize, sizeof *dstl->celldata);
		memcpy(dstl->celldata, srcl->celldata,
		    srcl->cellsize * sizeof *dstl->celldata);
	} else
		dstl->celldata = NULL;
	if (srcl->extdsize != 0) {
		dstl->extdsize = srcl->extdsize;
		dstl->extddata = xpool_reallocarray_private(NULL, dstl->extdsize,
		    sizeof *dstl->extddata);
		memcpy(dstl->extddata, srcl->extddata, dstl->extdsize *
		    sizeof *dstl->extddata);
	} else
		dstl->extddata = NULL;
	dstl->extdallocated = dstl->extdsize;
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

/* The pending source header carries column/marker adjustments after a partial
 * join. Its borrowed cell pointer is never published as an owning pointer. */
struct grid_reflow {
	struct grid *source;
	struct grid *target;
	struct grid_row_cursor reader;
	struct grid_line pending;
	u_int pending_y;
	u_int filled;
	u_int hscrolled;
};

static const struct grid_line *
grid_reflow_read(struct grid_reflow *rf, u_int y)
{
	if (y == rf->pending_y)
		return (&rf->pending);
	return (grid_read_line(rf->source, &rf->reader, y));
}

static u_int
grid_reflow_count(const struct grid_reflow *rf)
{
	return (rf->target->hsize + rf->filled);
}

/* Keep only the final viewport-height suffix in mutable storage. */
static void
grid_reflow_emit(struct grid_reflow *rf, const struct grid_line *line)
{
	struct grid *target = rf->target;
	struct grid_line oldest;

	if (rf->filled == target->sy) {
		oldest = grid_take_top(target);
		grid_rows_append(target, &oldest);
	} else
		rf->filled++;
	target->viewport[rf->filled - 1] = *line;
}

/* Preserve a whole row, including raw capture's allocated trailing cells. */
static void
grid_reflow_retain(struct grid_reflow *rf, struct grid_line *dst,
    const struct grid_line *src)
{
	if (src == &rf->pending)
		grid_copy_line_data(dst, src);
	else
		grid_retain_line(dst, src);
}

static void
grid_reflow_reserve(struct grid *target, struct grid_line *dst, u_int size)
{
	u_int capacity = dst->cellsize, request = 0;

	/* Match the reported capacity of sequential cell stores. */
	while (size > capacity) {
		request = capacity + 1;
		capacity = grid_line_size(target, request);
	}
	if (request != 0 && (dst->shared & GRID_SHARE_CELLS)) {
		struct grid_cell_entry *old = dst->celldata;
		u_int i;

		dst->celldata = xpool_reallocarray_private(NULL, capacity,
		    sizeof *dst->celldata);
		memcpy(dst->celldata, old, dst->cellsize * sizeof *old);
		for (i = dst->cellsize; i < capacity; i++)
			dst->celldata[i] = grid_cleared_entry;
		dst->cellsize = capacity;
		xpool_release(old);
		dst->shared &= ~GRID_SHARE_CELLS;
	} else if (request != 0)
		grid_expand_line(target, dst, request, 8);
}

/* Copy a bounded source fragment into a private destination row. */
static void
grid_reflow_cells(struct grid *target, struct grid_line *dst,
    const struct grid_line *src, u_int start, u_int n)
{
	struct grid_cell gc;
	struct grid_cell_entry *to, *end;
	u_int i, at = dst->cellused;

	grid_reflow_reserve(target, dst, at + n);
	grid_prepare_line(dst, GRID_SHARE_CELLS | GRID_SHARE_EXTD);
	to = &dst->celldata[at];
	dst->cellused += n;
	if (((src->flags | dst->flags) & GRID_LINE_EXTENDED) == 0) {
		memcpy(to, src->celldata + start, n * sizeof *to);
		for (end = to + n; to != end; to++)
			to->flags &= ~GRID_FLAG_CLEARED;
		return;
	}
	for (i = 0; i < n; i++, to++) {
		grid_get_cell1(src, start + i, &gc);
		if (grid_need_extended_cell(to, &gc))
			grid_extended_cell(dst, to, &gc);
		else
			grid_store_cell(to, &gc, gc.data.data[0]);
	}
}

static u_int
grid_reflow_fit(const struct grid_line *src, u_int start, u_int sx,
    u_int *width)
{
	struct grid_cell gc;
	u_int i, n;

	if ((src->flags & GRID_LINE_EXTENDED) == 0) {
		n = src->cellused - start;
		if (n > sx - *width)
			n = sx - *width;
		*width += n;
		return (n);
	}
	for (i = start; i < src->cellused; i++) {
		grid_get_cell1(src, i, &gc);
		if (gc.data.width > sx - *width)
			break;
		*width += gc.data.width;
	}
	return (i - start);
}

/* Size a joined row before touching its payload. Re-read the same pinned
 * source span to preserve marker and extended-cell store semantics. */
static void
grid_reflow_join(struct grid_reflow *rf, struct grid_line *dst, u_int *y,
    u_int width)
{
	struct grid *src = rf->source, *target = rf->target;
	struct grid_line from;
	u_int next, stop = *y + 1, n, last_cells = 0, appended = 0;
	u_int extd_new = 0, to = grid_reflow_count(rf);
	u_int total = src->hsize + src->sy, capacity;
	int wrapped = 1, partial = 0;

	for (next = *y + 1; next < total; next++) {
#ifdef ENABLE_IMAGES
		if (src->images != NULL && image_grid_line_has_images(src, next))
			break;
#endif
		from = *grid_reflow_read(rf, next);
		wrapped = (from.flags & GRID_LINE_WRAPPED) != 0;
		if (from.cellused == 0) {
			if (!wrapped && !(from.flags & GRID_LINE_OSC133_FLAGS))
				break;
			n = 0;
		} else {
			n = grid_reflow_fit(&from, 0, target->sx, &width);
			if (n == 0)
				break;
		}
		stop = next + 1;
		last_cells = n;
		appended += n;
		if (from.flags & GRID_LINE_EXTENDED)
			extd_new += n;
		partial = n != from.cellused;
		if (partial || !wrapped || (n != 0 && width == target->sx))
			break;
	}
	if (stop == *y + 1)
		return;
	if (appended != 0) {
		grid_reflow_reserve(target, dst, dst->cellused + appended);
		if (extd_new > UINT_MAX - dst->extdsize)
			fatalx("too many extended cells");
		capacity = dst->extdallocated;
		while (capacity < dst->extdsize + extd_new)
			capacity = capacity < 16 ? 16 :
			    capacity > UINT_MAX / 2 ? UINT_MAX : capacity * 2;
		if (capacity > dst->extdallocated &&
		    (dst->shared & GRID_SHARE_EXTD)) {
			struct grid_extd_entry *old = dst->extddata;

			dst->extddata = xpool_reallocarray_private(NULL, capacity,
			    sizeof *old);
			memcpy(dst->extddata, old, dst->extdsize * sizeof *old);
			xpool_release(old);
			dst->shared &= ~GRID_SHARE_EXTD;
		} else if (capacity > dst->extdallocated)
			dst->extddata = xpool_reallocarray_private(dst->extddata,
			    capacity, sizeof *dst->extddata);
		dst->extdallocated = capacity;
	}

	for (next = *y + 1; next < stop; next++) {
		from = *grid_reflow_read(rf, next);
		n = next == stop - 1 ? last_cells : from.cellused;
		grid_reflow_move_osc133(dst, &from, dst->cellused, n,
		    n == from.cellused);
		if (n != 0)
			grid_reflow_cells(target, dst, &from, 0, n);
	}
	if (partial) {
		from.celldata += last_cells;
		from.cellused -= last_cells;
		from.cellsize = from.cellused;
		rf->pending = from;
		rf->pending_y = --stop;
	} else if (!wrapped)
		dst->flags &= ~GRID_LINE_WRAPPED;

	total = stop - *y - 1;
	if (rf->hscrolled > to + total)
		rf->hscrolled -= total;
	else if (rf->hscrolled > to)
		rf->hscrolled = to;
	*y += total;
}

/* Split into private rows, retaining just the unchanged first slice when its
 * buffer starts at offset zero. */
static void
grid_reflow_split(struct grid_reflow *rf, const struct grid_line *src,
    u_int *yy, u_int at)
{
	struct grid *target = rf->target;
	struct grid_line dst, markers = *src;
	struct grid_cell gc;
	u_int start = at, n, width, lines = 1;

	grid_reflow_retain(rf, &dst, src);
	dst.cellsize = dst.cellused = at;
	dst.flags &= ~GRID_LINE_OSC133_FLAGS;
	dst.flags |= GRID_LINE_WRAPPED;
	memset(&dst.osc133_data, 0, sizeof dst.osc133_data);
	grid_reflow_move_osc133(&dst, &markers, 0, at, 0);
	grid_reflow_emit(rf, &dst);

	while (start < src->cellused) {
		width = 0;
		n = grid_reflow_fit(src, start, target->sx, &width);
		if (n == 0) {
			/* Legacy reflow advances past an overwide first cell. */
			grid_get_cell1(src, start, &gc);
			width = gc.data.width;
			n = 1;
		}
		memset(&dst, 0, sizeof dst);
		grid_reflow_cells(target, &dst, src, start, n);
		start += n;
		lines++;
		if (start != src->cellused || (src->flags & GRID_LINE_WRAPPED))
			dst.flags |= GRID_LINE_WRAPPED;
		grid_reflow_move_osc133(&dst, &markers, 0, n, start == src->cellused);
		if (start != src->cellused)
			grid_reflow_emit(rf, &dst);
	}

	if (*yy <= rf->hscrolled)
		rf->hscrolled += lines - 1;
	if (width < target->sx && (src->flags & GRID_LINE_WRAPPED))
		grid_reflow_join(rf, &dst, yy, width);
	grid_reflow_emit(rf, &dst);
}

void
grid_reflow(struct grid *gd, u_int sx)
{
	struct grid_reflow rf = { .source = gd, .pending_y = UINT_MAX,
	    .hscrolled = gd->hscrolled };
	struct grid *target = rf.target = grid_create(sx, gd->sy, gd->hlimit);
	const struct grid_line *src;
	struct grid_line dst, empty = { 0 };
	struct grid_cell gc;
	u_int yy, i, at, width;
	int over;

	for (yy = 0; yy < gd->hsize + gd->sy; yy++) {
		src = grid_reflow_read(&rf, yy);
#ifdef ENABLE_IMAGES
		if (gd->images != NULL && image_grid_line_has_images(gd, yy)) {
			image_grid_reflow_line(gd, yy, grid_reflow_count(&rf));
			grid_reflow_retain(&rf, &dst, src);
			grid_reflow_emit(&rf, &dst);
			continue;
		}
#endif
		if ((src->flags & GRID_LINE_EXTENDED) == 0) {
			width = src->cellused;
			at = width < sx ? width : sx;
		} else {
			at = width = 0;
			over = 0;
			for (i = 0; i < src->cellused; i++) {
				grid_get_cell1(src, i, &gc);
				/* Match the original zero sentinel for the split. */
				if (at == 0 && width + gc.data.width > sx) {
					at = i;
					over = 1;
				}
				width += gc.data.width;
			}
			if (!over)
				at = src->cellused;
		}
		if (width > sx) {
			grid_reflow_split(&rf, src, &yy, at);
			continue;
		}
		grid_reflow_retain(&rf, &dst, src);
		if (width < sx && (src->flags & GRID_LINE_WRAPPED))
			grid_reflow_join(&rf, &dst, &yy, width);
		grid_reflow_emit(&rf, &dst);
	}
	while (rf.filled < target->sy)
		grid_reflow_emit(&rf, &empty);

	for (yy = 0; yy < gd->sy; yy++) {
		xpool_release(gd->viewport[yy].celldata);
		xpool_release(gd->viewport[yy].extddata);
	}
	grid_rows_release(gd);
	free(gd->viewport_base);
#ifdef ENABLE_IMAGES
	image_grid_reflow_finish(gd);
#endif
	gd->rows = target->rows;
	gd->row_private_tree = target->row_private_tree;
	gd->row_tail = target->row_tail;
	gd->row_tail_block = target->row_tail_block;
	gd->row_first = target->row_first;
	gd->hsize = target->hsize;
	gd->hscrolled = rf.hscrolled < gd->hsize ? rf.hscrolled : gd->hsize;
	gd->viewport = target->viewport;
	gd->viewport_base = target->viewport_base;
	gd->viewport_space = target->viewport_space;
	gd->scroll_generation++;
	free(target);
}

/* Convert to position based on wrapped lines. */
void
grid_wrap_position(struct grid *gd, u_int px, u_int py, u_int *wx, u_int *wy)
{
	u_int	ax = 0, ay = 0, yy;

	for (yy = 0; yy < py; yy++) {
		if (grid_get_line(gd, yy)->flags & GRID_LINE_WRAPPED)
			ax += grid_get_line(gd, yy)->cellused;
		else {
			ax = 0;
			ay++;
		}
	}
	if (px >= grid_get_line(gd, yy)->cellused)
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
		if (~grid_get_line(gd, yy)->flags & GRID_LINE_WRAPPED)
			ay++;
	}

	/*
	 * yy is now 0 on the unwrapped line which contains wx. Walk forwards
	 * until we find the end or the line now containing wx.
	 */
	if (wx == UINT_MAX) {
		while (yy < ey && grid_get_line(gd, yy)->flags & GRID_LINE_WRAPPED)
			yy++;
		wx = grid_get_line(gd, yy)->cellused;
	} else {
		while (grid_get_line(gd, yy)->flags & GRID_LINE_WRAPPED) {
			if (wx < grid_get_line(gd, yy)->cellused)
				break;
			wx -= grid_get_line(gd, yy)->cellused;
			yy++;
		}
	}
	*px = wx;
	*py = yy;
}

/* Get the length within the current screen width. */
static u_int
grid_line_length_at(const struct grid_line *gl, u_int sx)
{
	struct grid_cell	gc;
	u_int			px = gl->cellsize < sx ? gl->cellsize : sx;
	while (px > 0) {
		grid_line_get_cell(gl, px - 1, &gc);
		if ((gc.flags & GRID_FLAG_PADDING) ||
		    gc.data.size != 1 ||
		    *gc.data.data != ' ')
			break;
		px--;
	}
	return (px);
}

u_int
grid_line_length(struct grid *gd, u_int py)
{
	return (grid_line_length_at(grid_get_line(gd, py), gd->sx));
}

/* Get last position on line, not including padding. */
u_int
grid_line_limit(struct grid *gd, u_int py)
{
	const struct grid_line	*gl = grid_get_line(gd, py);
	struct grid_cell	gc;
	u_int			px;

	px = grid_line_length_at(gl, gd->sx);
	if (px == 0)
		return (0);
	px--;
	while (px > 0) {
		grid_line_get_cell(gl, px, &gc);
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
