/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibMedia/Audio/SincInterpolator.h>

namespace Audio {

static double sinc(double x)
{
    // Exact at the center and at the zero crossings, so that a position on a frame reproduces that frame bit for bit.
    if (static_cast<double>(AK::round_to<i64>(x)) == x)
        return x == 0 ? 1 : 0;
    return AK::sin(AK::Pi<double> * x) / (AK::Pi<double> * x);
}

// The coefficients of the classic Blackman window, w(n) = a0 - a1 cos(2πn/N) + a2 cos(4πn/N). They are rounded
// from the exact ones, which trades a slightly wider main lobe for sidelobes that fall faster.
static constexpr double BLACKMAN_A0 = 0.42;
static constexpr double BLACKMAN_A1 = 0.5;
static constexpr double BLACKMAN_A2 = 0.08;

// The window centered on the interpolated position rather than on its first tap, which flips the sign of the a1 term.
static double blackman_window(double distance)
{
    if (AK::abs(distance) >= static_cast<double>(SincInterpolator::HALF_SPAN))
        return 0;
    auto phase = AK::Pi<double> * distance / static_cast<double>(SincInterpolator::HALF_SPAN);
    return BLACKMAN_A0 + (BLACKMAN_A1 * AK::cos(phase)) + (BLACKMAN_A2 * AK::cos(2 * phase));
}

static float apply_taps(ReadonlySpan<float> taps, ReadonlySpan<float> frames)
{
    double sum = 0;
    for (size_t i = 0; i < taps.size(); ++i)
        sum += static_cast<double>(taps[i]) * frames[i];
    return static_cast<float>(sum);
}

SincInterpolator::SincInterpolator(size_t phases, double cutoff)
    : m_phases(phases)
    , m_cutoff(cutoff)
{
    VERIFY(phases > 0);
    VERIFY(cutoff > 0 && cutoff <= 1);

    // One phase past the last lands on the next frame, so the final blend of a frame has something to reach.
    m_taps = MUST(FixedArray<float>::create((phases + 1) * SPAN));

    Array<double, SPAN> unnormalized_taps;
    for (size_t phase = 0; phase <= phases; ++phase) {
        auto position = static_cast<double>(phase) / static_cast<double>(phases);
        double sum = 0;
        for (size_t tap_index = 0; tap_index < SPAN; ++tap_index) {
            auto distance = position - (static_cast<double>(tap_index) - static_cast<double>(HALF_SPAN));
            unnormalized_taps[tap_index] = sinc(cutoff * distance) * blackman_window(distance);
            sum += unnormalized_taps[tap_index];
        }

        // Each phase is normalized to unity gain at DC on its own, so a constant signal interpolates to itself.
        auto taps = m_taps.span().slice(phase * SPAN, SPAN);
        for (size_t tap_index = 0; tap_index < SPAN; ++tap_index)
            taps[tap_index] = static_cast<float>(unnormalized_taps[tap_index] / sum);
    }
}

ReadonlySpan<float> SincInterpolator::phase_taps(size_t phase) const
{
    return m_taps.span().slice(phase * SPAN, SPAN);
}

float SincInterpolator::interpolate(ReadonlySpan<float> frames_around, double fraction) const
{
    VERIFY(frames_around.size() == SPAN);
    VERIFY(fraction >= 0 && fraction < 1);

    auto phase_position = fraction * static_cast<double>(m_phases);
    auto phase = static_cast<size_t>(phase_position);
    auto weight = phase_position - static_cast<double>(phase);

    auto value = apply_taps(phase_taps(phase), frames_around);
    if (weight == 0)
        return value;
    auto next_value = apply_taps(phase_taps(phase + 1), frames_around);
    return value + ((next_value - value) * static_cast<float>(weight));
}

}
