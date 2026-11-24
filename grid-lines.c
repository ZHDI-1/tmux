/* Persistent scrollback. Only the owner of the append cursor can extend a
 * shared leaf, and only beyond the visible bounds of every published view. */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#define ROW_SHIFT 5
#define ROW_COUNT (1U << ROW_SHIFT)
#define ROW_MASK (ROW_COUNT - 1)

struct grid_row_node {
	struct grid_row_node *next;
	u_int refs;
	u_int level;
};

struct grid_row_branch {
	struct grid_row_node node;
	struct grid_row_node *child[ROW_COUNT];
};

struct grid_row_leaf {
	struct grid_row_node node;
	uint32_t occupied;
	struct grid_line line[ROW_COUNT];
};

/* Each thread drains the nodes whose final reference it releases. */
static __thread struct grid_row_node *retired_rows;

static void
row_ref(struct grid_row_node *node)
{
	if (node != NULL)
		__atomic_add_fetch(&node->refs, 1, __ATOMIC_RELAXED);
}

static void
row_unref(struct grid_row_node *node)
{
	if (node == NULL ||
	    __atomic_sub_fetch(&node->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return;
	node->next = retired_rows;
	retired_rows = node;
}

int
grid_rows_reclaim(u_int budget)
{
	struct grid_row_node *node;
	struct grid_row_branch *branch;
	struct grid_row_leaf *leaf;
	uint32_t occupied;
	u_int i;

	while (budget-- != 0 && (node = retired_rows) != NULL) {
		retired_rows = node->next;
		if (node->level != 0) {
			branch = (struct grid_row_branch *)node;
			for (i = 0; i < ROW_COUNT; i++)
				row_unref(branch->child[i]);
		} else {
			leaf = (struct grid_row_leaf *)node;
			occupied = __atomic_load_n(&leaf->occupied, __ATOMIC_ACQUIRE);
			while (occupied != 0) {
				i = __builtin_ctz(occupied);
				occupied &= occupied - 1;
				xpool_release(leaf->line[i].celldata);
				xpool_release(leaf->line[i].extddata);
			}
		}
		free(node);
	}
	return (retired_rows != NULL);
}

static struct grid_row_branch *
row_branch_private(struct grid_row_node **link, u_int level,
    int private_tree)
{
	struct grid_row_node *old = *link;
	struct grid_row_branch *branch;
	u_int i;

	if (old != NULL && (private_tree ||
	    __atomic_load_n(&old->refs, __ATOMIC_ACQUIRE) == 1))
		return ((struct grid_row_branch *)old);
	branch = xcalloc(1, sizeof *branch);
	if (grid_job_metrics != NULL)
		grid_job_metrics->branch_allocations++;
	branch->node.refs = 1;
	branch->node.level = level;
	if (old != NULL) {
		memcpy(branch->child, ((struct grid_row_branch *)old)->child,
		    sizeof branch->child);
		for (i = 0; i < ROW_COUNT; i++)
			row_ref(branch->child[i]);
	}
	*link = &branch->node;
	row_unref(old);
	return (branch);
}

static struct grid_row_leaf *
row_lookup(const struct grid *gd, uint64_t key)
{
	struct grid_row_node *node = gd->rows;

	if (node == NULL || ((key >> ROW_SHIFT) >>
	    (node->level * ROW_SHIFT)) != 0)
		return (NULL);
	while (node->level != 0) {
		node = ((struct grid_row_branch *)node)->child[
		    (key >> (node->level * ROW_SHIFT)) & ROW_MASK];
		if (node == NULL)
			return (NULL);
	}
	return ((struct grid_row_leaf *)node);
}

/* Cursor storage is owned by the reader and is valid while the view is pinned. */
const struct grid_line *
grid_rows_read(const struct grid *gd, struct grid_row_cursor *cursor, u_int y)
{
	static const struct grid_line empty;
	uint64_t key = gd->row_first + y;
	uint64_t block = key >> ROW_SHIFT;
	const struct grid_line *gl;

	if (y >= gd->hsize)
		return (&empty);
	if (!cursor->valid || cursor->block != block) {
		cursor->leaf = row_lookup(gd, key);
		cursor->block = block;
		cursor->valid = 1;
	}
	if (cursor->leaf == NULL)
		return (&empty);
	gl = &cursor->leaf->line[key & ROW_MASK];
	if (gd->row_clear_wrap && y == gd->hsize - 1) {
		cursor->override = *gl;
		cursor->override.flags &= ~GRID_LINE_WRAPPED;
		return (&cursor->override);
	}
	return (gl);
}

const struct grid_line *
grid_rows_get(struct grid *gd, u_int y)
{
	return (grid_rows_read(gd, &gd->row_read, y));
}

/* Replace a leaf, copying only the intersection with this view. A worker
 * holding the old append cursor may be writing a slot outside that range. */
static struct grid_row_leaf *
row_fork(struct grid *gd, uint64_t key)
{
	struct grid_row_branch *branch;
	struct grid_row_node **link, *old;
	struct grid_row_leaf *leaf, *source;
	uint64_t block = key >> ROW_SHIFT;
	uint64_t base = key & ~(uint64_t)ROW_MASK;
	uint64_t first = gd->row_first, end = first + gd->hsize;
	u_int i, level;

	if (gd->rows == NULL) {
		for (level = 0; (block >> (level * ROW_SHIFT)) != 0; level++)
			;
		if (level != 0)
			row_branch_private(&gd->rows, level, gd->row_private_tree);
	}
	while (gd->rows != NULL &&
	    (block >> (gd->rows->level * ROW_SHIFT)) != 0) {
		branch = xcalloc(1, sizeof *branch);
		if (grid_job_metrics != NULL)
			grid_job_metrics->branch_allocations++;
		branch->node.refs = 1;
		branch->node.level = gd->rows->level + 1;
		branch->child[0] = gd->rows;
		gd->rows = &branch->node;
	}
	link = &gd->rows;
	for (level = *link == NULL ? 0 : (*link)->level; level != 0; level--) {
		branch = row_branch_private(link, level, gd->row_private_tree);
		link = &branch->child[(key >> (level * ROW_SHIFT)) & ROW_MASK];
	}
	old = *link;
	source = (struct grid_row_leaf *)old;
	leaf = xcalloc(1, sizeof *leaf);
	if (grid_job_metrics != NULL) {
		grid_job_metrics->leaf_allocations++;
		grid_job_metrics->tail_forks += source != NULL;
	}
	leaf->node.refs = 1;
	if (source != NULL) {
		for (i = 0; i < ROW_COUNT && base + i < end; i++) {
			if (base + i < first)
				continue;
			leaf->line[i] = source->line[i];
			xpool_retain(leaf->line[i].celldata);
			xpool_retain(leaf->line[i].extddata);
			leaf->occupied |= UINT32_C(1) << i;
		}
	}
	*link = &leaf->node;
	row_unref(old);
	gd->row_read.valid = 0;
	return (leaf);
}

/* Transfer the row's references into history. The caller may still own the
 * header, but must no longer read or release its payloads. */
void
grid_rows_append(struct grid *gd, const struct grid_line *gl)
{
	uint64_t key = gd->row_first + gd->hsize;
	u_int slot = key & ROW_MASK;
	uint32_t bit = UINT32_C(1) << slot, occupied;
	struct grid_row_leaf *leaf = gd->row_tail;

	if (gd->row_clear_wrap) {
		if (grid_job_metrics != NULL)
			grid_job_metrics->wrap_materializations++;
		leaf = row_fork(gd, key - 1);
		leaf->line[(key - 1) & ROW_MASK].flags &= ~GRID_LINE_WRAPPED;
		gd->row_clear_wrap = 0;
		gd->row_tail = leaf;
		gd->row_tail_block = (key - 1) >> ROW_SHIFT;
	}
	if (leaf == NULL || gd->row_tail_block != (key >> ROW_SHIFT) ||
	    (__atomic_load_n(&leaf->occupied, __ATOMIC_RELAXED) & bit)) {
		leaf = row_fork(gd, key);
		gd->row_tail = leaf;
		gd->row_tail_block = key >> ROW_SHIFT;
	} else if (grid_job_metrics != NULL)
		grid_job_metrics->shared_appends++;
	occupied = __atomic_load_n(&leaf->occupied, __ATOMIC_RELAXED);
	leaf->line[slot] = *gl;
	__atomic_store_n(&leaf->occupied, occupied | bit, __ATOMIC_RELEASE);
	gd->hsize++;
}

/* Detach complete nodes only. The partial first and last leaf are retained. */
static void
row_erase(struct grid_row_node **link, uint64_t first, uint64_t last,
    int private_tree)
{
	struct grid_row_node *node = *link;
	struct grid_row_branch *branch;
	uint64_t span, lower, upper;
	u_int i;

	if (node == NULL || first >= last)
		return;
	span = UINT64_C(1) << (node->level * ROW_SHIFT);
	if (first == 0 && (last >> (node->level * ROW_SHIFT)) >= ROW_COUNT) {
		*link = NULL;
		row_unref(node);
		return;
	}
	if (first / span >= ROW_COUNT || node->level == 0)
		return;
	branch = row_branch_private(link, node->level, private_tree);
	for (i = first / span; i < ROW_COUNT; i++) {
		lower = span * i;
		if (lower >= last)
			break;
		upper = last - lower;
		if (upper > span)
			upper = span;
		row_erase(&branch->child[i], first > lower ? first - lower : 0,
		    upper, private_tree);
	}
}

void
grid_rows_trim(struct grid *gd, u_int n)
{
	uint64_t end = gd->row_first + n;

	if (n == gd->hsize) {
		grid_rows_release(gd);
		gd->hsize = 0;
	} else {
		if ((end >> ROW_SHIFT) != (gd->row_first >> ROW_SHIFT)) {
			row_erase(&gd->rows, gd->row_first & ~(uint64_t)ROW_MASK,
			    end & ~(uint64_t)ROW_MASK, gd->row_private_tree);
			gd->row_read.valid = 0;
		}
		gd->hsize -= n;
	}
	gd->row_first = end;
}

void
grid_rows_pop(struct grid *gd, u_int n)
{
	uint64_t end = gd->row_first + gd->hsize;
	uint64_t keep = end - n;

	if (n == 0)
		return;
	if (n == gd->hsize)
		grid_rows_release(gd);
	else if (((end - 1) >> ROW_SHIFT) != ((keep - 1) >> ROW_SHIFT)) {
		row_erase(&gd->rows, (keep + ROW_MASK) & ~(uint64_t)ROW_MASK,
		    (end + ROW_MASK) & ~(uint64_t)ROW_MASK, gd->row_private_tree);
		gd->row_tail = NULL;
		gd->row_read.valid = 0;
	}
	gd->hsize -= n;
	gd->row_clear_wrap = 0;
}

void
grid_rows_share(struct grid *dst, struct grid *src)
{
	grid_rows_release(dst);
	row_ref(src->rows);
	if (src->rows != NULL)
		dst->row_private_tree = src->row_private_tree = 0;
	dst->rows = src->rows;
	dst->row_first = src->row_first;
	dst->row_clear_wrap = src->row_clear_wrap;
	dst->hsize = src->hsize;
}

/* Only the parser gets the source's append cursor. The queue mutex publishes
 * the handoff. An invalidated source has to fork before it can append again. */
void
grid_rows_handoff(struct grid *dst, struct grid *src)
{
	dst->row_tail = src->row_tail;
	dst->row_tail_block = src->row_tail_block;
	src->row_tail = NULL;
}

/* Completion has quiesced the worker. Return a cursor only if its
 * physical leaf is still the coordinator's append candidate. */
void
grid_rows_return_tail(struct grid *dst, struct grid *src)
{
	uint64_t key = dst->row_first + dst->hsize;

	if (dst->row_tail == NULL && src->row_tail != NULL &&
	    src->row_tail_block == key >> ROW_SHIFT &&
	    row_lookup(dst, key) == src->row_tail)
		grid_rows_handoff(dst, src);
}

void
grid_rows_release(struct grid *gd)
{
	row_unref(gd->rows);
	gd->rows = NULL;
	gd->row_private_tree = 1;
	gd->row_tail = NULL;
	gd->row_read.valid = 0;
	gd->row_clear_wrap = 0;
}
