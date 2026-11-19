/* $OpenBSD: xmalloc.c,v 1.15 2026/07/12 20:16:20 nicm Exp $ */

/*
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Versions of malloc and friends that check their results, and never return
 * failure (they call fatalx if they encounter an error).
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

void *
xmalloc(size_t size)
{
	void *ptr;

	if (size == 0)
		fatalx("xmalloc: zero size");
	ptr = malloc(size);
	if (ptr == NULL)
		fatal("xmalloc: allocating %zu bytes", size);
	return ptr;
}

void *
xcalloc(size_t nmemb, size_t size)
{
	void *ptr;

	if (size == 0 || nmemb == 0)
		fatalx("xcalloc: zero size");
	if (SIZE_MAX / nmemb < size)
		fatalx("xcalloc: nmemb * size > SIZE_MAX");
	ptr = calloc(nmemb, size);
	if (ptr == NULL)
		fatal("xcalloc: allocating %zu bytes", size * nmemb);
	return ptr;
}

void *
xrealloc(void *ptr, size_t size)
{
	return xreallocarray(ptr, 1, size);
}

void *
xreallocarray(void *ptr, size_t nmemb, size_t size)
{
	void *new_ptr;

	if (nmemb == 0 || size == 0)
		fatalx("xreallocarray: zero size");
	new_ptr = reallocarray(ptr, nmemb, size);
	if (new_ptr == NULL)
		fatal("xreallocarray: allocating %zu bytes", size * nmemb);
	return new_ptr;
}

void *
xrecallocarray(void *ptr, size_t oldnmemb, size_t nmemb, size_t size)
{
	void *new_ptr;

	if (nmemb == 0 || size == 0)
		fatalx("xrecallocarray: zero size");
	new_ptr = recallocarray(ptr, oldnmemb, nmemb, size);
	if (new_ptr == NULL)
		fatal("xrecallocarray: allocating %zu bytes", size * nmemb);
	return new_ptr;
}

char *
xstrdup(const char *str)
{
	char *cp;

	if ((cp = strdup(str)) == NULL)
		fatal("xstrdup");
	return cp;
}

char *
xstrndup(const char *str, size_t maxlen)
{
	char *cp;

	if ((cp = strndup(str, maxlen)) == NULL)
		fatal("xstrndup");
	return cp;
}

char *
xmemdup(const void *ptr, size_t len)
{
	char *cp;

	cp = xmalloc(len + 1);
	if (len != 0)
		memcpy(cp, ptr, len);
	cp[len] = '\0';
	return cp;
}

int
xasprintf(char **ret, const char *fmt, ...)
{
	va_list ap;
	int i;

	va_start(ap, fmt);
	i = xvasprintf(ret, fmt, ap);
	va_end(ap);
	return i;
}

int
xvasprintf(char **ret, const char *fmt, va_list ap)
{
	int i;

	i = vasprintf(ret, fmt, ap);
	if (i == -1 || *ret == NULL)
		fatal("xvasprintf");
	return i;
}

int
xsnprintf(char *str, size_t len, const char *fmt, ...)
{
	va_list ap;
	int i;

	va_start(ap, fmt);
	i = xvsnprintf(str, len, fmt, ap);
	va_end(ap);
	return i;
}

int
xvsnprintf(char *str, size_t len, const char *fmt, va_list ap)
{
	int i;

	if (len > INT_MAX)
		fatalx("xsnprintf: len > INT_MAX");
	i = vsnprintf(str, len, fmt, ap);
	if (i < 0 || i >= (int)len)
		fatalx("xsnprintf: overflow");
	return i;
}

/*
 * Private size-class slabs. Four classes per doubling bound internal waste
 * to 25%; keep at most one entirely empty slab per class. Server thread only.
 * All pointers passed to this API must originate here, never in a library.
 */
struct xpool_slab;
union xpool_header {
	struct {
		struct xpool_slab *slab;
		union {
			union xpool_header *next;
			size_t capacity;
		} u;
	} h;
	long double align;
};
struct xpool_class {
	struct xpool_slab *available, *empty;
};
struct xpool_slab {
	struct xpool_slab *prev, *next;
	struct xpool_class *bucket;
	union xpool_header *first;
	size_t capacity;
	u_int count, remaining;
	long double align;
};
static struct xpool_class xpool_classes[44];
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XPOOL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(XPOOL_ASAN)
#include <sanitizer/asan_interface.h>
#define XPOOL_POISON(p, n) __asan_poison_memory_region(p, n)
#define XPOOL_UNPOISON(p, n) __asan_unpoison_memory_region(p, n)
#else
#define XPOOL_POISON(p, n) ((void)0)
#define XPOOL_UNPOISON(p, n) ((void)0)
#endif
static void
xpool_unlink(struct xpool_slab *s)
{
	if (s->prev != NULL)
		s->prev->next = s->next;
	else
		s->bucket->available = s->next;
	if (s->next != NULL)
		s->next->prev = s->prev;
}
static void
xpool_link(struct xpool_slab *s)
{
	s->prev = NULL;
	s->next = s->bucket->available;
	if (s->next != NULL)
		s->next->prev = s;
	s->bucket->available = s;
}
static void *
xpool_alloc(size_t size)
{
	struct xpool_class *bucket;
	struct xpool_slab *s;
	union xpool_header *h;
	size_t capacity, stride, value;
	u_int bits, index, count, i;
	if (size > 65536) {
		if (size > SIZE_MAX - sizeof *h)
			fatalx("xpool_alloc: overflow");
		h = xmalloc(sizeof *h + size);
		h->h.slab = NULL;
		h->h.u.capacity = size;
		return h + 1;
	}
	if (size <= 64) {
		index = (size - 1) / 16;
		capacity = (index + 1) * 16;
	} else {
		for (bits = 0, value = size - 1; value > 1; value >>= 1)
			bits++;
		index = 4 + (bits - 6) * 4 + ((size - 1) >> (bits - 2)) - 4;
		capacity = (((size - 1) >> (bits - 2)) + 1) << (bits - 2);
	}
	bucket = &xpool_classes[index];
	s = bucket->available;
	if (s == NULL) {
		stride = sizeof *h + capacity;
		count = 65536 / stride;
		if (count == 0)
			count = 1;
		if (count > 64)
			count = 64;
		s = xmalloc(sizeof *s + count * stride);
		s->bucket = bucket;
		s->capacity = capacity;
		s->count = s->remaining = count;
		s->first = NULL;
		for (i = 0; i < count; i++) {
			h = (void *)((char *)(s + 1) + i * stride);
			h->h.slab = s;
			h->h.u.next = s->first;
			s->first = h;
			XPOOL_POISON(h + 1, capacity);
		}
		xpool_link(s);
	}
	if (bucket->empty == s)
		bucket->empty = NULL;
	h = s->first;
	s->first = h->h.u.next;
	if (--s->remaining == 0)
		xpool_unlink(s);
	h->h.u.capacity = size;
	XPOOL_UNPOISON(h + 1, size);
	return h + 1;
}
void
xpool_free(void *ptr)
{
	union xpool_header *h;
	struct xpool_slab *s;
	if (ptr == NULL)
		return;
	h = (union xpool_header *)ptr - 1;
	s = h->h.slab;
	if (s == NULL) {
		free(h);
		return;
	}
	XPOOL_POISON(ptr, s->capacity);
	h->h.u.next = s->first;
	s->first = h;
	if (s->remaining++ == 0)
		xpool_link(s);
	if (s->remaining == s->count) {
		if (s->bucket->empty != NULL) {
			xpool_unlink(s);
			free(s);
		} else
			s->bucket->empty = s;
	}
}
void *
xpool_reallocarray(void *ptr, size_t nmemb, size_t size)
{
	union xpool_header *h;
	size_t capacity, previous;
	void *next;
	if (nmemb == 0 || size == 0 || nmemb > SIZE_MAX / size)
		fatalx("xpool_reallocarray: invalid size");
	size *= nmemb;
	if (ptr != NULL) {
		h = (union xpool_header *)ptr - 1;
		previous = h->h.u.capacity;
		capacity = h->h.slab != NULL ? h->h.slab->capacity : previous;
		if (size <= capacity && size >= capacity / 4) {
			XPOOL_UNPOISON(ptr, capacity);
			XPOOL_POISON((char *)ptr + size, capacity - size);
			h->h.u.capacity = size;
			return ptr;
		}
		if (h->h.slab == NULL && size > 65536) {
			if (size > SIZE_MAX - sizeof *h)
				fatalx("xpool_reallocarray: overflow");
			h = xrealloc(h, sizeof *h + size);
			h->h.u.capacity = size;
			return h + 1;
		}
	} else
		previous = 0;
	next = xpool_alloc(size);
	if (previous != 0)
		memcpy(next, ptr, size < previous ? size : previous);
	xpool_free(ptr);
	return next;
}
