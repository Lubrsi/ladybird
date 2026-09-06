/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/Oversampler.h>

namespace Web::WebAudio::Rendering {

// The number of kernel taps that line up with each input frame, which is also the latency of the two stages together.
// The kernel spans one tap more than this times the factor, so that it is symmetric around a whole frame.
static constexpr size_t TAPS_PER_FRAME = 64;

// A sinc low-pass at the original Nyquist frequency, shaped by a Blackman window and normalized to unity gain at DC.
static Vector<float> compute_low_pass_kernel(size_t factor)
{
    auto length = (TAPS_PER_FRAME * factor) + 1;
    auto center = static_cast<double>(length - 1) / 2;

    Vector<double> taps;
    taps.resize(length);
    double sum = 0;
    for (size_t i = 0; i < length; ++i) {
        auto t = (static_cast<double>(i) - center) / static_cast<double>(factor);
        auto sinc = t == 0 ? 1. : AK::sin(AK::Pi<double> * t) / (AK::Pi<double> * t);
        auto phase = 2 * AK::Pi<double> * static_cast<double>(i) / static_cast<double>(length - 1);
        auto window = 0.42 - (0.5 * AK::cos(phase)) + (0.08 * AK::cos(2 * phase));
        taps[i] = sinc * window;
        sum += taps[i];
    }

    Vector<float> kernel;
    kernel.ensure_capacity(length);
    for (auto tap : taps)
        kernel.unchecked_append(static_cast<float>(tap / sum));
    return kernel;
}

Oversampler::Oversampler(size_t factor)
    : m_factor(factor)
    , m_kernel(compute_low_pass_kernel(factor))
{
}

void Oversampler::set_channel_count(size_t channel_count)
{
    if (m_channel_states.size() > channel_count)
        m_channel_states.resize(channel_count);
    while (m_channel_states.size() < channel_count) {
        ChannelState state;
        state.input_history.resize(TAPS_PER_FRAME + 1);
        state.upsampled_history.resize((TAPS_PER_FRAME + 1) * m_factor);
        m_channel_states.append(move(state));
    }
}

void Oversampler::upsample(size_t channel, float sample, Span<float> upsampled)
{
    auto& state = m_channel_states[channel];
    auto& history = state.input_history;
    history[state.input_position] = sample;
    state.input_position = (state.input_position + 1) % history.size();

    // Polyphase form of filtering the zero-stuffed signal: the samples of each phase only meet every `factor`-th tap,
    // so the zero-valued samples are never multiplied.
    for (size_t phase = 0; phase < m_factor; ++phase) {
        double sum = 0;
        auto position = state.input_position;
        for (size_t tap_index = phase; tap_index < m_kernel.size(); tap_index += m_factor) {
            position = position == 0 ? history.size() - 1 : position - 1;
            sum += m_kernel[tap_index] * history[position];
        }
        upsampled[phase] = static_cast<float>(sum * static_cast<double>(m_factor));
    }
}

float Oversampler::downsample(size_t channel, ReadonlySpan<float> upsampled)
{
    auto& state = m_channel_states[channel];
    auto& history = state.upsampled_history;
    for (auto sample : upsampled) {
        history[state.upsampled_position] = sample;
        state.upsampled_position = (state.upsampled_position + 1) % history.size();
    }

    // The frame is decimated at its first oversampled sample rather than its last, so that both stages together delay
    // the signal by exactly TAPS_PER_FRAME frames.
    auto position = (state.upsampled_position + history.size() - (m_factor - 1)) % history.size();
    double sum = 0;
    for (auto tap : m_kernel) {
        position = position == 0 ? history.size() - 1 : position - 1;
        sum += tap * history[position];
    }
    return static_cast<float>(sum);
}

}
