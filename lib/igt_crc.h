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
 * # Pipe CRC Support
 *
 * This library wraps up the kernel's support for capturing pipe CRCs into a
 * neat and tidy package. For the detailed usage see all the functions which
 * work on #igt_pipe_crc_t. This is supported on all platforms and outputs.
 *
 * Actually using pipe CRCs to write modeset tests is a bit tricky though, so
 * there is no way to directly check a CRC: Both the details of the plane
 * blending, color correction and other hardware and how exactly the CRC is
 * computed at each tap point vary by hardware generation and are not disclosed.
 *
 * The only way to use #igt_crc_t CRCs therefore is to compare CRCs among each
 * another either for equality or difference. Otherwise CRCs must be treated as
 * completely opaque values. Note that not even CRCs from different pipes or tap
 * points on the same platform can be compared. Hence only use
 * igt_assert_crc_equal() to inspect CRC values captured by the same
 * #igt_pipe_crc_t object.
 */

#ifndef __IGT_CRC_H__
#define __IGT_CRC_H__

#include <stdbool.h>
#include <stdint.h>

enum pipe;

/**
 * igt_pipe_crc_t:
 *
 * Pipe CRC support structure. Needs to be allocated and set up with
 * igt_pipe_crc_new() for a specific pipe and pipe CRC source value.
 */
typedef struct _igt_pipe_crc igt_pipe_crc_t;

#define DRM_MAX_CRC_NR 10
/**
 * igt_crc_t:
 * @frame: frame number of the capture CRC
 * @n_words: internal field, don't access
 * @crc: internal field, don't access
 *
 * Pipe CRC value. All other members than @frame are private and should not be
 * inspected by testcases.
 */
typedef struct {
	uint32_t frame;
	bool has_valid_frame;
	int n_words;
	uint32_t crc[DRM_MAX_CRC_NR];
} igt_crc_t;

/**
 * intel_pipe_crc_source:
 * @INTEL_PIPE_CRC_SOURCE_NONE: No source
 * @INTEL_PIPE_CRC_SOURCE_PLANE1: Plane 1
 * @INTEL_PIPE_CRC_SOURCE_PLANE2: Plane 2
 * @INTEL_PIPE_CRC_SOURCE_PF: Panel Filter
 * @INTEL_PIPE_CRC_SOURCE_PIPE: Pipe
 * @INTEL_PIPE_CRC_SOURCE_TV: TV
 * @INTEL_PIPE_CRC_SOURCE_DP_B: DisplayPort B
 * @INTEL_PIPE_CRC_SOURCE_DP_C: DisplayPort C
 * @INTEL_PIPE_CRC_SOURCE_DP_D: DisplayPort D
 * @INTEL_PIPE_CRC_SOURCE_AUTO: Automatic source selection
 * @INTEL_PIPE_CRC_SOURCE_MAX: Number of available sources
 *
 * Enumeration of all supported pipe CRC sources. Not all platforms and all
 * outputs support all of them. Generic tests should just use
 * INTEL_PIPE_CRC_SOURCE_AUTO. It should always map to an end-of-pipe CRC
 * suitable for checking planes, cursor, color correction and any other
 * output-agnostic features.
 */
enum intel_pipe_crc_source {
        INTEL_PIPE_CRC_SOURCE_NONE,
        INTEL_PIPE_CRC_SOURCE_PLANE1,
        INTEL_PIPE_CRC_SOURCE_PLANE2,
        INTEL_PIPE_CRC_SOURCE_PF,
        INTEL_PIPE_CRC_SOURCE_PIPE,
        INTEL_PIPE_CRC_SOURCE_TV,
        INTEL_PIPE_CRC_SOURCE_DP_B,
        INTEL_PIPE_CRC_SOURCE_DP_C,
        INTEL_PIPE_CRC_SOURCE_DP_D,
        INTEL_PIPE_CRC_SOURCE_AUTO,
        INTEL_PIPE_CRC_SOURCE_MAX,
};

void igt_assert_crc_equal(const igt_crc_t *a, const igt_crc_t *b);
char *igt_crc_to_string(igt_crc_t *crc);

void igt_require_pipe_crc(int fd);
igt_pipe_crc_t *
igt_pipe_crc_new(int fd, enum pipe pipe, enum intel_pipe_crc_source source);
igt_pipe_crc_t *
igt_pipe_crc_new_nonblock(int fd, enum pipe pipe, enum intel_pipe_crc_source source);
void igt_pipe_crc_free(igt_pipe_crc_t *pipe_crc);
void igt_pipe_crc_start(igt_pipe_crc_t *pipe_crc);
void igt_pipe_crc_stop(igt_pipe_crc_t *pipe_crc);
__attribute__((warn_unused_result))
int igt_pipe_crc_get_crcs(igt_pipe_crc_t *pipe_crc, int n_crcs,
			  igt_crc_t **out_crcs);
void igt_pipe_crc_collect_crc(igt_pipe_crc_t *pipe_crc, igt_crc_t *out_crc);

#endif /* __IGT_CRC_H__ */
