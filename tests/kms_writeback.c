/*
 * (C) COPYRIGHT 2017 ARM Limited. All rights reserved.
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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "igt.h"
#include "igt_fb.h"
#include "sw_sync.h"

/* We need to define these ourselves until we get an updated libdrm */
#ifndef DRM_MODE_CONNECTOR_WRITEBACK
#define DRM_MODE_CONNECTOR_WRITEBACK   18
#endif

static drmModePropertyBlobRes *get_writeback_formats_blob(igt_output_t *output)
{
	drmModePropertyBlobRes *blob = NULL;
	uint64_t blob_id;
	int ret;

	ret = kmstest_get_property(output->display->drm_fd,
				   output->config.connector->connector_id,
				   DRM_MODE_OBJECT_CONNECTOR,
				   igt_connector_prop_names[IGT_CONNECTOR_WRITEBACK_PIXEL_FORMATS],
				   NULL, &blob_id, NULL);
	if (ret)
		blob = drmModeGetPropertyBlob(output->display->drm_fd, blob_id);

	igt_assert(blob);

	return blob;
}

static uint32_t pick_writeback_format(igt_output_t *output)
{
	drmModePropertyBlobRes *wb_formats_blob = get_writeback_formats_blob(output);
	const uint32_t *wb_formats, *cairo_formats;
	uint32_t format = 0;
	int n_cairo_formats, n_wb_formats, i, j;

	igt_get_all_cairo_formats(&cairo_formats, &n_cairo_formats);

	wb_formats = wb_formats_blob->data;
	n_wb_formats = wb_formats_blob->length / sizeof(*wb_formats);
	for (i = 0; (i < n_wb_formats) && !format; i++) {
		for (j = 0; j < n_cairo_formats; j++) {
			if (wb_formats[i] == cairo_formats[j]) {
				format = wb_formats[i];
				break;
			}
		}
	}

	drmModeFreePropertyBlob(wb_formats_blob);

	igt_assert(format);
	return format;
}

static bool check_writeback_config(igt_display_t *display, igt_output_t *output,
				   int pipe, igt_output_t **clone)
{
	igt_fb_t input_fb, output_fb;
	igt_plane_t *plane;
	uint32_t writeback_format = pick_writeback_format(output);
	uint64_t tiling = igt_fb_mod_to_tiling(0);
	int width, height, ret;
	drmModeModeInfo override_mode = {
		.clock = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.hskew = 0,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
		.vscan = 0,
		.vrefresh = 60,
		.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
		.name = {"640x480-60"},
	};
	igt_output_override_mode(output, &override_mode);

	width = override_mode.hdisplay;
	height = override_mode.vdisplay;

	ret = igt_create_fb(display->drm_fd, width, height, DRM_FORMAT_XRGB8888,
			    tiling, &input_fb);
	igt_assert(ret >= 0);

	ret = igt_create_fb(display->drm_fd, width, height, writeback_format,
			    tiling, &output_fb);
	igt_assert(ret >= 0);

	plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(plane, &input_fb);
	igt_output_set_writeback_fb(output, &output_fb);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
					    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (!ret && clone) {
		/* Try and find a clone */
		int i, newret;
		*clone = NULL;

		for (i = 0; i < display->n_outputs; i++) {
			igt_output_t *second_output = &display->outputs[i];
			if (output != second_output &&
			    igt_pipe_connector_valid(pipe, second_output)) {

				igt_output_clone_pipe(second_output, pipe);
				newret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY |
								    DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
				igt_output_set_pipe(second_output, PIPE_NONE);
				if (!newret) {
					*clone = second_output;
					break;
				}
			}
		}
	}
	igt_plane_set_fb(plane, NULL);
	igt_remove_fb(display->drm_fd, &input_fb);
	igt_remove_fb(display->drm_fd, &output_fb);

	return !ret;
}

static igt_output_t *kms_writeback_get_output(igt_display_t *display, enum pipe *pipe,
					      igt_output_t **clone)
{
	int i;

	for (i = 0; i < display->n_outputs; i++) {
		igt_output_t *output = &display->outputs[i];
		int j;

		if (output->config.connector->connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;

		kmstest_force_connector(display->drm_fd, output->config.connector, FORCE_CONNECTOR_ON);

		for (j = 0; j < igt_display_get_n_pipes(display); j++) {
			igt_output_set_pipe(output, j);

			if (check_writeback_config(display, output, j, clone)) {
				igt_debug("Using connector %u:%s on pipe %d\n",
					  output->config.connector->connector_id,
					  output->name, j);
				if (clone && *clone)
					igt_debug("Cloning to connector %u:%s\n",
						  (*clone)->config.connector->connector_id,
						  (*clone)->name);
				if (pipe)
					*pipe = j;
				return output;
			}
		}

		/* Restore any connectors we don't use, so we don't trip on them later */
		kmstest_force_connector(display->drm_fd, output->config.connector, FORCE_CONNECTOR_UNSPECIFIED);
	}

	return NULL;
}

static void check_writeback_fb_id(igt_output_t *output)
{
	bool found;
	uint64_t check_fb_id;

	found = kmstest_get_property(output->display->drm_fd, output->id,
				     DRM_MODE_OBJECT_CONNECTOR,
				     igt_connector_prop_names[IGT_CONNECTOR_WRITEBACK_FB_ID],
				     NULL, &check_fb_id, NULL);
	igt_assert(found && (check_fb_id == 0));
}

static int do_writeback_test(igt_output_t *output, uint32_t flags,
			      uint32_t fb_id, int32_t *out_fence_ptr,
			      bool ptr_valid)
{
	int ret;
	enum pipe pipe;
	drmModeAtomicReq *req;
	igt_display_t *display = output->display;
	struct kmstest_connector_config *config = &output->config;

	req = drmModeAtomicAlloc();
	drmModeAtomicSetCursor(req, 0);

	for_each_pipe(display, pipe) {
		igt_pipe_t *pipe_obj = &display->pipes[pipe];
		igt_plane_t *plane;

		igt_atomic_prepare_crtc_commit(pipe_obj, req);

		for_each_plane_on_pipe(display, pipe, plane) {
			igt_atomic_prepare_plane_commit(plane, pipe_obj, req);
		}
	}

	igt_atomic_populate_connector_req(req, output, IGT_CONNECTOR_CRTC_ID, config->crtc->crtc_id);
	igt_atomic_populate_connector_req(req, output, IGT_CONNECTOR_WRITEBACK_FB_ID, fb_id);
	igt_atomic_populate_connector_req(req, output, IGT_CONNECTOR_WRITEBACK_OUT_FENCE_PTR, (uint64_t)out_fence_ptr);

	if (ptr_valid)
		*out_fence_ptr = 0;

	ret = drmModeAtomicCommit(display->drm_fd, req, flags, NULL);

	if (ptr_valid && (ret || (flags & DRM_MODE_ATOMIC_TEST_ONLY)))
		igt_assert(*out_fence_ptr == -1);

	drmModeAtomicFree(req);

	/* WRITEBACK_FB_ID must always read as zero */
	check_writeback_fb_id(output);

	return ret;
}

static void invalid_out_fence(igt_output_t *output, igt_fb_t *valid_fb, igt_fb_t *invalid_fb)
{
	int i, ret;
	int32_t out_fence;
	struct {
		uint32_t fb_id;
		bool ptr_valid;
		int32_t *out_fence_ptr;
	} invalid_tests[] = {
		{
			/* No output buffer, but the WRITEBACK_OUT_FENCE_PTR set. */
			.fb_id = 0,
			.ptr_valid = true,
			.out_fence_ptr = &out_fence,
		},
		{
			/* Invalid output buffer. */
			.fb_id = invalid_fb->fb_id,
			.ptr_valid = true,
			.out_fence_ptr = &out_fence,
		},
		{
			/* Invalid WRITEBACK_OUT_FENCE_PTR. */
			.fb_id = valid_fb->fb_id,
			.ptr_valid = false,
			.out_fence_ptr = (int32_t *)0x8,
		},
	};

	for (i = 0; i < ARRAY_SIZE(invalid_tests); i++) {
		ret = do_writeback_test(output, DRM_MODE_ATOMIC_ALLOW_MODESET,
					invalid_tests[i].fb_id,
					invalid_tests[i].out_fence_ptr,
					invalid_tests[i].ptr_valid);
		igt_assert(ret != 0);
	}
}

static void writeback_fb_id(igt_output_t *output, igt_fb_t *valid_fb, igt_fb_t *invalid_fb)
{

	int ret;

	/* Valid output buffer */
	ret = do_writeback_test(output, DRM_MODE_ATOMIC_ALLOW_MODESET,
				valid_fb->fb_id, NULL, false);
	igt_assert(ret == 0);

	/* Invalid object for WRITEBACK_FB_ID */
	ret = do_writeback_test(output, DRM_MODE_ATOMIC_ALLOW_MODESET,
				output->id, NULL, false);
	igt_assert(ret == -EINVAL);

	/* Zero WRITEBACK_FB_ID */
	ret = do_writeback_test(output, DRM_MODE_ATOMIC_ALLOW_MODESET,
				0, NULL, false);
	igt_assert(ret == 0);
}

static void fill_fb(igt_fb_t *fb, double color[3])
{
	cairo_t *cr = igt_get_cairo_ctx(fb->fd, fb);
	igt_assert(cr);

	igt_paint_color(cr, 0, 0, fb->width, fb->height,
			color[0], color[1], color[2]);
}

static void get_and_wait_out_fence(igt_output_t *output)
{
	int ret, out_fence = out_fence = igt_output_get_last_writeback_out_fence(output);
	igt_assert(out_fence >= 0);

	ret = sync_fence_wait(out_fence, 1000);
	igt_assert(ret == 0);
	close(out_fence);
}

static void writeback_seqence(igt_output_t *output, igt_plane_t *plane,
			      igt_fb_t *in_fb, igt_fb_t *out_fbs[], int n_commits)
{
	int i, color_idx = 0;
	double in_fb_colors[2][3] = {
		{ 1.0, 0.0, 0.0 },
		{ 0.0, 1.0, 0.0 },
	};
	double clear_color[3] = { 1.0, 1.0, 1.0 };
	igt_crc_t cleared_crc, out_expected;

	for (i = 0; i < n_commits; i++, color_idx++) {
		/* Change the input color each time */
		fill_fb(in_fb, in_fb_colors[color_idx % 2]);

		if (out_fbs[i]) {
			igt_crc_t out_before;

			/* Get the expected CRC */
			fill_fb(out_fbs[i], in_fb_colors[color_idx % 2]);
			igt_fb_get_crc(out_fbs[i], &out_expected);

			fill_fb(out_fbs[i], clear_color);
			if (i == 0)
				igt_fb_get_crc(out_fbs[i], &cleared_crc);
			igt_fb_get_crc(out_fbs[i], &out_before);
			igt_assert_crc_equal(&cleared_crc, &out_before);
		}

		/* Commit */
		igt_plane_set_fb(plane, in_fb);
		igt_output_set_writeback_fb(output, out_fbs[i]);
		if (out_fbs[i])
			igt_output_request_writeback_out_fence(output);
		igt_display_commit_atomic(output->display,
					  DRM_MODE_ATOMIC_ALLOW_MODESET,
					  NULL);
		if (out_fbs[i])
			get_and_wait_out_fence(output);

		/* Make sure the old output buffer is untouched */
		if (i > 0 && out_fbs[i - 1] && (out_fbs[i] != out_fbs[i - 1])) {
			igt_crc_t out_prev;
			igt_fb_get_crc(out_fbs[i - 1], &out_prev);
			igt_assert_crc_equal(&cleared_crc, &out_prev);
		}

		/* Make sure this output buffer is written */
		if (out_fbs[i]) {
			igt_crc_t out_after;
			igt_fb_get_crc(out_fbs[i], &out_after);
			igt_assert_crc_equal(&out_expected, &out_after);

			/* And clear it, for the next time */
			fill_fb(out_fbs[i], clear_color);
		}
	}
}

static void writeback_check_output(igt_output_t *output, igt_plane_t *plane,
				   igt_fb_t *input_fb, igt_fb_t *output_fb)
{
	igt_fb_t *out_fbs[2] = { 0 };
	igt_fb_t second_out_fb;
	int ret;

	/* One commit, with a writeback. */
	writeback_seqence(output, plane, input_fb, &output_fb, 1);

	/* Two commits, the second with no writeback */
	out_fbs[0] = output_fb;
	writeback_seqence(output, plane, input_fb, out_fbs, 2);

	/* Two commits, both with writeback */
	out_fbs[1] = output_fb;
	writeback_seqence(output, plane, input_fb, out_fbs, 2);

	ret = igt_create_fb(output_fb->fd, output_fb->width, output_fb->height,
			    DRM_FORMAT_XRGB8888,
			    igt_fb_mod_to_tiling(0),
			    &second_out_fb);
	igt_require(ret > 0);

	/* Two commits, with different writeback buffers */
	out_fbs[1] = &second_out_fb;
	writeback_seqence(output, plane, input_fb, out_fbs, 2);

	igt_remove_fb(output_fb->fd, &second_out_fb);
}

igt_main
{
	igt_display_t display;
	igt_output_t *output, *clone;
	igt_plane_t *plane;
	igt_fb_t input_fb;
	drmModeModeInfo mode;
	enum pipe pipe;
	int ret;

	memset(&display, 0, sizeof(display));

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_assert_fd(display.drm_fd);

		kmstest_set_vt_graphics_mode();

		igt_display_init(&display, display.drm_fd);

		igt_require(display.is_atomic);

		output = kms_writeback_get_output(&display, &pipe, &clone);
		igt_require(output);

		if (output->use_override_mode)
			memcpy(&mode, &output->override_mode, sizeof(mode));
		else
			memcpy(&mode, &output->config.default_mode, sizeof(mode));

		plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_require(plane);

		ret = igt_create_fb(display.drm_fd, mode.hdisplay,
				    mode.vdisplay,
				    DRM_FORMAT_XRGB8888,
				    igt_fb_mod_to_tiling(0),
				    &input_fb);
		igt_assert(ret >= 0);
		igt_plane_set_fb(plane, &input_fb);
	}

	igt_subtest("writeback-pixel-formats") {
		drmModePropertyBlobRes *formats_blob = get_writeback_formats_blob(output);
		const char *valid_chars = "0123456 ABCGNRUXY";
		unsigned int i;
		char *c;

		/*
		 * We don't have a comprehensive list of formats, so just check
		 * that the blob length is sensible and that it doesn't contain
		 * any outlandish characters
		 */
		igt_assert(!(formats_blob->length % 4));
		c = formats_blob->data;
		for (i = 0; i < formats_blob->length; i++)
			igt_assert_f(strchr(valid_chars, c[i]),
				     "Unexpected character %c\n", c[i]);
	}

	igt_subtest("writeback-invalid-out-fence") {
		igt_fb_t invalid_fb;
		ret = igt_create_fb(display.drm_fd, mode.hdisplay / 2,
				    mode.vdisplay / 2,
				    DRM_FORMAT_XRGB8888,
				    igt_fb_mod_to_tiling(0),
				    &invalid_fb);
		igt_require(ret > 0);

		invalid_out_fence(output, &input_fb, &invalid_fb);

		igt_remove_fb(display.drm_fd, &invalid_fb);
	}

	igt_subtest("writeback-fb-id") {
		igt_fb_t output_fb;
		ret = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				    DRM_FORMAT_XRGB8888,
				    igt_fb_mod_to_tiling(0),
				    &output_fb);
		igt_require(ret > 0);

		writeback_fb_id(output, &input_fb, &output_fb);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_subtest("writeback-check-output") {
		igt_fb_t output_fb;
		ret = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				    DRM_FORMAT_XRGB8888,
				    igt_fb_mod_to_tiling(0),
				    &output_fb);
		igt_require(ret > 0);

		writeback_check_output(output, plane, &input_fb, &output_fb);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_subtest("writeback-check-output-clone") {
		igt_fb_t output_fb;

		igt_require(clone);

		ret = igt_create_fb(display.drm_fd, mode.hdisplay, mode.vdisplay,
				    DRM_FORMAT_XRGB8888,
				    igt_fb_mod_to_tiling(0),
				    &output_fb);
		igt_require(ret > 0);

		igt_output_clone_pipe(clone, pipe);

		writeback_check_output(output, plane, &input_fb, &output_fb);

		igt_output_set_pipe(clone, PIPE_NONE);

		igt_remove_fb(display.drm_fd, &output_fb);
	}

	igt_fixture {
		igt_remove_fb(display.drm_fd, &input_fb);
		igt_display_fini(&display);
	}
}
