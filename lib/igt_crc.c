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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "igt_aux.h"
#include "igt_crc.h"
#include "igt_core.h"
#include "igt_debugfs.h"
#include "igt_kms.h"

/**
 * igt_assert_crc_equal:
 * @a: first pipe CRC value
 * @b: second pipe CRC value
 *
 * Compares two CRC values and fails the testcase if they don't match with
 * igt_fail(). Note that due to CRC collisions CRC based testcase can only
 * assert that CRCs match, never that they are different. Otherwise there might
 * be random testcase failures when different screen contents end up with the
 * same CRC by chance.
 */
void igt_assert_crc_equal(const igt_crc_t *a, const igt_crc_t *b)
{
	int i;

	for (i = 0; i < a->n_words; i++)
		igt_assert_eq_u32(a->crc[i], b->crc[i]);
}

/**
 * igt_crc_to_string:
 * @crc: pipe CRC value to print
 *
 * This formats @crc into a string buffer which is owned by igt_crc_to_string().
 * The next call will override the buffer again, which makes this multithreading
 * unsafe.
 *
 * This should only ever be used for diagnostic debug output.
 */
char *igt_crc_to_string(igt_crc_t *crc)
{
	int i;
	char buf[128] = { 0 };

	for (i = 0; i < crc->n_words; i++)
		sprintf(buf + strlen(buf), "%08x ", crc->crc[i]);

	return strdup(buf);
}

#define MAX_CRC_ENTRIES 10
#define MAX_LINE_LEN (10 + 11 * MAX_CRC_ENTRIES + 1)

/* (6 fields, 8 chars each, space separated (5) + '\n') */
#define LEGACY_LINE_LEN       (6 * 8 + 5 + 1)

struct _igt_pipe_crc {
	int fd;
	int dir;
	int ctl_fd;
	int crc_fd;
	int flags;
	bool is_legacy;

	enum pipe pipe;
	enum intel_pipe_crc_source source;
};

static const char *pipe_crc_sources[] = {
	"none",
	"plane1",
	"plane2",
	"pf",
	"pipe",
	"TV",
	"DP-B",
	"DP-C",
	"DP-D",
	"auto"
};

static const char *pipe_crc_source_name(enum intel_pipe_crc_source source)
{
        return pipe_crc_sources[source];
}

static bool igt_pipe_crc_do_start(igt_pipe_crc_t *pipe_crc)
{
	char buf[64];

	/* Stop first just to make sure we don't have lingering state left. */
	igt_pipe_crc_stop(pipe_crc);

	if (pipe_crc->is_legacy)
		sprintf(buf, "pipe %s %s", kmstest_pipe_name(pipe_crc->pipe),
			pipe_crc_source_name(pipe_crc->source));
	else
		sprintf(buf, "%s", pipe_crc_source_name(pipe_crc->source));

	igt_assert_eq(write(pipe_crc->ctl_fd, buf, strlen(buf)), strlen(buf));

	if (!pipe_crc->is_legacy) {
		int err;

		sprintf(buf, "crtc-%d/crc/data", pipe_crc->pipe);
		err = 0;

		pipe_crc->crc_fd = openat(pipe_crc->dir, buf, pipe_crc->flags);
		if (pipe_crc->crc_fd < 0)
			err = -errno;

		if (err == -EINVAL)
			return false;

		igt_assert_eq(err, 0);
	}

	errno = 0;
	return true;
}

static void igt_pipe_crc_pipe_off(int fd, enum pipe pipe)
{
	char buf[32];

	sprintf(buf, "pipe %s none", kmstest_pipe_name(pipe));
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));
}

static void igt_pipe_crc_reset(int drm_fd)
{
	struct dirent *dirent;
	const char *cmd = "none";
	bool done = false;
	DIR *dir;
	int fdir;
	int fd;

	fdir = igt_debugfs_dir(drm_fd);
	if (fdir < 0)
		return;

	dir = fdopendir(fdir);
	if (!dir) {
		close(fdir);
		return;
	}

	while ((dirent = readdir(dir))) {
		char buf[128];

		if (strcmp(dirent->d_name, "crtc-") != 0)
			continue;

		sprintf(buf, "%s/crc/control", dirent->d_name);
		fd = openat(fdir, buf, O_WRONLY);
		if (fd < 0)
			continue;

		igt_assert_eq(write(fd, cmd, strlen(cmd)), strlen(cmd));
		close(fd);

		done = true;
	}
	closedir(dir);

	if (!done) {
		fd = openat(fdir, "i915_display_crtc_ctl", O_WRONLY);
		if (fd != -1) {
			igt_pipe_crc_pipe_off(fd, PIPE_A);
			igt_pipe_crc_pipe_off(fd, PIPE_B);
			igt_pipe_crc_pipe_off(fd, PIPE_C);

			close(fd);
		}
	}

	close(fdir);
}

static void pipe_crc_exit_handler(int sig)
{
	struct dirent *dirent;
	char buf[128];
	DIR *dir;
	int fd;

	dir = opendir("/dev/dri");
	if (!dir)
		return;

	/*
	 * Try to reset CRC capture for all DRM devices, this is only needed
	 * for the legacy CRC ABI and can be completely removed once the
	 * legacy codepaths are removed.
	 */
	while ((dirent = readdir(dir))) {
		if (strncmp(dirent->d_name, "card", 4) != 0)
			continue;

		sprintf(buf, "/dev/dri/%s", dirent->d_name);
		fd = open(buf, O_WRONLY);

		igt_pipe_crc_reset(fd);

		close(fd);
	}
	closedir(dir);
}

/**
 * igt_require_pipe_crc:
 *
 * Convenience helper to check whether pipe CRC capturing is supported by the
 * kernel. Uses igt_skip to automatically skip the test/subtest if this isn't
 * the case.
 */
void igt_require_pipe_crc(int fd)
{
	const char *cmd = "pipe A none";
	int ctl, written;

	ctl = igt_debugfs_open(fd, "crtc-0/crc/control", O_RDONLY);
	if (ctl < 0) {
		ctl = igt_debugfs_open(fd, "i915_display_crc_ctl", O_WRONLY);
		igt_require_f(ctl,
			      "No display_crc_ctl found, kernel too old\n");

		written = write(ctl, cmd, strlen(cmd));
		igt_require_f(written < 0,
			      "CRCs not supported on this platform\n");
	}
	close(ctl);
}

static igt_pipe_crc_t *
pipe_crc_new(int fd, enum pipe pipe, enum intel_pipe_crc_source source, int flags)
{
	igt_pipe_crc_t *pipe_crc;
	char buf[128];
	int debugfs;

	debugfs = igt_debugfs_dir(fd);
	igt_assert(debugfs != -1);

	igt_install_exit_handler(pipe_crc_exit_handler);

	pipe_crc = calloc(1, sizeof(struct _igt_pipe_crc));

	sprintf(buf, "crtc-%d/crc/control", pipe);
	pipe_crc->ctl_fd = openat(debugfs, buf, O_WRONLY);
	if (pipe_crc->ctl_fd == -1) {
		pipe_crc->ctl_fd = openat(debugfs,
					  "i915_display_crc_ctl", O_WRONLY);
		igt_assert(pipe_crc->ctl_fd != -1);
		pipe_crc->is_legacy = true;
	}

	if (pipe_crc->is_legacy) {
		sprintf(buf, "i915_pipe_%s_crc", kmstest_pipe_name(pipe));
		pipe_crc->crc_fd = openat(debugfs, buf, flags);
		igt_assert(pipe_crc->crc_fd != -1);
		igt_debug("Using legacy frame CRC ABI\n");
	} else {
		pipe_crc->crc_fd = -1;
		igt_debug("Using generic frame CRC ABI\n");
	}

	pipe_crc->fd = fd;
	pipe_crc->dir = debugfs;
	pipe_crc->pipe = pipe;
	pipe_crc->source = source;
	pipe_crc->flags = flags;

	return pipe_crc;
}

/**
 * igt_pipe_crc_new:
 * @pipe: display pipe to use as source
 * @source: CRC tap point to use as source
 *
 * This sets up a new pipe CRC capture object for the given @pipe and @source
 * in blocking mode.
 *
 * Returns: A pipe CRC object for the given @pipe and @source. The library
 * assumes that the source is always available since recent kernels support at
 * least INTEL_PIPE_CRC_SOURCE_AUTO everywhere.
 */
igt_pipe_crc_t *
igt_pipe_crc_new(int fd, enum pipe pipe, enum intel_pipe_crc_source source)
{
	return pipe_crc_new(fd, pipe, source, O_RDONLY);
}

/**
 * igt_pipe_crc_new_nonblock:
 * @pipe: display pipe to use as source
 * @source: CRC tap point to use as source
 *
 * This sets up a new pipe CRC capture object for the given @pipe and @source
 * in nonblocking mode.
 *
 * Returns: A pipe CRC object for the given @pipe and @source. The library
 * assumes that the source is always available since recent kernels support at
 * least INTEL_PIPE_CRC_SOURCE_AUTO everywhere.
 */
igt_pipe_crc_t *
igt_pipe_crc_new_nonblock(int fd, enum pipe pipe, enum intel_pipe_crc_source source)
{
	return pipe_crc_new(fd, pipe, source, O_RDONLY | O_NONBLOCK);
}

/**
 * igt_pipe_crc_free:
 * @pipe_crc: pipe CRC object
 *
 * Frees all resources associated with @pipe_crc.
 */
void igt_pipe_crc_free(igt_pipe_crc_t *pipe_crc)
{
	if (!pipe_crc)
		return;

	close(pipe_crc->ctl_fd);
	close(pipe_crc->crc_fd);
	close(pipe_crc->dir);
	free(pipe_crc);
}

static bool pipe_crc_init_from_string(igt_pipe_crc_t *pipe_crc, igt_crc_t *crc,
				      const char *line)
{
	int n, i;
	const char *buf;

	if (pipe_crc->is_legacy) {
		crc->has_valid_frame = true;
		crc->n_words = 5;
		n = sscanf(line, "%8u %8x %8x %8x %8x %8x", &crc->frame,
			   &crc->crc[0], &crc->crc[1], &crc->crc[2],
			   &crc->crc[3], &crc->crc[4]);
		return n == 6;
	}

	if (strncmp(line, "XXXXXXXXXX", 10) == 0)
		crc->has_valid_frame = false;
	else {
		crc->has_valid_frame = true;
		crc->frame = strtoul(line, NULL, 16);
	}

	buf = line + 10;
	for (i = 0; *buf != '\n'; i++, buf += 11)
		crc->crc[i] = strtoul(buf, NULL, 16);

	crc->n_words = i;

	return true;
}

static int read_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out)
{
	ssize_t bytes_read;
	char buf[MAX_LINE_LEN + 1];
	size_t read_len;

	if (pipe_crc->is_legacy)
		read_len = LEGACY_LINE_LEN;
	else
		read_len = MAX_LINE_LEN;

	igt_set_timeout(5, "CRC reading");
	bytes_read = read(pipe_crc->crc_fd, &buf, read_len);
	igt_reset_timeout();

	if (bytes_read < 0 && errno == EAGAIN)
		igt_assert(pipe_crc->flags & O_NONBLOCK);

	if (bytes_read < 0)
		bytes_read = 0;

	buf[bytes_read] = '\0';

	if (bytes_read && !pipe_crc_init_from_string(pipe_crc, out, buf))
		return -EINVAL;

	return bytes_read;
}

static void read_one_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out)
{
	while (read_crc(pipe_crc, out) == 0)
		usleep(1000);
}

/**
 * igt_pipe_crc_start:
 * @pipe_crc: pipe CRC object
 *
 * Starts the CRC capture process on @pipe_crc.
 */
void igt_pipe_crc_start(igt_pipe_crc_t *pipe_crc)
{
	igt_crc_t crc;

	igt_assert(igt_pipe_crc_do_start(pipe_crc));

	if (pipe_crc->is_legacy) {
		/*
		 * For some no yet identified reason, the first CRC is
		 * bonkers. So let's just wait for the next vblank and read
		 * out the buggy result.
		 *
		 * On CHV sometimes the second CRC is bonkers as well, so
		 * don't trust that one either.
		 */
		read_one_crc(pipe_crc, &crc);
		read_one_crc(pipe_crc, &crc);
	}
}

/**
 * igt_pipe_crc_stop:
 * @pipe_crc: pipe CRC object
 *
 * Stops the CRC capture process on @pipe_crc.
 */
void igt_pipe_crc_stop(igt_pipe_crc_t *pipe_crc)
{
	char buf[32];

	if (pipe_crc->is_legacy) {
		sprintf(buf, "pipe %s none", kmstest_pipe_name(pipe_crc->pipe));
		igt_assert_eq(write(pipe_crc->ctl_fd, buf, strlen(buf)),
			      strlen(buf));
	} else {
		close(pipe_crc->crc_fd);
		pipe_crc->crc_fd = -1;
	}
}

/**
 * igt_pipe_crc_get_crcs:
 * @pipe_crc: pipe CRC object
 * @n_crcs: number of CRCs to capture
 * @out_crcs: buffer pointer for the captured CRC values
 *
 * Read up to @n_crcs from @pipe_crc. This function does not block, and will
 * return early if not enough CRCs can be captured, if @pipe_crc has been
 * opened using igt_pipe_crc_new_nonblock(). It will block until @n_crcs are
 * retrieved if @pipe_crc has been opened using igt_pipe_crc_new(). @out_crcs is
 * alloced by this function and must be released with free() by the caller.
 *
 * Callers must start and stop the capturing themselves by calling
 * igt_pipe_crc_start() and igt_pipe_crc_stop(). For one-shot CRC collecting
 * look at igt_pipe_crc_collect_crc().
 *
 * Returns:
 * The number of CRCs captured. Should be equal to @n_crcs in blocking mode, but
 * can be less (even zero) in non-blocking mode.
 */
int
igt_pipe_crc_get_crcs(igt_pipe_crc_t *pipe_crc, int n_crcs,
		      igt_crc_t **out_crcs)
{
	igt_crc_t *crcs;
	int n = 0;

	crcs = calloc(n_crcs, sizeof(igt_crc_t));

	do {
		igt_crc_t *crc = &crcs[n];
		int ret;

		ret = read_crc(pipe_crc, crc);
		if (ret < 0)
			continue;
		if (ret == 0)
			break;

		n++;
	} while (n < n_crcs);

	*out_crcs = crcs;
	return n;
}

static void crc_sanity_checks(igt_crc_t *crc)
{
	int i;
	bool all_zero = true;

	for (i = 0; i < crc->n_words; i++) {
		igt_warn_on_f(crc->crc[i] == 0xffffffff,
			      "Suspicious CRC: it looks like the CRC "
			      "read back was from a register in a powered "
			      "down well\n");
		if (crc->crc[i])
			all_zero = false;
	}

	igt_warn_on_f(all_zero, "Suspicious CRC: All values are 0.\n");
}

/**
 * igt_pipe_crc_collect_crc:
 * @pipe_crc: pipe CRC object
 * @out_crc: buffer for the captured CRC values
 *
 * Read a single CRC from @pipe_crc. This function blocks until the CRC is
 * retrieved, irrespective of whether @pipe_crc has been opened with
 * igt_pipe_crc_new() or igt_pipe_crc_new_nonblock().  @out_crc must be
 * allocated by the caller.
 *
 * This function takes care of the pipe_crc book-keeping, it will start/stop
 * the collection of the CRC.
 *
 * This function also calls the interactive debug with the "crc" domain, so you
 * can make use of this feature to actually see the screen that is being CRC'd.
 *
 * For continuous CRC collection look at igt_pipe_crc_start(),
 * igt_pipe_crc_get_crcs() and igt_pipe_crc_stop().
 */
void igt_pipe_crc_collect_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc)
{
	igt_debug_wait_for_keypress("crc");

	igt_pipe_crc_start(pipe_crc);
	read_one_crc(pipe_crc, out_crc);
	igt_pipe_crc_stop(pipe_crc);

	crc_sanity_checks(out_crc);
}
