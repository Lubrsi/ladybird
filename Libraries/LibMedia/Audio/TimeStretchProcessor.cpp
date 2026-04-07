/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TimeStretchProcessor.h"

#include <AK/Math.h>
#include <AK/SIMD.h>

namespace Audio {

TimeStretchProcessor::TimeStretchProcessor()
{
    for (size_t i = 0; i < WINDOW_SIZE; i++)
        m_hann_window[i] = 0.5f * (1.0f - AK::cos(2.0f * AK::Pi<float> * static_cast<float>(i) / static_cast<float>(WINDOW_SIZE - 1)));
}

void TimeStretchProcessor::initialize(u32 sample_rate, u8 channel_count)
{
    m_sample_rate = sample_rate;
    m_channel_count = channel_count;

    // Input buffer needs to hold enough for one WSOLA iteration:
    // window + search range on both sides, with room for accumulation across callbacks.
    auto input_buffer_size = (WINDOW_SIZE + 2 * SEARCH_RANGE) * 4 * channel_count;
    m_input_buffer.resize(input_buffer_size);
    m_input_buffer_fill = 0;

    m_overlap_buffer.resize(WINDOW_SIZE * channel_count);
    m_has_overlap = false;

    m_input_position = 0;
    m_resample_position = 0;
}

void TimeStretchProcessor::reset()
{
    m_input_buffer_fill = 0;
    m_has_overlap = false;
    m_input_position = 0;
    m_resample_position = 0;
}

void TimeStretchProcessor::flush_input()
{
    // Clear accumulated input and position, but keep the overlap buffer
    // so the next window can crossfade smoothly against the previous output.
    m_input_buffer_fill = 0;
    m_input_position = 0;
    m_resample_position = 0;
}

size_t TimeStretchProcessor::input_frames_needed(size_t desired_output_frames) const
{
    if (m_rate == 1.0)
        return desired_output_frames;

    auto base = static_cast<size_t>(static_cast<double>(desired_output_frames) * m_rate) + 2;

    // When the WSOLA internal buffer is low (e.g. after a seek), request enough
    // extra input to cover the window + search range overhead needed to start
    // producing output. Once the buffer has accumulated, this overhead drops to zero.
    if (m_preserves_pitch) {
        auto buffered_frames = m_input_buffer_fill / m_channel_count;
        auto wsola_overhead = WINDOW_SIZE + SEARCH_RANGE;
        if (buffered_frames < wsola_overhead)
            base += wsola_overhead - buffered_frames;
    }

    return base;
}

TimeStretchProcessor::ProcessResult TimeStretchProcessor::process(ReadonlySpan<float> input, Span<float> output)
{
    if (m_rate == 1.0) {
        auto frames_copied = input.copy_trimmed_to(output);
        auto frame_count = frames_copied / m_channel_count;
        return { frame_count, frame_count };
    }

    if (m_preserves_pitch)
        return process_wsola(input, output);
    return process_resample(input, output);
}

TimeStretchProcessor::ProcessResult TimeStretchProcessor::process_wsola(ReadonlySpan<float> input, Span<float> output)
{
    auto const channels = m_channel_count;
    auto const window_data_size = WINDOW_SIZE * channels;
    auto const synthesis_hop_data_size = SYNTHESIS_HOP * channels;
    double const analysis_hop = SYNTHESIS_HOP * m_rate;

    // Append new input to our internal buffer.
    auto input_to_copy = min(input.size(), m_input_buffer.size() - m_input_buffer_fill);
    input.trim(input_to_copy).copy_to(m_input_buffer.span().slice(m_input_buffer_fill));
    m_input_buffer_fill += input_to_copy;

    size_t total_input_consumed_from_buffer = 0;
    size_t output_written = 0;
    auto output_frames = output.size() / channels;

    while (output_written + SYNTHESIS_HOP <= output_frames) {
        // Check if we have enough input for this iteration.
        auto nominal_position = static_cast<size_t>(m_input_position);
        auto required_end = (nominal_position + WINDOW_SIZE + SEARCH_RANGE) * channels;
        if (required_end > m_input_buffer_fill)
            break;

        // Find the best overlap position using cross-correlation.
        size_t best_position;
        if (m_has_overlap) {
            auto search_start = (nominal_position > SEARCH_RANGE) ? nominal_position - SEARCH_RANGE : 0;
            auto search_end_frame = min(nominal_position + SEARCH_RANGE, (m_input_buffer_fill / channels) - WINDOW_SIZE);
            if (search_start >= search_end_frame) {
                best_position = nominal_position;
            } else {
                auto search_region_start = search_start * channels;
                auto search_region_size = (search_end_frame - search_start + WINDOW_SIZE) * channels;
                search_region_size = min(search_region_size, m_input_buffer_fill - search_region_start);

                auto search_region = m_input_buffer.span().slice(search_region_start, search_region_size);
                auto segment = m_overlap_buffer.span().slice(0, window_data_size);
                auto best_offset = find_best_overlap_position(segment, search_region);
                best_position = search_start + best_offset;
            }
        } else {
            best_position = nominal_position;
        }

        // Extract the window at best_position, apply Hann window, and overlap-add to output.
        auto window_start = best_position * channels;
        auto output_start = output_written * channels;

        if (m_has_overlap) {
            // Overlap-add: crossfade between previous overlap tail and new window.
            for (size_t i = 0; i < SYNTHESIS_HOP; i++) {
                auto window_weight = m_hann_window[i];
                for (u8 ch = 0; ch < channels; ch++) {
                    auto idx = (i * channels) + ch;
                    output[output_start + idx] = (m_overlap_buffer[idx + synthesis_hop_data_size] * (1.0f - window_weight))
                        + (m_input_buffer[window_start + idx] * window_weight);
                }
            }
        } else {
            // First window: just apply the Hann window directly.
            for (size_t i = 0; i < SYNTHESIS_HOP; i++) {
                auto window_weight = m_hann_window[i];
                for (u8 ch = 0; ch < channels; ch++) {
                    auto idx = (i * channels) + ch;
                    output[output_start + idx] = m_input_buffer[window_start + idx] * window_weight;
                }
            }
        }

        // Store the current window as the overlap buffer for the next iteration.
        m_input_buffer.span().slice(window_start, window_data_size).copy_to(m_overlap_buffer.span());
        m_has_overlap = true;

        output_written += SYNTHESIS_HOP;
        m_input_position += analysis_hop;
        total_input_consumed_from_buffer = static_cast<size_t>(m_input_position) * channels;
    }

    // Shift unconsumed input to the beginning of the buffer.
    auto consumed_data = min(total_input_consumed_from_buffer, m_input_buffer_fill);
    if (consumed_data > 0 && consumed_data < m_input_buffer_fill) {
        auto remaining = m_input_buffer_fill - consumed_data;
        AK::TypedTransfer<float>::move(m_input_buffer.data(), m_input_buffer.data() + consumed_data, remaining);
        m_input_buffer_fill = remaining;
        m_input_position -= static_cast<double>(consumed_data) / static_cast<double>(channels);
    } else if (consumed_data >= m_input_buffer_fill) {
        m_input_buffer_fill = 0;
        m_input_position = 0;
    }

    // The number of input frames consumed from the caller's perspective is based on the rate.
    // We consumed input_to_copy floats from the caller's input buffer.
    auto input_frames_consumed = input_to_copy / channels;

    return { input_frames_consumed, output_written };
}

size_t TimeStretchProcessor::find_best_overlap_position(ReadonlySpan<float> segment, ReadonlySpan<float> search_region) const
{
    auto const channels = m_channel_count;
    auto const segment_length = SYNTHESIS_HOP * channels;

    // Compare against the second half of the segment (the overlap tail).
    auto overlap_tail = segment.slice(SYNTHESIS_HOP * channels, segment_length);

    float best_correlation = -1.0f;
    size_t best_offset = 0;

    auto max_offset = (search_region.size() >= segment_length) ? (search_region.size() - segment_length) / channels : 0;

    for (size_t offset = 0; offset <= max_offset; offset++) {
        auto candidate = search_region.slice(offset * channels, segment_length);
        auto correlation = cross_correlation(overlap_tail, candidate, segment_length);
        if (correlation > best_correlation) {
            best_correlation = correlation;
            best_offset = offset;
        }
    }

    return best_offset;
}

float TimeStretchProcessor::cross_correlation(ReadonlySpan<float> a, ReadonlySpan<float> b, size_t length)
{
    VERIFY(length <= a.size());
    VERIFY(length <= b.size());

    using AK::SIMD::f32x4;

    auto const* pa = a.data();
    auto const* pb = b.data();

    f32x4 vec_ab = { 0.0f, 0.0f, 0.0f, 0.0f };
    f32x4 vec_aa = { 0.0f, 0.0f, 0.0f, 0.0f };
    f32x4 vec_bb = { 0.0f, 0.0f, 0.0f, 0.0f };

    size_t i = 0;
    auto const simd_end = length & ~3u;
    for (; i < simd_end; i += 4) {
        f32x4 va = { pa[i], pa[i + 1], pa[i + 2], pa[i + 3] };
        f32x4 vb = { pb[i], pb[i + 1], pb[i + 2], pb[i + 3] };
        vec_ab += va * vb;
        vec_aa += va * va;
        vec_bb += vb * vb;
    }

    float sum_ab = vec_ab[0] + vec_ab[1] + vec_ab[2] + vec_ab[3];
    float sum_aa = vec_aa[0] + vec_aa[1] + vec_aa[2] + vec_aa[3];
    float sum_bb = vec_bb[0] + vec_bb[1] + vec_bb[2] + vec_bb[3];

    // Handle remaining elements.
    for (; i < length; i++) {
        sum_ab += pa[i] * pb[i];
        sum_aa += pa[i] * pa[i];
        sum_bb += pb[i] * pb[i];
    }

    constexpr float silence_epsilon = 1e-10f;
    auto denominator = AK::sqrt(sum_aa * sum_bb);
    if (denominator < silence_epsilon)
        return 0.0f;
    return sum_ab / denominator;
}

TimeStretchProcessor::ProcessResult TimeStretchProcessor::process_resample(ReadonlySpan<float> input, Span<float> output)
{
    auto const channels = m_channel_count;
    auto input_frames = input.size() / channels;
    auto output_frames = output.size() / channels;

    size_t output_written = 0;

    while (output_written < output_frames) {
        auto input_index = static_cast<size_t>(m_resample_position);
        if (input_index + 1 >= input_frames)
            break;

        auto fraction = static_cast<float>(m_resample_position - static_cast<double>(input_index));

        for (u8 ch = 0; ch < channels; ch++) {
            auto sample_a = input[(input_index * channels) + ch];
            auto sample_b = input[((input_index + 1) * channels) + ch];
            output[(output_written * channels) + ch] = sample_a + (fraction * (sample_b - sample_a));
        }

        output_written++;
        m_resample_position += m_rate;
    }

    auto input_frames_consumed = min(static_cast<size_t>(m_resample_position), input_frames);
    m_resample_position -= static_cast<double>(input_frames_consumed);

    return { input_frames_consumed, output_written };
}

}
