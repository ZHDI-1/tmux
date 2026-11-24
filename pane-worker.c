/* Sequential screen work for one pane. The server owns every published screen. */

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "tmux.h"

struct pane_work {
	struct screen	 screen;
	struct input_ctx *input;
	const u_char	*bytes; /* Borrowed from the held gather slot. */
	size_t		 size;
	size_t		 used;
	u_int		 sx;
	u_int		 sy;
	int		 scroll_on_clear;
	time_t		 time;
	uint64_t	 copy_token;
	u_int		 anchor_x, anchor_y;
	u_int		 mapped_x, mapped_y;
	int		 trace;
	uint64_t	 snapshot_ns, snapshot_bytes, snapshot_retains;
	uint64_t	 queued_ns, started_ns, completed_ns, parse_cpu_ns, taken_ns;
	struct grid_job_metrics metrics;
};

struct pane_worker {
	struct window_pane *pane;	/* Coordinator only. */
	struct event	 event;	/* Coordinator only. */
	int		 busy;	/* Coordinator only. */

	int		 notify[2];
	pthread_mutex_t	 mutex;
	pthread_cond_t	 work;
	int		 stop;
	void		*retired_buffer;
	struct pane_work *pending;
	struct pane_work *complete;
};

int pane_workers_enabled;
__thread int pane_worker_thread;
__thread time_t pane_worker_time;

void
pane_workers_init(void)
{
	const char *value = getenv("TMUX_PANE_WORKERS");

	pane_workers_enabled = value != NULL && strcmp(value, "1") == 0;
}

void
pane_worker_discard(struct pane_work *work)
{
	if (work == NULL)
		return;
	if (work->screen.grid != NULL)
		grid_destroy(work->screen.grid);
	free(work->screen.tabs);
	input_worker_free(work->input);
	free(work);
}

static uint64_t
pane_worker_clock(clockid_t clock)
{
	struct timespec ts;

	if (clock_gettime(clock, &ts) != 0)
		return (0);
	return ((uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

static void
pane_worker_record(struct pane_work *work, FILE *f, int accepted)
{
	const struct grid_job_metrics *m = &work->metrics;

	if (f == NULL || !work->trace)
		return;
	fprintf(f, "{\"accepted\":%d,\"submitted\":%zu,\"consumed\":%zu"
	    ",\"snapshot_cpu_ns\":%" PRIu64 ",\"snapshot_bytes\":%" PRIu64
	    ",\"retains\":%" PRIu64 ",\"queue_ns\":%" PRIu64
	    ",\"parse_cpu_ns\":%" PRIu64 ",\"completion_ns\":%" PRIu64
	    ",\"commit_ns\":%" PRIu64 ",\"shared_rows_touched\":%" PRIu64
	    ",\"rows_copied\":%" PRIu64 ",\"packed_bytes\":%" PRIu64
	    ",\"extended_bytes\":%" PRIu64 ",\"probes\":%" PRIu64
	    ",\"promotions\":%" PRIu64 ",\"full_clears\":%" PRIu64
	    ",\"growth_bytes\":%" PRIu64 ",\"allocations\":%" PRIu64
	    ",\"tail_slot_appends\":%" PRIu64 ",\"tail_forks\":%" PRIu64
	    ",\"leaf_allocations\":%" PRIu64 ",\"branch_allocations\":%" PRIu64
	    ",\"wrap_materializations\":%" PRIu64 "}\n", accepted,
	    work->size, work->used, work->snapshot_ns, work->snapshot_bytes,
	    work->snapshot_retains, work->started_ns - work->queued_ns,
	    work->parse_cpu_ns, work->taken_ns - work->completed_ns,
	    pane_worker_clock(CLOCK_MONOTONIC) - work->taken_ns, m->touched,
	    m->copied_rows, m->packed_bytes, m->extended_bytes, m->probes,
	    m->promotions, m->full_clears, m->growth_bytes, m->allocations,
	    m->shared_appends, m->tail_forks, m->leaf_allocations,
	    m->branch_allocations, m->wrap_materializations);
}

void
pane_worker_reject(struct pane_work *work, struct window_pane *wp, FILE *trace)
{
	if (work->input != NULL)
		grid_rows_return_tail(wp->base.grid, work->screen.grid);
	pane_worker_record(work, trace, 0);
	pane_worker_discard(work);
}

static void
pane_worker_notify(struct pane_worker *worker)
{
	ssize_t n;

	do
		n = write(worker->notify[1], "W", 1);
	while (n == -1 && errno == EINTR);
}

static void *
pane_worker_run(void *arg)
{
	struct pane_worker *worker = arg;
	struct pane_work *work;
	uint64_t cpu = 0;

	pane_worker_thread = 1;
#ifdef __linux__
	prctl(PR_SET_NAME, "tmux-pane", 0, 0, 0);
#endif
	pthread_mutex_lock(&worker->mutex);
	for (;;) {
		while (!worker->stop && worker->pending == NULL)
			pthread_cond_wait(&worker->work, &worker->mutex);
		if (worker->stop)
			break;
		work = worker->pending;
		worker->pending = NULL;
		pthread_mutex_unlock(&worker->mutex);

		pane_worker_time = work->time;
		if (work->input != NULL) {
			if (work->trace) {
				work->started_ns = pane_worker_clock(CLOCK_MONOTONIC);
				cpu = pane_worker_clock(CLOCK_THREAD_CPUTIME_ID);
				grid_job_metrics = &work->metrics;
			}
			work->used = input_worker_parse(work->input, &work->screen,
			    work->bytes, work->size, work->scroll_on_clear);
			screen_write_free_list(&work->screen);
			if (work->trace) {
				grid_job_metrics = NULL;
				work->parse_cpu_ns = pane_worker_clock(CLOCK_THREAD_CPUTIME_ID) - cpu;
				work->completed_ns = pane_worker_clock(CLOCK_MONOTONIC);
			}
		} else if (work->copy_token != 0) {
			struct screen *s = &work->screen;
			u_int wx, wy;
			int reflow = screen_size_x(s) != work->sx;

			work->mapped_x = work->anchor_x;
			work->mapped_y = work->anchor_y;
			if (reflow)
				grid_wrap_position(s->grid, work->anchor_x,
				    work->anchor_y, &wx, &wy);
			screen_resize_cursor(s, work->sx, work->sy, 1, 0, 0);
			if (reflow)
				grid_unwrap_position(s->grid,
				    &work->mapped_x, &work->mapped_y, wx, wy);
		} else
			screen_resize(&work->screen, work->sx, work->sy, 1);

		pthread_mutex_lock(&worker->mutex);
		worker->complete = work;
		if (!worker->stop)
			pane_worker_notify(worker);
		pthread_mutex_unlock(&worker->mutex);

		/* Every tree here is already unreachable from the worker screen. */
		while (grid_rows_reclaim(64))
			;
		pthread_mutex_lock(&worker->mutex);
	}

	pthread_mutex_unlock(&worker->mutex);
	pane_worker_discard(worker->pending);
	pane_worker_discard(worker->complete);
	free(worker->retired_buffer);
	while (grid_rows_reclaim(64))
		;
	screen_write_worker_cleanup();
	close(worker->notify[0]);
	close(worker->notify[1]);
	pthread_cond_destroy(&worker->work);
	pthread_mutex_destroy(&worker->mutex);
	free(worker);
	return (NULL);
}

static void
pane_worker_ready(int fd, __unused short events, void *arg)
{
	struct pane_worker *worker = arg;
	char byte;

	while (read(fd, &byte, 1) == 1)
		;
	pane_io_worker_ready(worker->pane);
}

struct pane_worker *
pane_worker_create(struct window_pane *wp)
{
	struct pane_worker *worker;
	pthread_attr_t attr;
	pthread_t thread;
	sigset_t all, saved;
	int i, error;

	worker = xcalloc(1, sizeof *worker);
	if (pipe(worker->notify) != 0) {
		free(worker);
		return (NULL);
	}
	worker->pane = wp;
	for (i = 0; i < 2; i++) {
		setblocking(worker->notify[i], 0);
		if (fcntl(worker->notify[i], F_SETFD, FD_CLOEXEC) == -1)
			fatal("pane worker cloexec");
	}
	if (pthread_mutex_init(&worker->mutex, NULL) != 0 ||
	    pthread_cond_init(&worker->work, NULL) != 0 ||
	    pthread_attr_init(&attr) != 0 ||
	    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0)
		fatalx("pane worker synchronization init failed");
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &saved);
	error = pthread_create(&thread, &attr, pane_worker_run, worker);
	pthread_sigmask(SIG_SETMASK, &saved, NULL);
	pthread_attr_destroy(&attr);
	if (error != 0) {
		close(worker->notify[0]);
		close(worker->notify[1]);
		pthread_cond_destroy(&worker->work);
		pthread_mutex_destroy(&worker->mutex);
		free(worker);
		log_debug("pane worker create: %s", strerror(error));
		return (NULL);
	}
	event_set(&worker->event, worker->notify[0], EV_READ|EV_PERSIST,
	    pane_worker_ready, worker);
	if (event_add(&worker->event, NULL) != 0)
		fatalx("pane worker event add failed");
	return (worker);
}

void
pane_worker_destroy(struct pane_worker *worker, void *buffer)
{
	if (worker == NULL) {
		free(buffer);
		return;
	}
	event_del(&worker->event);
	pthread_mutex_lock(&worker->mutex);
	worker->retired_buffer = buffer;
	worker->stop = 1;
	pthread_cond_signal(&worker->work);
	pthread_mutex_unlock(&worker->mutex);
}

int
pane_worker_busy(struct pane_worker *worker)
{
	return (worker != NULL && worker->busy);
}

static struct pane_work *
pane_worker_snapshot(struct screen *src)
{
	struct pane_work *work = xcalloc(1, sizeof *work);
	struct screen *dst;
	size_t tabs_size;

	work->time = current_time;
	dst = &work->screen;
	dst->cx = src->cx;
	dst->cy = src->cy;
	dst->rupper = src->rupper;
	dst->rlower = src->rlower;
	dst->mode = src->mode;
	dst->grid = grid_create(0, 0, src->grid->hlimit);
	grid_share_lines(dst->grid, src->grid);
	dst->grid->flags = src->grid->flags;
	dst->grid->scroll_generation = src->grid->scroll_generation;
	dst->grid->scroll_added = src->grid->scroll_added;
	dst->grid->scroll_collected = src->grid->scroll_collected;

	tabs_size = bitstr_size(screen_size_x(src)) * sizeof *src->tabs;
	dst->tabs = xmalloc(tabs_size);
	memcpy(dst->tabs, src->tabs, tabs_size);
	return (work);
}

static void
pane_worker_queue(struct pane_worker *worker, struct pane_work *work)
{
	worker->busy = 1;
	pthread_mutex_lock(&worker->mutex);
	worker->pending = work;
	pthread_cond_signal(&worker->work);
	pthread_mutex_unlock(&worker->mutex);
}

int
pane_worker_submit(struct pane_worker *worker, struct window_pane *wp,
    const u_char *bytes, size_t size, u_int sx, u_int sy, int trace)
{
	struct screen *src = &wp->base;
	struct input_ctx *input = NULL;
	struct pane_work *work;
	uint64_t cpu = 0;
	u_int y;

	if (worker == NULL || worker->busy || !grid_worker_eligible(src->grid) ||
	    SCREEN_IS_ALTERNATE(src) || src->sel != NULL ||
	    (bytes == NULL && (src->mode & MODE_SYNC)))
		return (0);
	if (bytes != NULL) {
		input = input_worker_clone(wp->ictx);
		if (input == NULL)
			return (0);
	}
	if (trace)
		cpu = pane_worker_clock(CLOCK_THREAD_CPUTIME_ID);
	work = pane_worker_snapshot(src);
	if (trace) {
		work->trace = 1;
		work->snapshot_ns = pane_worker_clock(CLOCK_THREAD_CPUTIME_ID) - cpu;
		work->snapshot_bytes = src->grid->sy * sizeof(struct grid_line);
		for (y = 0; y < src->grid->sy; y++) {
			work->snapshot_retains += (src->grid->viewport[y].celldata != NULL);
			work->snapshot_retains += (src->grid->viewport[y].extddata != NULL);
		}
	}
	work->input = input;
	work->sx = sx;
	work->sy = sy;
	if (bytes != NULL) {
		grid_rows_handoff(work->screen.grid, src->grid);
		work->bytes = bytes;
		work->size = size;
		work->scroll_on_clear = TAILQ_EMPTY(&wp->modes) &&
		    options_get_number(wp->options, "scroll-on-clear");
	}

	if (trace)
		work->queued_ns = pane_worker_clock(CLOCK_MONOTONIC);
	pane_worker_queue(worker, work);
	return (1);
}

int
pane_worker_submit_copy(struct pane_worker *worker, struct screen *src,
    u_int sx, u_int sy, u_int cx, u_int cy, uint64_t token)
{
	struct pane_work *work;

	if (worker == NULL || worker->busy || !grid_worker_eligible(src->grid))
		return (0);
	work = pane_worker_snapshot(src);
	work->sx = sx;
	work->sy = sy;
	work->anchor_x = cx;
	work->anchor_y = cy;
	work->copy_token = token;
	pane_worker_queue(worker, work);
	return (1);
}

uint64_t
pane_worker_copy_token(const struct pane_work *work)
{
	return (work->copy_token);
}

struct pane_work *
pane_worker_take(struct pane_worker *worker)
{
	struct pane_work *work;

	if (!pane_worker_busy(worker))
		return (NULL);
	pthread_mutex_lock(&worker->mutex);
	work = worker->complete;
	worker->complete = NULL;
	pthread_mutex_unlock(&worker->mutex);
	if (work != NULL) {
		worker->busy = 0;
		if (work->trace)
			work->taken_ns = pane_worker_clock(CLOCK_MONOTONIC);
	}
	return (work);
}

static void
pane_worker_swap_screen(struct screen *dst, struct screen *src)
{
	if (dst->write_list != NULL && screen_size_y(src) != screen_size_y(dst))
		screen_write_free_list(dst);
	grid_destroy(dst->grid);
	dst->grid = src->grid;
	src->grid = NULL;
	dst->cx = src->cx;
	dst->cy = src->cy;
	dst->rupper = src->rupper;
	dst->rlower = src->rlower;
	free(dst->tabs);
	dst->tabs = src->tabs;
	src->tabs = NULL;
}

void
pane_worker_apply_copy(struct pane_work *work, struct screen *dst,
    u_int *cx, u_int *cy)
{
	u_int wx, wy;

	if (*cx == work->anchor_x && *cy == work->anchor_y) {
		*cx = work->mapped_x;
		*cy = work->mapped_y;
	} else if (screen_size_x(dst) != work->sx) {
		/* A navigation key may have moved the anchor while we reflowed. */
		grid_wrap_position(dst->grid, *cx, *cy, &wx, &wy);
		grid_unwrap_position(work->screen.grid, cx, cy, wx, wy);
	}
	pane_worker_swap_screen(dst, &work->screen);
	pane_worker_discard(work);
}

size_t
pane_worker_apply(struct pane_work *work, struct window_pane *wp, FILE *trace)
{
	struct screen *dst = &wp->base;
	struct window_mode_entry *wme;
	size_t used = work->used;

	if (work->input == NULL || used != 0) {
		pane_worker_swap_screen(dst, &work->screen);

		if (work->input != NULL)
			input_worker_commit(wp, work->input);
		else {
			/* A mode may have opened after layout changed. */
			wme = TAILQ_FIRST(&wp->modes);
			if (wme != NULL && wme->mode->resize != NULL &&
			    (screen_size_x(wp->screen) != wp->sx ||
			    screen_size_y(wp->screen) != wp->sy))
				wme->mode->resize(wme, wp->sx, wp->sy);
		}
		if (TAILQ_EMPTY(&wp->modes)) {
			if (dst->mode & MODE_SYNC)
				screen_write_sync_worker_dirty(wp);
			else
				wp->flags |= PANE_REDRAW|PANE_REDRAWSCROLLBAR;
		}
	} else
		grid_rows_return_tail(dst->grid, work->screen.grid);
	pane_worker_record(work, trace, 1);
	pane_worker_discard(work);
	return (used);
}
