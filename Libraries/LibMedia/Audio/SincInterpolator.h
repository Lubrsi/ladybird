/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Audio {

// Interpolates a signal between its frames with a sinc low-pass shaped by a Blackman window. The kernel is tabulated
// at a number of phases per frame, and a position between two phases blends them linearly. Lowering the cutoff below
// the Nyquist frequency also removes what would alias when the interpolated signal is read faster than the original.
class MEDIA_API SincInterpolator {
public:
    // The kernel reaches this many frames to either side of the interpolated position.
    static constexpr size_t HALF_SPAN = 32;
    static constexpr size_t SPAN = (2 * HALF_SPAN) + 1;

    SincInterpolator(size_t phases, double cutoff);

    double cutoff() const { return m_cutoff; }

    // Interpolates `fraction` of the way from a frame to the next, given the SPAN frames centered on that frame.
    float interpolate(ReadonlySpan<float> frames_around, double fraction) const;

private:
    ReadonlySpan<float> phase_taps(size_t phase) const;

    size_t m_phases { 1 };
    double m_cutoff { 1 };
    FixedArray<float> m_taps;
};

}
