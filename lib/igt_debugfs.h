/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef __IGT_DEBUGFS_H__
#define __IGT_DEBUGFS_H__

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

const char *igt_debugfs_mount(void);

int igt_debugfs_dir(int device);

int igt_debugfs_open(int fd, const char *filename, int mode);
void __igt_debugfs_read(int fd, const char *filename, char *buf, int buf_size);
bool igt_debugfs_search(int fd, const char *filename, const char *substring);

/**
 * igt_debugfs_read:
 * @filename: name of the debugfs file
 * @buf: buffer where the contents will be stored, allocated by the caller.
 *
 * This is just a convenience wrapper for __igt_debugfs_read. See its
 * documentation.
 */
#define igt_debugfs_read(fd, filename, buf) \
		__igt_debugfs_read(fd, (filename), (buf), sizeof(buf))

void igt_hpd_storm_set_threshold(int fd, unsigned int threshold);
void igt_hpd_storm_reset(int fd);
bool igt_hpd_storm_detected(int fd);
void igt_require_hpd_storm_ctl(int fd);

/*
 * Drop caches
 */

/**
 * DROP_UNBOUND:
 *
 * Drop all currently unbound gem buffer objects from the cache.
 */
#define DROP_UNBOUND 0x1
/**
 * DROP_BOUND:
 *
 * Drop all inactive objects which are bound into some gpu address space.
 */
#define DROP_BOUND 0x2
/**
 * DROP_RETIRE:
 *
 * Wait for all outstanding gpu commands to complete, but do not take any
 * further actions.
 */
#define DROP_RETIRE 0x4
/**
 * DROP_ACTIVE:
 *
 * Also drop active objects once retired.
 */
#define DROP_ACTIVE 0x8
/**
 * DROP_FREED:
 *
 * Also drop freed objects.
 */
#define DROP_FREED 0x10
/**
 * DROP_SHRINK_ALL:
 *
 * Force all unpinned buffers to be evicted from their GTT and returned to the
 * system.
 */
#define DROP_SHRINK_ALL 0x20
/**
 * DROP_ALL:
 *
 * All of the above DROP_ flags combined.
 */
#define DROP_ALL (DROP_UNBOUND | \
		  DROP_BOUND | \
		  DROP_SHRINK_ALL | \
		  DROP_RETIRE | \
		  DROP_ACTIVE | \
		  DROP_FREED)

bool igt_drop_caches_has(int fd, uint64_t val);
void igt_drop_caches_set(int fd, uint64_t val);

/*
 * Prefault control
 */

void igt_disable_prefault(void);
void igt_enable_prefault(void);

/*
 * Put the driver into a stable (quiescent) state and get the current number of
 * gem buffer objects
 */
int igt_get_stable_obj_count(int driver);
void igt_debugfs_dump(int device, const char *filename);

#endif /* __IGT_DEBUGFS_H__ */
