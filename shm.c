// shm — bufory wl_shm dla urd (pasek, tapeta) i deco (cienie)
#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "norns.h"

static void buf_destroy(struct buf *b)
{
	pixman_image_unref(b->img);
	munmap(b->data, b->size);
	wl_buffer_destroy(b->wb);
	free(b);
}

/* Compositor oddał bufor: jednorazowy sprzątamy, wielokrotny wraca do puli. */
static void buf_release(void *data, struct wl_buffer *wb)
{
	struct buf *b = data;
	b->busy = false;
	if (!b->keep)
		buf_destroy(b);
}

static const struct wl_buffer_listener buf_listener = { .release = buf_release };

struct buf *buf_new(int w, int h, bool alpha, bool keep)
{
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("norns", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct buf *b = calloc(1, sizeof *b);
	b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
	                                  alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->keep = keep;
	b->img = pixman_image_create_bits(alpha ? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8, w, h, data, stride);
	wl_buffer_add_listener(b->wb, &buf_listener, b);
	return b;
}

/* Bufor przypięty do powierzchni: commit oddaje go compositorowi do czasu release. */
void buf_attach(struct buf *b, struct wl_surface *s)
{
	b->busy = true;
	wl_surface_attach(s, b->wb, 0, 0);
}

/* Bufor z puli przestaje być potrzebny; jeśli compositor go trzyma, zniknie przy release. */
void buf_drop(struct buf *b)
{
	if (!b)
		return;
	if (b->busy)
		b->keep = false;
	else
		buf_destroy(b);
}
