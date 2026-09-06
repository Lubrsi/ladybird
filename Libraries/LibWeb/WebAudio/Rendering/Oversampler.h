/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>

namespace Web::WebAudio::Rendering {

// Raises the sample rate of a signal by an integer factor and lowers it again afterwards, so that a non-linear
// operation applied in between produces its distortion products at the higher rate, where the ones above the original
// Nyquist frequency can be filtered out instead of aliasing into the audible band. Both stages share one windowed-sinc
// low-pass filter, and together they delay the signal by a whole number of frames.
class Oversampler {
public:
    explicit Oversampler(size_t factor);

    size_t factor() const { return m_factor; }
    void set_channel_count(size_t);

    // Interpolates one input frame of a channel into `factor` samples.
    void upsample(size_t channel, float sample, Span<float> upsampled);

    // Decimates `factor` samples of a channel back into one output frame.
    float downsample(size_t channel, ReadonlySpan<float> upsampled);

private:
    struct ChannelState {
        Vector<float> input_history;
        Vector<float> upsampled_history;
        size_t input_position { 0 };
        size_t upsampled_position { 0 };
    };

    size_t m_factor { 1 };
    Vector<float> m_kernel;
    Vector<ChannelState> m_channel_states;
};

}
