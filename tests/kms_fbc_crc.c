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

#include "igt.h"
#include "igt_crc.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


IGT_TEST_DESCRIPTION(
   "Performs various write operations to the scanout buffer while FBC is "
   "enabled. CRC checks will be used to make sure the modifications to scanout "
   "buffer are detected.");

enum test_mode {
	TEST_PAGE_FLIP,
	TEST_MMAP_CPU,
	TEST_MMAP_GTT,
	TEST_BLT,
	TEST_RENDER,
	TEST_CONTEXT,
	TEST_PAGE_FLIP_AND_MMAP_CPU,
	TEST_PAGE_FLIP_AND_MMAP_GTT,
	TEST_PAGE_FLIP_AND_BLT,
	TEST_PAGE_FLIP_AND_RENDER,
	TEST_PAGE_FLIP_AND_CONTEXT,
};

typedef struct {
	int drm_fd;
	igt_crc_t ref_crc[4];
	igt_pipe_crc_t *pipe_crc;
	drm_intel_bufmgr *bufmgr;
	drm_intel_context *ctx[2];
	uint32_t devid;
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	igt_plane_t *primary;
	struct igt_fb fb[2];
} data_t;

static const char *test_mode_str(enum test_mode mode)
{
	static const char * const test_modes[] = {
		[TEST_PAGE_FLIP] = "page_flip",
		[TEST_MMAP_CPU] = "mmap_cpu",
		[TEST_MMAP_GTT] = "mmap_gtt",
		[TEST_BLT] = "blt",
		[TEST_RENDER] = "render",
		[TEST_CONTEXT] = "context",
		[TEST_PAGE_FLIP_AND_MMAP_CPU] = "page_flip_and_mmap_cpu",
		[TEST_PAGE_FLIP_AND_MMAP_GTT] = "page_flip_and_mmap_gtt",
		[TEST_PAGE_FLIP_AND_BLT] = "page_flip_and_blt",
		[TEST_PAGE_FLIP_AND_RENDER] = "page_flip_and_render",
		[TEST_PAGE_FLIP_AND_CONTEXT] = "page_flip_and_context",
	};

	return test_modes[mode];
}

static void fill_blt(data_t *data,
		     uint32_t handle,
		     struct igt_fb *fb,
		     unsigned char color)
{
	drm_intel_bo *dst = gem_handle_to_libdrm_bo(data->bufmgr,
						    data->drm_fd,
						    "", handle);
	struct intel_batchbuffer *batch;
	unsigned flags;
	int pitch;
	uint32_t pixel = color | (color << 8) | (color << 16) | (color << 24);

	batch = intel_batchbuffer_alloc(data->bufmgr, data->devid);
	igt_assert(batch);

	pitch = fb->stride;
	flags = XY_COLOR_BLT_WRITE_ALPHA |
		XY_COLOR_BLT_WRITE_RGB;
	if (fb->tiling && batch->gen >= 4) {
		flags |= XY_COLOR_BLT_TILED;
		pitch /= 4;
	}

	COLOR_BLIT_COPY_BATCH_START(flags);
	OUT_BATCH(3 << 24 | 0xf0 << 16 | pitch);
	OUT_BATCH(0);
	OUT_BATCH(1 << 16 | 1);
	OUT_RELOC_FENCED(dst, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, 0);
	OUT_BATCH(pixel);
	ADVANCE_BATCH();

	intel_batchbuffer_flush(batch);
	intel_batchbuffer_free(batch);

	gem_bo_busy(data->drm_fd, handle);
}

static void scratch_buf_init(struct igt_buf *buf, drm_intel_bo *bo)
{
	buf->bo = bo;
	buf->stride = 4096;
	buf->tiling = I915_TILING_X;
	buf->size = 4096;
}

static void exec_nop(data_t *data, uint32_t handle, drm_intel_context *context)
{
	drm_intel_bo *dst;
	struct intel_batchbuffer *batch;

	dst = gem_handle_to_libdrm_bo(data->bufmgr, data->drm_fd, "", handle);
	igt_assert(dst);

	batch = intel_batchbuffer_alloc(data->bufmgr, data->devid);
	igt_assert(batch);

	/* add the reloc to make sure the kernel will think we write to dst */
	BEGIN_BATCH(4, 1);
	OUT_BATCH(MI_BATCH_BUFFER_END);
	OUT_BATCH(MI_NOOP);
	OUT_RELOC(dst, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, 0);
	OUT_BATCH(MI_NOOP);
	ADVANCE_BATCH();

	intel_batchbuffer_flush_with_context(batch, context);
	intel_batchbuffer_free(batch);
}

static void fill_render(data_t *data, uint32_t handle,
			drm_intel_context *context, unsigned char color)
{
	drm_intel_bo *src, *dst;
	struct intel_batchbuffer *batch;
	struct igt_buf src_buf, dst_buf;
	const uint8_t buf[4] = { color, color, color, color };
	igt_render_copyfunc_t rendercopy = igt_get_render_copyfunc(data->devid);

	igt_skip_on(!rendercopy);

	dst = gem_handle_to_libdrm_bo(data->bufmgr, data->drm_fd, "", handle);
	igt_assert(dst);

	src = drm_intel_bo_alloc(data->bufmgr, "", 4096, 4096);
	igt_assert(src);

	gem_write(data->drm_fd, src->handle, 0, buf, 4);

	scratch_buf_init(&src_buf, src);
	scratch_buf_init(&dst_buf, dst);

	batch = intel_batchbuffer_alloc(data->bufmgr, data->devid);
	igt_assert(batch);

	rendercopy(batch, context,
		   &src_buf, 0, 0, 1, 1,
		   &dst_buf, 0, 0);

	intel_batchbuffer_free(batch);

	gem_bo_busy(data->drm_fd, handle);
}

static void fill_mmap_cpu(data_t *data, uint32_t handle, unsigned char color)
{
	void *ptr;

	ptr = gem_mmap__cpu(data->drm_fd, handle, 0, 4096, PROT_WRITE);
	gem_set_domain(data->drm_fd, handle, I915_GEM_DOMAIN_CPU,
		       I915_GEM_DOMAIN_CPU);
	memset(ptr, color, 4);
	munmap(ptr, 4096);
	gem_sw_finish(data->drm_fd, handle);
}

static void fill_mmap_gtt(data_t *data, uint32_t handle, unsigned char color)
{
	void *ptr;

	ptr = gem_mmap__gtt(data->drm_fd, handle, 4096, PROT_WRITE);
	gem_set_domain(data->drm_fd, handle, I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);
	memset(ptr, color, 4);
	munmap(ptr, 4096);
}

static bool fbc_enabled(data_t *data)
{
	char str[128] = {};

	igt_debugfs_read(data->drm_fd, "i915_fbc_status", str);
	return strstr(str, "FBC enabled") != NULL;
}

static bool wait_for_fbc_enabled(data_t *data)
{
	return igt_wait(fbc_enabled(data), 3000, 30);
}

static void check_crc(data_t *data, enum test_mode mode)
{
	igt_pipe_crc_t *pipe_crc = data->pipe_crc;
	igt_crc_t crc, *ref_crc;

	switch (mode) {
	case TEST_PAGE_FLIP:
		ref_crc = &data->ref_crc[1];
		break;
	case TEST_MMAP_CPU:
	case TEST_MMAP_GTT:
	case TEST_BLT:
	case TEST_RENDER:
	case TEST_CONTEXT:
		ref_crc = &data->ref_crc[2];
		break;
	case TEST_PAGE_FLIP_AND_MMAP_CPU:
	case TEST_PAGE_FLIP_AND_MMAP_GTT:
	case TEST_PAGE_FLIP_AND_BLT:
	case TEST_PAGE_FLIP_AND_RENDER:
	case TEST_PAGE_FLIP_AND_CONTEXT:
		ref_crc = &data->ref_crc[3];
		break;
	default:
		igt_assert(false);
	}

	igt_pipe_crc_collect_crc(pipe_crc, &crc);
	igt_assert_crc_equal(&crc, ref_crc);
}

static void test_crc(data_t *data, enum test_mode mode)
{
	uint32_t crtc_id = data->output->config.crtc->crtc_id;
	uint32_t handle = data->fb[0].gem_handle;
	drm_intel_context *context = NULL;

	igt_assert(fbc_enabled(data));

	if (mode == TEST_PAGE_FLIP || mode >= TEST_PAGE_FLIP_AND_MMAP_CPU) {
		handle = data->fb[1].gem_handle;
		igt_assert(drmModePageFlip(data->drm_fd, crtc_id,
					   data->fb[1].fb_id, 0, NULL) == 0);

		if (mode != TEST_PAGE_FLIP)
			igt_assert(wait_for_fbc_enabled(data));
	}

	switch (mode) {
	case TEST_PAGE_FLIP:
		break;
	case TEST_MMAP_CPU:
	case TEST_PAGE_FLIP_AND_MMAP_CPU:
		fill_mmap_cpu(data, handle, 0xff);
		break;
	case TEST_MMAP_GTT:
	case TEST_PAGE_FLIP_AND_MMAP_GTT:
		fill_mmap_gtt(data, handle, 0xff);
		break;
	case TEST_BLT:
	case TEST_PAGE_FLIP_AND_BLT:
		fill_blt(data, handle, data->fb, ~0);
		break;
	case TEST_CONTEXT:
	case TEST_PAGE_FLIP_AND_CONTEXT:
		context = data->ctx[1];
	case TEST_RENDER:
	case TEST_PAGE_FLIP_AND_RENDER:
		fill_render(data, handle, context, 0xff);
		break;
	}

	/*
	 * Make sure we're looking at new data (two vblanks
	 * to leave some leeway for the kernel if we ever do
	 * some kind of delayed FBC disable for GTT mmaps.
	 */
	igt_wait_for_vblank(data->drm_fd, data->pipe);
	igt_wait_for_vblank(data->drm_fd, data->pipe);

	check_crc(data, mode);

	/*
	 * Allow time for FBC to kick in again if it
	 * got disabled during dirtyfb or page flip.
	 */
	igt_assert(wait_for_fbc_enabled(data));

	check_crc(data, mode);
}

static void prepare_crtc(data_t *data)
{
	igt_output_t *output = data->output;

	igt_output_set_pipe(output, data->pipe);
}

static void create_fbs(data_t *data, bool tiled, struct igt_fb *fbs)
{
	int rc;
	drmModeModeInfo *mode = igt_output_get_mode(data->output);
	uint64_t tiling = tiled ? LOCAL_I915_FORMAT_MOD_X_TILED :
				  LOCAL_DRM_FORMAT_MOD_NONE;

	rc = igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
				 DRM_FORMAT_XRGB8888, tiling,
				 0.0, 0.0, 0.0, &fbs[0]);
	igt_assert(rc);
	rc = igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
				 DRM_FORMAT_XRGB8888, tiling,
				 0.1, 0.1, 0.1, &fbs[1]);
	igt_assert(rc);
}

/* Since we want to be really safe that the CRCs are actually what we really
 * want, use untiled FBs, so FBC won't happen to disrupt things. Also do the
 * drawing before setting the modes, just to be sure. */
static void get_ref_crcs(data_t *data)
{
	igt_display_t *display = &data->display;
	struct igt_fb fbs[4];
	int i;

	create_fbs(data, false, &fbs[0]);
	create_fbs(data, false, &fbs[2]);

	fill_mmap_gtt(data, fbs[2].gem_handle, 0xff);
	fill_mmap_gtt(data, fbs[3].gem_handle, 0xff);

	for (i = 0; i < 4; i++) {
		igt_plane_set_fb(data->primary, &fbs[i]);
		igt_display_commit(display);
		igt_wait_for_vblank(data->drm_fd, data->pipe);
		igt_assert(!fbc_enabled(data));
		igt_pipe_crc_collect_crc(data->pipe_crc, &data->ref_crc[i]);
		igt_assert(!fbc_enabled(data));
	}

	igt_plane_set_fb(data->primary, &data->fb[1]);
	igt_display_commit(display);

	for (i = 0; i < 4; i++)
		igt_remove_fb(data->drm_fd, &fbs[i]);
}

static bool prepare_test(data_t *data, enum test_mode test_mode)
{
	igt_display_t *display = &data->display;
	igt_output_t *output = data->output;
	igt_pipe_crc_t *pipe_crc;

	data->primary = igt_output_get_plane_type(data->output, DRM_PLANE_TYPE_PRIMARY);

	create_fbs(data, true, data->fb);

	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;
	pipe_crc = igt_pipe_crc_new(data->drm_fd, data->pipe,
				    INTEL_PIPE_CRC_SOURCE_AUTO);
	data->pipe_crc = pipe_crc;

	get_ref_crcs(data);

	/* scanout = fb[1] */
	igt_plane_set_fb(data->primary, &data->fb[1]);
	igt_display_commit(display);

	if (!wait_for_fbc_enabled(data)) {
		igt_info("FBC not enabled\n");

		igt_plane_set_fb(data->primary, NULL);
		igt_output_set_pipe(output, PIPE_ANY);
		igt_display_commit(display);

		igt_remove_fb(data->drm_fd, &data->fb[0]);
		igt_remove_fb(data->drm_fd, &data->fb[1]);
		return false;
	}

	if (test_mode == TEST_CONTEXT || test_mode == TEST_PAGE_FLIP_AND_CONTEXT) {
		data->ctx[0] = drm_intel_gem_context_create(data->bufmgr);
		igt_assert(data->ctx[0]);
		data->ctx[1] = drm_intel_gem_context_create(data->bufmgr);
		igt_assert(data->ctx[1]);

		/*
		 * Disable FBC RT address for both contexts
		 * (by "rendering" to a non-scanout buffer).
		 */
		exec_nop(data, data->fb[0].gem_handle, data->ctx[1]);
		exec_nop(data, data->fb[0].gem_handle, data->ctx[0]);
		exec_nop(data, data->fb[0].gem_handle, data->ctx[1]);
		exec_nop(data, data->fb[0].gem_handle, data->ctx[0]);
	}

	/* scanout = fb[0] */
	igt_plane_set_fb(data->primary, &data->fb[0]);
	igt_display_commit(display);

	igt_assert(wait_for_fbc_enabled(data));

	if (test_mode == TEST_CONTEXT || test_mode == TEST_PAGE_FLIP_AND_CONTEXT) {
		/*
		 * make ctx[0] FBC RT address point to fb[0], ctx[1]
		 * FBC RT address is left as disabled.
		 */
		exec_nop(data, data->fb[0].gem_handle, data->ctx[0]);
		igt_assert(wait_for_fbc_enabled(data));
	}

	igt_wait_for_vblank(data->drm_fd, data->pipe);

	return true;
}

static void finish_crtc(data_t *data, enum test_mode mode)
{
	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	if (mode == TEST_CONTEXT || mode == TEST_PAGE_FLIP_AND_CONTEXT) {
		drm_intel_gem_context_destroy(data->ctx[0]);
		drm_intel_gem_context_destroy(data->ctx[1]);
	}

	igt_plane_set_fb(data->primary, NULL);
	igt_output_set_pipe(data->output, PIPE_ANY);
	igt_display_commit(&data->display);

	igt_remove_fb(data->drm_fd, &data->fb[0]);
	igt_remove_fb(data->drm_fd, &data->fb[1]);
}

static void reset_display(data_t *data)
{
	igt_display_t *display = &data->display;
	enum pipe pipe_id;

	for_each_pipe(display, pipe_id) {
        igt_pipe_t *pipe = &display->pipes[pipe_id];
		igt_plane_t *plane = igt_pipe_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);

		if (plane->fb)
			igt_plane_set_fb(plane, NULL);
	}

	for_each_connected_output(display, data->output)
		igt_output_set_pipe(data->output, PIPE_ANY);
}

static void run_test(data_t *data, enum test_mode mode)
{
	igt_display_t *display = &data->display;
	int valid_tests = 0;

	if (mode == TEST_CONTEXT || mode == TEST_PAGE_FLIP_AND_CONTEXT) {
		drm_intel_context *ctx = drm_intel_gem_context_create(data->bufmgr);
		igt_require(ctx);
		drm_intel_gem_context_destroy(ctx);
	}

	reset_display(data);

	for_each_pipe_with_valid_output(display, data->pipe, data->output) {
		prepare_crtc(data);

		igt_info("Beginning %s on pipe %s, connector %s\n",
			  igt_subtest_name(),
			  kmstest_pipe_name(data->pipe),
			  igt_output_name(data->output));

		if (!prepare_test(data, mode)) {
			igt_info("%s on pipe %s, connector %s: SKIPPED\n",
				  igt_subtest_name(),
				  kmstest_pipe_name(data->pipe),
				  igt_output_name(data->output));
			continue;
		}

		valid_tests++;

		test_crc(data, mode);

		igt_info("%s on pipe %s, connector %s: PASSED\n",
			  igt_subtest_name(),
			  kmstest_pipe_name(data->pipe),
			  igt_output_name(data->output));

		finish_crtc(data, mode);
	}

	igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
}

igt_main
{
	data_t data = {};
	enum test_mode mode;

	igt_skip_on_simulation();

	igt_fixture {
		char buf[128];

		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();

		data.devid = intel_get_drm_devid(data.drm_fd);

		igt_require_pipe_crc(data.drm_fd);

		igt_debugfs_read(data.drm_fd, "i915_fbc_status", buf);
		igt_require_f(!strstr(buf, "unsupported on this chipset"),
			      "FBC not supported\n");

		if (intel_gen(data.devid) >= 6)
			igt_set_module_param_int("enable_fbc", 1);

		data.bufmgr = drm_intel_bufmgr_gem_init(data.drm_fd, 4096);
		igt_assert(data.bufmgr);
		drm_intel_bufmgr_gem_enable_reuse(data.bufmgr);

		igt_display_init(&data.display, data.drm_fd);
	}

	for (mode = TEST_PAGE_FLIP; mode <= TEST_PAGE_FLIP_AND_CONTEXT; mode++) {
		igt_subtest_f("%s", test_mode_str(mode)) {
			run_test(&data, mode);
		}
	}

	igt_fixture {
		drm_intel_bufmgr_destroy(data.bufmgr);
		igt_display_fini(&data.display);
	}
}
