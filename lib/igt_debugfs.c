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

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_kms.h"
#include "igt_debugfs.h"
#include "igt_sysfs.h"

/**
 * SECTION:igt_debugfs
 * @short_description: Support code for debugfs features
 * @title: debugfs
 * @include: igt.h
 *
 * This library provides helpers to access debugfs features. On top of some
 * basic functions to access debugfs files with e.g. igt_debugfs_open() it also
 * provides higher-level wrappers for some debugfs features.
 *
 * # Other debugfs interface wrappers
 *
 * This covers the miscellaneous debugfs interface wrappers:
 *
 * - drm/i915 supports interfaces to evict certain classes of gem buffer
 *   objects, see igt_drop_caches_set().
 *
 * - drm/i915 supports an interface to disable prefaulting, useful to test
 *   slow paths in ioctls. See igt_disable_prefault().
 */

/*
 * General debugfs helpers
 */

static bool is_mountpoint(const char *path)
{
	char buf[strlen(path) + 4];
	dev_t dot_dev, dotdot_dev;
	struct stat st;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/.", path), sizeof(buf));
	igt_assert_eq(stat(buf, &st), 0);
	dot_dev = st.st_dev;

	igt_assert_lt(snprintf(buf, sizeof(buf), "%s/..", path), sizeof(buf));
	igt_assert_eq(stat(buf, &st), 0);
	dotdot_dev = st.st_dev;

	return dot_dev != dotdot_dev;
}

/**
 * igt_debugfs_mount:
 *
 * This attempts to locate where debugfs is mounted on the filesystem,
 * and if not found, will then try to mount debugfs at /sys/kernel/debug.
 *
 * Returns:
 * The path to the debugfs mount point (e.g. /sys/kernel/debug)
 */
const char *igt_debugfs_mount(void)
{
	struct stat st;

	if (stat("/debug/dri", &st) == 0)
		return "/debug";

	if (stat("/sys/kernel/debug/dri", &st) == 0)
		return "/sys/kernel/debug";

	igt_assert(is_mountpoint("/sys/kernel/debug") ||
		   mount("debug", "/sys/kernel/debug", "debugfs", 0, 0) == 0);

	return "/sys/kernel/debug";
}

/**
 * igt_debugfs_dir:
 * @device: fd of the device
 *
 * This opens the debugfs directory corresponding to device for use
 * with igt_sysfs_get() and related functions.
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_debugfs_dir(int device)
{
	struct stat st;
	const char *debugfs_root;
	char path[200];
	int idx;

	if (fstat(device, &st)) {
		igt_debug("Couldn't stat FD for DRM device: %s\n", strerror(errno));
		return -1;
	}

	if (!S_ISCHR(st.st_mode)) {
		igt_debug("FD for DRM device not a char device!\n");
		return -1;
	}

	debugfs_root = igt_debugfs_mount();

	idx = minor(st.st_rdev);
	snprintf(path, sizeof(path), "%s/dri/%d/name", debugfs_root, idx);
	if (stat(path, &st))
		return -1;

	if (idx >= 64) {
		int file, name_len, cmp_len;
		char name[100], cmp[100];

		file = open(path, O_RDONLY);
		if (file < 0)
			return -1;

		name_len = read(file, name, sizeof(name));
		close(file);

		for (idx = 0; idx < 16; idx++) {
			snprintf(path, sizeof(path), "%s/dri/%d/name",
				 debugfs_root, idx);
			file = open(path, O_RDONLY);
			if (file < 0)
				return -1;

			cmp_len = read(file, cmp, sizeof(cmp));
			close(file);

			if (cmp_len == name_len && !memcmp(cmp, name, name_len))
				break;
		}

		if (idx == 16)
			return -1;
	}

	snprintf(path, sizeof(path), "%s/dri/%d", debugfs_root, idx);
	igt_debug("Opening debugfs directory '%s'\n", path);
	return open(path, O_RDONLY);
}

/**
 * igt_debugfs_open:
 * @filename: name of the debugfs node to open
 * @mode: mode bits as used by open()
 *
 * This opens a debugfs file as a Unix file descriptor. The filename should be
 * relative to the drm device's root, i.e. without "drm/<minor>".
 *
 * Returns:
 * The Unix file descriptor for the debugfs file or -1 if that didn't work out.
 */
int igt_debugfs_open(int device, const char *filename, int mode)
{
	int dir, ret;

	dir = igt_debugfs_dir(device);
	if (dir < 0)
		return dir;

	ret = openat(dir, filename, mode);

	close(dir);

	return ret;
}

/**
 * __igt_debugfs_read:
 * @filename: file name
 * @buf: buffer where the contents will be stored, allocated by the caller
 * @buf_size: size of the buffer
 *
 * This function opens the debugfs file, reads it, stores the content in the
 * provided buffer, then closes the file. Users should make sure that the buffer
 * provided is big enough to fit the whole file, plus one byte.
 */
void __igt_debugfs_read(int fd, const char *filename, char *buf, int buf_size)
{
	int dir;
	int len;

	dir = igt_debugfs_dir(fd);
	len = igt_sysfs_read(dir, filename, buf, buf_size - 1);
	if (len < 0)
		len = 0;
	buf[len] = '\0';
	close(dir);
}

/**
 * igt_debugfs_search:
 * @filename: file name
 * @substring: string to search for in @filename
 *
 * Searches each line in @filename for the substring specified in @substring.
 *
 * Returns: True if the @substring is found to occur in @filename
 */
bool igt_debugfs_search(int device, const char *filename, const char *substring)
{
	FILE *file;
	size_t n = 0;
	char *line = NULL;
	bool matched = false;
	int fd;

	fd = igt_debugfs_open(device, filename, O_RDONLY);
	file = fdopen(fd, "r");
	igt_assert(file);

	while (getline(&line, &n, file) >= 0) {
		matched = strstr(line, substring) != NULL;
		if (matched)
			break;
	}

	free(line);
	fclose(file);
	close(fd);

	return matched;
}

static void igt_hpd_storm_exit_handler(int sig)
{
	int fd = drm_open_driver_master(DRIVER_INTEL);

	/* Here we assume that only one i915 device will be ever present */
	igt_hpd_storm_reset(fd);

	close(fd);
}

/**
 * igt_hpd_storm_set_threshold:
 * @threshold: How many hotplugs per second required to trigger an HPD storm,
 * or 0 to disable storm detection.
 *
 * Convienence helper to configure the HPD storm detection threshold for i915
 * through debugfs. Useful for hotplugging tests where HPD storm detection
 * might get in the way and slow things down.
 *
 * If the system does not support HPD storm detection, this function does
 * nothing.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_hpd_storm_set_threshold(int drm_fd, unsigned int threshold)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_WRONLY);
	char buf[16];

	if (fd < 0)
		return;

	igt_debug("Setting HPD storm threshold to %d\n", threshold);
	snprintf(buf, sizeof(buf), "%d", threshold);
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));

	close(fd);
	igt_install_exit_handler(igt_hpd_storm_exit_handler);
}

/**
 * igt_hpd_storm_reset:
 *
 * Convienence helper to reset HPD storm detection to it's default settings.
 * If hotplug detection was disabled on any ports due to an HPD storm, it will
 * be immediately re-enabled. Always called on exit if the HPD storm detection
 * threshold was modified during any tests.
 *
 * If the system does not support HPD storm detection, this function does
 * nothing.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_hpd_storm_reset(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_WRONLY);
	const char *buf = "reset";

	if (fd < 0)
		return;

	igt_debug("Resetting HPD storm threshold\n");
	igt_assert_eq(write(fd, buf, strlen(buf)), strlen(buf));

	close(fd);
}

/**
 * igt_hpd_storm_detected:
 *
 * Checks whether or not i915 has detected an HPD interrupt storm on any of the
 * system's ports.
 *
 * This function always returns false on systems that do not support HPD storm
 * detection.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 *
 * Returns: Whether or not an HPD storm has been detected.
 */
bool igt_hpd_storm_detected(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_RDONLY);
	char *start_loc;
	char buf[32] = {0}, detected_str[4];
	bool ret;

	if (fd < 0)
		return false;

	igt_assert_lt(0, read(fd, buf, sizeof(buf)));
	igt_assert(start_loc = strstr(buf, "Detected: "));
	igt_assert_eq(sscanf(start_loc, "Detected: %s\n", detected_str), 1);

	if (strcmp(detected_str, "yes") == 0)
		ret = true;
	else if (strcmp(detected_str, "no") == 0)
		ret = false;
	else
		igt_fail_on_f(true, "Unknown hpd storm detection status '%s'\n",
			      detected_str);

	close(fd);
	return ret;
}

/**
 * igt_require_hpd_storm_ctl:
 *
 * Skips the current test if the system does not have HPD storm detection.
 *
 * See: https://01.org/linuxgraphics/gfx-docs/drm/gpu/i915.html#hotplug
 */
void igt_require_hpd_storm_ctl(int drm_fd)
{
	int fd = igt_debugfs_open(drm_fd, "i915_hpd_storm_ctl", O_RDONLY);

	igt_require_f(fd > 0, "No i915_hpd_storm_ctl found in debugfs\n");
	close(fd);
}

/*
 * Drop caches
 */

/**
 * igt_drop_caches_has:
 * @val: bitmask for DROP_* values
 *
 * This queries the debugfs to see if it supports the full set of desired
 * operations.
 */
bool igt_drop_caches_has(int drm_fd, uint64_t val)
{
	uint64_t mask;
	int dir;

	mask = 0;
	dir = igt_debugfs_dir(drm_fd);
	igt_sysfs_scanf(dir, "i915_gem_drop_caches", "0x%" PRIx64, &mask);
	close(dir);

	return (val & mask) == val;
}

/**
 * igt_drop_caches_set:
 * @val: bitmask for DROP_* values
 *
 * This calls the debugfs interface the drm/i915 GEM driver exposes to drop or
 * evict certain classes of gem buffer objects.
 */
void igt_drop_caches_set(int drm_fd, uint64_t val)
{
	int fd;
	char data[19];
	size_t nbytes;

	sprintf(data, "0x%" PRIx64, val);

	fd = igt_debugfs_open(drm_fd, "i915_gem_drop_caches", O_WRONLY);

	igt_assert(fd >= 0);
	do {
		nbytes = write(fd, data, strlen(data) + 1);
	} while (nbytes == -1 && (errno == EINTR || errno == EAGAIN));
	igt_assert(nbytes == strlen(data) + 1);
	close(fd);
}

/*
 * Prefault control
 */

#define PREFAULT_DEBUGFS "/sys/module/i915/parameters/prefault_disable"
static void igt_prefault_control(bool enable)
{
	const char *name = PREFAULT_DEBUGFS;
	int fd;
	char buf[2] = {'Y', 'N'};
	int index;

	fd = open(name, O_RDWR);
	igt_require(fd >= 0);

	if (enable)
		index = 1;
	else
		index = 0;

	igt_require(write(fd, &buf[index], 1) == 1);

	close(fd);
}

static void enable_prefault_at_exit(int sig)
{
	igt_enable_prefault();
}

/**
 * igt_disable_prefault:
 *
 * Disable prefaulting in certain gem ioctls through the debugfs interface. As
 * usual this installs an exit handler to clean up and re-enable prefaulting
 * even when the test exited abnormally.
 *
 * igt_enable_prefault() will enable normale operation again.
 */
void igt_disable_prefault(void)
{
	igt_prefault_control(false);

	igt_install_exit_handler(enable_prefault_at_exit);
}

/**
 * igt_enable_prefault:
 *
 * Enable prefault (again) through the debugfs interface.
 */
void igt_enable_prefault(void)
{
	igt_prefault_control(true);
}

static int get_object_count(int fd)
{
	int dir, ret, scanned;

	igt_drop_caches_set(fd, DROP_RETIRE | DROP_ACTIVE | DROP_FREED);

	dir = igt_debugfs_dir(fd);
	scanned = igt_sysfs_scanf(dir, "i915_gem_objects",
				  "%i objects", &ret);
	igt_assert_eq(scanned, 1);
	close(dir);

	return ret;
}

/**
 * igt_get_stable_obj_count:
 * @driver: fd to drm/i915 GEM driver
 *
 * This puts the driver into a stable (quiescent) state and then returns the
 * current number of gem buffer objects as reported in the i915_gem_objects
 * debugFS interface.
 */
int igt_get_stable_obj_count(int driver)
{
	int obj_count;
	gem_quiescent_gpu(driver);
	obj_count = get_object_count(driver);
	/* The test relies on the system being in the same state before and
	 * after the test so any difference in the object count is a result of
	 * leaks during the test. gem_quiescent_gpu() mostly achieves this but
	 * on android occasionally obj_count can still change briefly.
	 * The loop ensures obj_count has remained stable over several checks
	 */
#ifdef ANDROID
	{
		int loop_count = 0;
		int prev_obj_count = obj_count;
		while (loop_count < 4) {
			usleep(200000);
			gem_quiescent_gpu(driver);
			obj_count = get_object_count(driver);
			if (obj_count == prev_obj_count) {
				loop_count++;
			} else {
				igt_debug("loop_count=%d, obj_count=%d, prev_obj_count=%d\n",
					loop_count, obj_count, prev_obj_count);
				loop_count = 0;
				prev_obj_count = obj_count;
			}

		}
	}
#endif
	return obj_count;
}

void igt_debugfs_dump(int device, const char *filename)
{
	char *contents;
	int dir;

	dir = igt_debugfs_dir(device);
	contents = igt_sysfs_get(dir, filename);
	close(dir);

	igt_debug("%s:\n%s\n", filename, contents);
	free(contents);
}
