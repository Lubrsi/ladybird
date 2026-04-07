/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Span.h>
#include <AK/Vector.h>

namespace Audio {

class TimeStretchProcessor {
public:
    struct ProcessResult {
        size_t input_frames_consumed;
        size_t output_frames_produced;
    };

    TimeStretchProcessor();

    void initialize(u32 sample_rate, u8 channel_count);
    void reset();
    void flush_input();

    void set_rate(double rate) { m_rate = rate; }
    void set_preserves_pitch(bool preserves_pitch) { m_preserves_pitch = preserves_pitch; }

    double rate() const { return m_rate; }
    bool preserves_pitch() const { return m_preserves_pitch; }

    size_t input_frames_needed(size_t desired_output_frames) const;

    ProcessResult process(ReadonlySpan<float> input, Span<float> output);

private:
    ProcessResult process_wsola(ReadonlySpan<float> input, Span<float> output);
    ProcessResult process_resample(ReadonlySpan<float> input, Span<float> output);

    size_t find_best_overlap_position(ReadonlySpan<float> segment, ReadonlySpan<float> search_region) const;
    static float cross_correlation(ReadonlySpan<float> a, ReadonlySpan<float> b, size_t length);

    static constexpr size_t WINDOW_SIZE = 1024;
    static constexpr size_t SYNTHESIS_HOP = WINDOW_SIZE / 2;
    static constexpr size_t SEARCH_RANGE = 128;

    u32 m_sample_rate { 0 };
    u8 m_channel_count { 0 };
    double m_rate { 1.0 };
    bool m_preserves_pitch { true };

    // Pre-computed Hann window
    Array<float, WINDOW_SIZE> m_hann_window;

    // WSOLA state
    Vector<float> m_input_buffer;
    size_t m_input_buffer_fill { 0 };
    Vector<float> m_overlap_buffer; // Tail of previous output window for cross-correlation
    bool m_has_overlap { false };
    double m_input_position { 0 }; // Fractional input position tracking

    // Resampling state
    double m_resample_position { 0 };
};

}
