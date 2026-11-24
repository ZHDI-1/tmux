/* Bounded PTY gather queues and coordinator-side pane work scheduling. */
#include <sys/types.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "tmux.h"

#define IO_LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define IO_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define IO_SC_LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define IO_SC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define IO_TRACE(p, field, amount) do { \
 if ((p)->trace != NULL) (p)->field += (amount); \
} while (0)

struct pane_io {
 struct pane_io *next;
 struct window_pane *wp; /* main thread only */
 int fd, notify[2], wake[2], mode, slots;
 int stop, enabled, done, notified, waiting;
 int running, restart, bridging, error;
 pthread_t thread;
 pthread_mutex_t mutex;
 pthread_cond_t space;
 struct event event;
 size_t capacity, slot_capacity, lens[8];
 size_t head;
 char head_pad[64];
 size_t tail;
 char tail_pad[64];
 unsigned char *data;
 char *trace;
 FILE *job_trace;
 uint64_t reads, bytes, publications, full_waits, spins, polls, worker_ns;
 uint64_t callbacks, parsed;
 struct pane_worker *worker;
 uint64_t revision, work_revision;
 uint64_t worker_bytes, worker_resizes, worker_runs, worker_rejected;
 int resize_requested, copy_resize_requested, worker_unavailable;
 enum { PANE_WORK_PARSE, PANE_WORK_RESIZE, PANE_WORK_COPY } work_type;
 uint64_t worker_copy_resizes;
};
static struct pane_io *pane_ios;
static int pane_io_atfork;
static void pane_io_callback(int, short, void *);
static void pane_io_start_thread(struct pane_io *);

static uint64_t
pane_io_clock(clockid_t clock)
{
 struct timespec ts;
 if (clock_gettime(clock, &ts) != 0) return 0;
 return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void
pane_io_drain_fd(int fd)
{
 char buf[128];
 while (read(fd, buf, sizeof buf) > 0) {}
}

static void
pane_io_wake(struct pane_io *p)
{
 ssize_t n;
 do n = write(p->wake[1], "w", 1); while (n == -1 && errno == EINTR);
 /* EAGAIN means an unread wake already exists. The fd stays live until join. */
}

static void
pane_io_notify(struct pane_io *p)
{
 ssize_t n;
 if (__atomic_exchange_n(&p->notified, 1, __ATOMIC_ACQ_REL) != 0) return;
 do n = write(p->notify[1], "n", 1); while (n == -1 && errno == EINTR);
}

static int
pane_io_poll(struct pane_io *p, int timeout)
{
 struct pollfd fds[2] = {{p->fd, POLLIN, 0}, {p->wake[0], POLLIN, 0}};
 int n;
 IO_TRACE(p, polls, 1);
 do n = poll(fds, 2, timeout); while (n == -1 && errno == EINTR);
 if (n == -1) { p->error = errno; return -1; }
 if (fds[1].revents != 0) pane_io_drain_fd(p->wake[0]);
 if (IO_LOAD(&p->stop)) return -1;
 /* A wake deliberately terminates a bridge wait, even if PTY is also ready. */
 if (timeout >= 0 && fds[1].revents != 0) return 0;
 if (fds[0].revents & (POLLIN|POLLHUP|POLLERR|POLLNVAL)) return 1;
 return 0;
}

static int
pane_io_wait_ring(struct pane_io *p)
{
 int full;
 pthread_mutex_lock(&p->mutex);
 for (;;) {
  if (IO_LOAD(&p->stop)) break;
  IO_SC_STORE(&p->waiting, 1);
  full = IO_LOAD(&p->head) - IO_SC_LOAD(&p->tail) == p->capacity;
  if (IO_LOAD(&p->enabled) && !full) break;
  IO_TRACE(p, full_waits, full);
  pthread_cond_wait(&p->space, &p->mutex);
 }
 IO_SC_STORE(&p->waiting, 0);
 pthread_mutex_unlock(&p->mutex);
 return !IO_LOAD(&p->stop);
}

static int
pane_io_ring(struct pane_io *p)
{
 size_t head, used, index, room;
 ssize_t n;
 for (;;) {
  if (IO_LOAD(&p->stop)) return 0;
  head = IO_LOAD(&p->head);
  used = head - IO_SC_LOAD(&p->tail);
  if (!IO_LOAD(&p->enabled) || used == p->capacity) {
   if (!pane_io_wait_ring(p)) return 0;
   continue;
  }
  index = head & (p->capacity - 1);
  room = p->capacity - used;
  if (room > p->capacity - index) room = p->capacity - index;
  IO_TRACE(p, reads, 1);
  n = read(p->fd, p->data + index, room);
  if (n > 0) {
   IO_TRACE(p, bytes, n);
   IO_STORE(&p->head, head + n);
   IO_TRACE(p, publications, 1);
   pane_io_notify(p);
   continue;
  }
  if (n == 0) return 1;
  if (errno == EINTR) continue;
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
   p->error = errno; return 1;
  }
  if (pane_io_poll(p, -1) < 0) return !IO_LOAD(&p->stop);
 }
}

static int
pane_io_batch(struct pane_io *p, int bridge)
{
 size_t head, total;
 unsigned int spins;
 unsigned char *buf;
 uint64_t bridge_start;
 ssize_t n;
 int end, result, busy;
 for (;;) {
  pthread_mutex_lock(&p->mutex);
  while (!IO_LOAD(&p->stop) && (!IO_LOAD(&p->enabled) ||
      IO_LOAD(&p->head) - IO_LOAD(&p->tail) == (size_t)p->slots)) {
   IO_TRACE(p, full_waits, IO_LOAD(&p->enabled));
   pthread_cond_wait(&p->space, &p->mutex);
  }
  head = IO_LOAD(&p->head);
  pthread_mutex_unlock(&p->mutex);
  if (IO_LOAD(&p->stop)) return 0;
  buf = p->data + (head % p->slots) * p->slot_capacity;
  total = spins = 0; end = 0; bridge_start = 0;
  while (total < p->slot_capacity) {
   if (IO_LOAD(&p->stop) || !IO_LOAD(&p->enabled)) break;
   IO_TRACE(p, reads, 1);
   n = read(p->fd, buf + total, p->slot_capacity - total);
   if (n > 0) { total += n; IO_TRACE(p, bytes, n); spins = 0; continue; }
   if (n == 0) { end = 1; break; }
   if (errno == EINTR) continue;
   if (errno != EAGAIN && errno != EWOULDBLOCK) {
    p->error = errno; end = 1; break;
   }
   if (!bridge || total < 1024) break;
   if (spins++ < 16) { IO_TRACE(p, spins, 1); continue; }
   if (bridge_start == 0) bridge_start = pane_io_clock(CLOCK_MONOTONIC);
   else if (pane_io_clock(CLOCK_MONOTONIC) - bridge_start >= 3000000) break;
   pthread_mutex_lock(&p->mutex);
   busy = IO_LOAD(&p->head) != IO_LOAD(&p->tail);
   p->bridging = busy;
   pthread_mutex_unlock(&p->mutex);
   if (!busy) break;
   result = pane_io_poll(p, 1);
   pthread_mutex_lock(&p->mutex);
   p->bridging = 0;
   pthread_mutex_unlock(&p->mutex);
   if (result <= 0) { if (result < 0 && !IO_LOAD(&p->stop)) end = 1; break; }
  }
  /* Publish even an incomplete batch before shutdown/fork quiescence. */
  if (total != 0) {
   pthread_mutex_lock(&p->mutex);
   p->lens[head % p->slots] = total;
   IO_STORE(&p->head, head + 1);
   pthread_mutex_unlock(&p->mutex);
   IO_TRACE(p, publications, 1);
   pane_io_notify(p);
  }
  if (end) return 1;
  if (IO_LOAD(&p->stop)) return 0;
  if (total == p->slot_capacity || !IO_LOAD(&p->enabled)) continue;
  if (pane_io_poll(p, -1) < 0) return !IO_LOAD(&p->stop);
 }
}

static void *
pane_io_worker(void *arg)
{
 struct pane_io *p = arg;
 uint64_t start = p->trace != NULL ? pane_io_clock(CLOCK_THREAD_CPUTIME_ID) : 0;
 int end;
#ifdef __linux__
 prctl(PR_SET_NAME, p->mode == 1 ? "tmux-io-ring" : "tmux-io-batch", 0, 0, 0);
#endif
 end = p->mode == 1 ? pane_io_ring(p) : pane_io_batch(p, p->mode == 2);
 if (p->trace != NULL) p->worker_ns += pane_io_clock(CLOCK_THREAD_CPUTIME_ID) - start;
 if (end) { IO_STORE(&p->done, 1); pane_io_notify(p); }
 return NULL;
}

static void
pane_io_stop_thread(struct pane_io *p)
{
 if (!p->running) return;
 pthread_mutex_lock(&p->mutex);
 IO_STORE(&p->stop, 1);
 pthread_cond_broadcast(&p->space);
 pthread_mutex_unlock(&p->mutex);
 pane_io_wake(p);
 if (pthread_join(p->thread, NULL) != 0) fatalx("pane io join failed");
 p->running = 0;
}

static void
pane_io_start_thread(struct pane_io *p)
{
 sigset_t all, saved;
 int error;
 pane_io_drain_fd(p->wake[0]);
 IO_STORE(&p->stop, 0);
 /* Only main receives asynchronous signals, including during startup. */
 sigfillset(&all);
 pthread_sigmask(SIG_BLOCK, &all, &saved);
 error = pthread_create(&p->thread, NULL, pane_io_worker, p);
 pthread_sigmask(SIG_SETMASK, &saved, NULL);
 if (error != 0) fatalx("pane io create: %s", strerror(error));
 p->running = 1;
}

static void
pane_io_fork_prepare(void)
{
 struct pane_io *p;
 for (p = pane_ios; p != NULL; p = p->next) {
  pane_io_stop_thread(p);
  p->restart = !IO_LOAD(&p->done);
 }
}

static void
pane_io_fork_parent(void)
{
 struct pane_io *p;
 for (p = pane_ios; p != NULL; p = p->next)
  if (p->restart) pane_io_start_thread(p);
}

static void
pane_io_fork_child(void)
{
 /* All workers were joined. Child will close inherited fds and exec. */
 pane_ios = NULL;
}

static void
pane_io_pipe(int fds[2])
{
 if (pipe(fds) != 0) fatal("pane io pipe");
 for (int i = 0; i < 2; i++) {
  setblocking(fds[i], 0);
  if (fcntl(fds[i], F_SETFD, FD_CLOEXEC) == -1) fatal("pane io cloexec");
 }
}

int
pane_io_start(struct window_pane *wp)
{
 struct pane_io *p;
 const char *mode = getenv("TMUX_IO_MODE"), *size, *slots, *bridge, *trace;
 char *end;
 unsigned long long bytes, count;
 if (mode == NULL) mode = "batch";
 if (strcmp(mode, "off") == 0) return 0;
 if (strcmp(mode, "ring") != 0 && strcmp(mode, "batch") != 0)
  fatalx("TMUX_IO_MODE must be off, ring or batch");
 size = getenv("TMUX_IO_BYTES"); slots = getenv("TMUX_IO_SLOTS");
 bridge = getenv("TMUX_IO_BRIDGE"); trace = getenv("TMUX_IO_TRACE");
 if (bridge == NULL) bridge = "0";
 bytes = 262144; count = 4;
 if (size != NULL) { bytes = strtoull(size, &end, 10); if (*end) fatalx("bad io size"); }
 if (slots != NULL) { count = strtoull(slots, &end, 10); if (*end) fatalx("bad io slots"); }
 if (strcmp(mode, "ring") == 0) count = 1;
 if (bytes < 256 || bytes > 16 * 1024 * 1024 || (bytes & (bytes - 1)))
  fatalx("io bytes must be a power of two, 256..16777216");
 if (count < 1 || count > 8 || bytes % count || bytes / count < 256)
  fatalx("io slots must divide bytes, <=8 and >=256 bytes/slot");
 p = xcalloc(1, sizeof *p);
 p->wp = wp; p->fd = wp->fd; p->enabled = 1; p->capacity = bytes;
 p->mode = strcmp(mode, "ring") == 0 ? 1 : (bridge != NULL && !strcmp(bridge,"0") ? 3 : 2);
 p->slots = p->mode == 1 ? 1 : count;
 p->slot_capacity = bytes / p->slots;
 p->data = xmalloc(bytes);
 if (trace != NULL) {
  char *path;
  int trace_fd;
  p->trace = xstrdup(trace);
  if (pane_workers_enabled) {
   xasprintf(&path, "%s.%ld.%u.jobs", trace, (long)getpid(), wp->id);
   trace_fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0600);
   if (trace_fd != -1 && (p->job_trace = fdopen(trace_fd, "w")) == NULL)
    close(trace_fd);
   free(path);
  }
 }
 if (pthread_mutex_init(&p->mutex, NULL) || pthread_cond_init(&p->space, NULL))
  fatalx("pane io synchronization init failed");
 pane_io_pipe(p->notify); pane_io_pipe(p->wake);
 event_set(&p->event, p->notify[0], EV_READ|EV_PERSIST, pane_io_callback, p);
 if (event_add(&p->event, NULL) != 0) fatalx("pane io event add failed");
 if (!pane_io_atfork) {
  if (pthread_atfork(pane_io_fork_prepare, pane_io_fork_parent, pane_io_fork_child))
   fatalx("pane io atfork failed");
  pane_io_atfork = 1;
 }
 p->next = pane_ios; pane_ios = p; wp->io = p;
 pane_io_start_thread(p);
 return 1;
}

void
pane_io_set_read(struct window_pane *wp, int enabled)
{
 struct pane_io *p = wp->io;
 if (p == NULL) {
  if (enabled) bufferevent_enable(wp->event, EV_READ);
  else bufferevent_disable(wp->event, EV_READ);
  return;
 }
 if (IO_LOAD(&p->enabled) == enabled) return;
 pthread_mutex_lock(&p->mutex);
 IO_STORE(&p->enabled, enabled);
 pthread_cond_broadcast(&p->space);
 pthread_mutex_unlock(&p->mutex);
 pane_io_wake(p);
}

int
pane_io_finished(struct window_pane *wp)
{
 struct pane_io *p = wp->io;
 if (p == NULL) return 1;
 /* Acquire EOF before the counters: final publication precedes done. */
 return IO_LOAD(&p->done) && IO_LOAD(&p->head) == IO_LOAD(&p->tail);
}

static int
pane_io_get_worker(struct pane_io *p)
{
 if (!pane_workers_enabled || p->worker_unavailable)
  return (0);
 if (p->worker == NULL) {
  p->worker = pane_worker_create(p->wp);
  if (p->worker == NULL) {
   p->worker_unavailable = 1;
   return (0);
  }
 }
 return (1);
}

void
pane_io_worker_ready(struct window_pane *wp)
{
 pane_io_notify(wp->io);
}

void
pane_io_invalidate(struct window_pane *wp)
{
 if (wp->io != NULL)
  wp->io->revision++;
}

/* The layout changes now, while expensive width reflow uses a snapshot. */
int
pane_io_resize(struct window_pane *wp)
{
 struct pane_io *p = wp->io;
 struct screen *s = &wp->base;

 if (!pane_workers_enabled || p == NULL || !grid_worker_eligible(s->grid) ||
     s->grid->hsize < 4096 || SCREEN_IS_ALTERNATE(s))
  return (0);
 if (screen_size_x(s) == wp->sx && !p->resize_requested &&
     !(pane_worker_busy(p->worker) && p->work_type == PANE_WORK_RESIZE))
  return (0);
 if (!pane_io_get_worker(p))
  return (0);
 p->resize_requested = 1;
 pane_io_notify(p);
 return (1);
}

int
pane_io_resize_pending(struct window_pane *wp)
{
 return (wp->io != NULL && wp->io->resize_requested);
}

int
pane_io_resize_copy(struct window_pane *wp)
{
 struct pane_io *p = wp->io;

 if (p == NULL || !pane_io_get_worker(p))
  return (0);
 p->copy_resize_requested = 1;
 pane_io_notify(p);
 return (1);
}

static int
pane_io_start_resize(struct pane_io *p)
{
 struct window_pane *wp = p->wp;
 struct screen *s = &wp->base;
 u_int old_width = screen_size_x(s);

 if (!p->resize_requested)
  return (0);
 if (old_width == wp->sx && screen_size_y(s) == wp->sy) {
  p->resize_requested = 0;
  return (0);
 }
 if (pane_worker_submit(p->worker, wp, NULL, 0, wp->sx, wp->sy, 0)) {
  p->work_revision = p->revision;
  p->work_type = PANE_WORK_RESIZE;
  return (1);
 }

 /* Alternate screens, images and synchronized output stay on main. */
 screen_resize(s, wp->sx, wp->sy, s->saved_grid == NULL);
#ifdef ENABLE_IMAGES
 if (wp->sx > old_width)
  image_grid_resize_width(s->grid, wp->sx);
#endif
 wp->flags |= PANE_REDRAW|PANE_REDRAWSCROLLBAR;
 p->resize_requested = 0;
 return (0);
}

static void
pane_io_callback(__unused int fd, __unused short events, void *arg)
{
 struct pane_io *p = arg;
 struct pane_work *work;
 size_t head, tail, end, available, first, total = 0, used = 0;
 const unsigned char *a, *b;
 int wake, finished_parse = 0;
 pane_io_drain_fd(p->notify[0]);
 __atomic_exchange_n(&p->notified, 0, __ATOMIC_ACQ_REL);
 IO_TRACE(p, callbacks, 1);
 if (pane_worker_busy(p->worker)) {
  work = pane_worker_take(p->worker);
  if (work == NULL)
   return; /* Completion will wake us, even if the PTY stopped writing. */
  finished_parse = p->work_type == PANE_WORK_PARSE;
  if (p->work_type == PANE_WORK_COPY) {
   if (window_copy_worker_finish(p->wp, work))
    p->worker_copy_resizes++;
   else
    p->worker_rejected++;
  } else if (p->work_revision == p->revision &&
      (!finished_parse || window_pane_io_direct(p->wp))) {
   used = pane_worker_apply(work, p->wp, p->job_trace);
   p->worker_bytes += used;
   p->worker_resizes += !finished_parse;
  } else {
   p->worker_rejected++;
   pane_worker_reject(work, p->wp, p->job_trace);
  }
 }
 if (!finished_parse) {
  if (pane_io_start_resize(p))
   return;
  if (p->copy_resize_requested) {
   p->copy_resize_requested = 0;
   if (window_copy_worker_submit(p->wp, p->worker)) {
    p->work_type = PANE_WORK_COPY;
    return;
   }
  }
 }
 /* Consume only what was queued at entry, then yield to tty writes.
  * Chasing a live producer here can starve output and trigger TTY_BLOCK. */
 end = IO_LOAD(&p->head);
 while (total < 64 * 1024) {
  if (p->mode != 1) pthread_mutex_lock(&p->mutex);
  head = end; tail = IO_LOAD(&p->tail);
  if (head == tail) {
   if (p->mode != 1) pthread_mutex_unlock(&p->mutex);
   break;
  }
  b = NULL;
  if (p->mode == 1) {
   available = head - tail;
   if (available > 64 * 1024 - total) available = 64 * 1024 - total;
   first = p->capacity - (tail & (p->capacity - 1));
   if (first > available) first = available;
   a = p->data + (tail & (p->capacity - 1));
   if (first < available) b = p->data;
  } else {
   available = first = p->lens[tail % p->slots];
   a = p->data + (tail % p->slots) * p->slot_capacity;
   pthread_mutex_unlock(&p->mutex);
  }
  if (!finished_parse && p->mode != 1 && first >= 4096 &&
      first <= 65536 && pane_workers_enabled && !p->resize_requested &&
      !p->copy_resize_requested &&
      grid_worker_eligible(p->wp->base.grid) && window_pane_io_direct(p->wp) &&
      pane_io_get_worker(p) &&
      pane_worker_submit(p->worker, p->wp, a, first, 0, 0,
      p->job_trace != NULL)) {
   p->work_type = PANE_WORK_PARSE;
   p->work_revision = p->revision;
   p->worker_runs++;
   return; /* Keep the original slot until its committed offset is known. */
  }
  /* Producer cannot reuse these bytes until the coordinator advances tail. */
  if (used != 0)
   window_pane_io_advance(p->wp, used);
  if (available != used)
   window_pane_io_data(p->wp, a + used, first - used, b,
       available - first);
  used = 0;
  finished_parse = 0;
  IO_TRACE(p, parsed, available);
  total += available;
  if (p->mode == 1) {
   /* SC waiter/tail handshake closes the full->sleep lost-wakeup window. */
   IO_SC_STORE(&p->tail, tail + available);
   if (IO_SC_LOAD(&p->waiting)) {
    pthread_mutex_lock(&p->mutex);
    pthread_cond_signal(&p->space);
    pthread_mutex_unlock(&p->mutex);
   }
  } else {
   pthread_mutex_lock(&p->mutex);
   IO_STORE(&p->tail, tail + 1);
   wake = IO_LOAD(&p->head) == tail + 1 && p->bridging;
   pthread_cond_signal(&p->space);
   pthread_mutex_unlock(&p->mutex);
   if (wake) pane_io_wake(p);
  }
 }
 if (IO_LOAD(&p->head) != IO_LOAD(&p->tail) ||
     p->resize_requested || p->copy_resize_requested)
  pane_io_notify(p);
 if (pane_io_finished(p->wp)) {
  window_pane_io_eof(p->wp); /* May destroy p and wp. Do not access either. */
  return;
 }
}

void
pane_io_destroy(struct window_pane *wp)
{
 struct pane_io *p = wp->io, **link;
 FILE *f;
 char *path;
 if (p == NULL) return;
 wp->io = NULL;
 event_del(&p->event);
 pane_io_stop_thread(p);
 pane_worker_destroy(p->worker, p->data);
 for (link = &pane_ios; *link != NULL; link = &(*link)->next)
  if (*link == p) { *link = p->next; break; }
 if (p->trace != NULL) {
  xasprintf(&path, "%s.%ld.%u.json", p->trace, (long)getpid(), wp->id);
  if ((f = fopen(path, "w")) != NULL) {
   fprintf(f, "{\"mode\":%d,\"bytes\":%llu,\"slots\":%d,\"read_calls\":%llu,\"input_bytes\":%llu,\"publications\":%llu,\"full_waits\":%llu,\"spin_reads\":%llu,\"polls\":%llu,\"worker_cpu_ns\":%llu,\"callbacks\":%llu,\"parsed_bytes\":%llu,\"worker_bytes\":%llu,\"worker_runs\":%llu,\"worker_resizes\":%llu,\"worker_rejected\":%llu,\"worker_copy_resizes\":%llu}\n",
    p->mode, (unsigned long long)p->capacity, p->slots,
    (unsigned long long)p->reads, (unsigned long long)p->bytes,
    (unsigned long long)p->publications, (unsigned long long)p->full_waits,
    (unsigned long long)p->spins, (unsigned long long)p->polls,
    (unsigned long long)p->worker_ns, (unsigned long long)p->callbacks,
    (unsigned long long)p->parsed, (unsigned long long)p->worker_bytes,
    (unsigned long long)p->worker_runs, (unsigned long long)p->worker_resizes,
    (unsigned long long)p->worker_rejected,
    (unsigned long long)p->worker_copy_resizes);
   fclose(f);
  }
  free(path);
 }
 close(p->notify[0]); close(p->notify[1]); close(p->wake[0]); close(p->wake[1]);
 pthread_cond_destroy(&p->space); pthread_mutex_destroy(&p->mutex);
 if (p->job_trace != NULL) fclose(p->job_trace);
 free(p->trace); free(p);
}
