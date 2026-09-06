/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/SaturatingMath.h>
#include <AK/TypedTransfer.h>
#include <LibMedia/Audio/ResamplingTimeStretcher.h>

namespace Audio {

ErrorOr<NonnullOwnPtr<TimeStretcher>> ResamplingTimeStretcher::create(SampleSpecification sample_specification)
{
    if (!sample_specification.is_valid())
        return Error::from_string_literal("Invalid sample specification");
    return adopt_own(*new ResamplingTimeStretcher(sample_specification));
}

ResamplingTimeStretcher::ResamplingTimeStretcher(SampleSpecification sample_specification)
    : m_sample_specification(sample_specification)
    , m_input(sample_specification)
    , m_interpolator(PHASES_PER_FRAME, 1.0)
{
}

ResamplingTimeStretcher::~ResamplingTimeStretcher() = default;

size_t ResamplingTimeStretcher::output_chunk_frames() const
{
    auto frames_for_30ms = ((static_cast<u64>(m_sample_specification.sample_rate()) * 30) + 500) / 1000;
    return clamp<size_t>(frames_for_30ms, 1, Media::AudioBlock::max_frame_count(m_sample_specification.channel_count()));
}

i64 ResamplingTimeStretcher::preroll_frame_count() const
{
    return SincInterpolator::HALF_SPAN;
}

void ResamplingTimeStretcher::set_rate(float rate)
{
    VERIFY(isfinite(rate));
    VERIFY(rate > 0.0f);
    m_rate = rate;

    // Reading faster than the original rate lowers the frequency above which the input would alias, so the low-pass
    // follows it down.
    auto cutoff = min(1.0, 1.0 / static_cast<double>(rate));
    if (cutoff != m_interpolator.cutoff())
        m_interpolator = SincInterpolator(PHASES_PER_FRAME, cutoff);
}

void ResamplingTimeStretcher::flush(AK::Duration media_start_timestamp, i64 output_start_frame_index)
{
    m_input.clear();
    m_read_index = 0;
    m_read_fraction = 0.0;
    m_eos_signalled = false;

    auto media_start_in_frames = media_start_timestamp.to_time_units(1, m_sample_specification.sample_rate());
    m_input_front_media_frame = media_start_in_frames;
    m_expected_next_input_media_frame = media_start_in_frames;
    m_reported_media_end_frame = media_start_in_frames;
    m_next_output_frame_index = output_start_frame_index;
}

void ResamplingTimeStretcher::push_block(Media::AudioBlock const& input)
{
    VERIFY(!input.is_empty());
    VERIFY(input.sample_specification() == m_sample_specification);

    auto block_start = input.first_frame_index();
    auto frame_count = input.frame_count();
    size_t frames_to_skip = 0;

    // Keep the buffered input contiguous: fill gaps with silence and drop frames that were already buffered.
    auto gap = saturating_sub(block_start, m_expected_next_input_media_frame);
    if (gap > 0) {
        m_input.append_silence(static_cast<size_t>(gap));
    } else if (gap < 0) {
        frames_to_skip = min(-static_cast<size_t>(gap), frame_count);
        if (frames_to_skip == frame_count)
            return;
    }

    if (frames_to_skip == 0) {
        m_input.append(input);
    } else {
        auto frames_to_append = frame_count - frames_to_skip;
        Media::AudioBlock block_to_append;
        block_to_append.initialize(m_sample_specification, saturating_add(block_start, AK::clamp_to<i64>(frames_to_skip)), frames_to_append);
        for (size_t channel = 0; channel < input.channel_count(); channel++) {
            auto input_channel = input.channel_data(channel).slice(frames_to_skip, frames_to_append);
            AK::TypedTransfer<float>::copy(block_to_append.channel_data(channel).data(), input_channel.data(), frames_to_append);
        }
        m_input.append(block_to_append);
    }
    m_expected_next_input_media_frame = saturating_add(block_start, AK::clamp_to<i64>(frame_count));
}

void ResamplingTimeStretcher::signal_end_of_stream()
{
    if (m_eos_signalled)
        return;
    m_eos_signalled = true;

    // The kernel reaches past every position it interpolates, so the final frames need silence to reach into.
    m_input.append_silence(SincInterpolator::HALF_SPAN);
}

size_t ResamplingTimeStretcher::renderable_frame_count(size_t requested_frames) const
{
    // A position can be interpolated once the input reaches HALF_SPAN frames beyond it.
    auto available_frames = m_input.frame_count();
    if (available_frames <= m_read_index + SincInterpolator::HALF_SPAN)
        return 0;
    auto limit = available_frames - m_read_index - SincInterpolator::HALF_SPAN;

    auto rate = static_cast<double>(m_rate);
    auto is_renderable = [&](size_t frame) {
        return static_cast<size_t>(m_read_fraction + (static_cast<double>(frame) * rate)) < limit;
    };

    auto count = min(static_cast<size_t>((static_cast<double>(limit) - m_read_fraction) / rate), requested_frames);
    while (count > 0 && !is_renderable(count - 1))
        count--;
    while (count < requested_frames && is_renderable(count))
        count++;
    return count;
}

void ResamplingTimeStretcher::gather_window(size_t frame_count)
{
    auto last_position = static_cast<size_t>(m_read_fraction + (static_cast<double>(frame_count - 1) * static_cast<double>(m_rate)));
    auto window_frame_count = last_position + SincInterpolator::SPAN;
    if (m_window.frame_count() < window_frame_count)
        m_window = AudioBuffer { m_sample_specification, window_frame_count };

    // The frames before the first buffered one, which the kernel reaches into right after a flush, read as silence.
    auto history_frames = min(m_read_index, SincInterpolator::HALF_SPAN);
    auto silent_frames = SincInterpolator::HALF_SPAN - history_frames;
    m_window.zero_frames(0, silent_frames);
    m_input.copy_frames_to(m_read_index - history_frames, window_frame_count - silent_frames, silent_frames, m_window);
}

Media::DecoderErrorOr<void> ResamplingTimeStretcher::retrieve_block(Media::AudioBlock& into)
{
    auto const sample_rate = m_sample_specification.sample_rate();
    auto const channel_count = m_sample_specification.channel_count();
    auto const rate = static_cast<double>(m_rate);

    auto rendered_frames = renderable_frame_count(output_chunk_frames());
    if (rendered_frames == 0) {
        into.clear();
        if (m_eos_signalled)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "End of stream"sv);
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::NeedsMoreInput, "Need more input"sv);
    }

    // Each output frame interpolates the input at its read position, which advances by the rate.
    gather_window(rendered_frames);
    into.initialize(m_sample_specification, m_next_output_frame_index, rendered_frames);
    for (size_t frame = 0; frame < rendered_frames; frame++) {
        auto position = m_read_fraction + (static_cast<double>(frame) * rate);
        auto whole_position = static_cast<size_t>(position);
        auto fraction = position - static_cast<double>(whole_position);
        for (size_t channel = 0; channel < channel_count; channel++) {
            auto frames_around = m_window.channel_data(channel).slice(whole_position, SincInterpolator::SPAN);
            into.channel_data(channel)[frame] = m_interpolator.interpolate(frames_around, fraction);
        }
    }

    auto advance = m_read_fraction + (static_cast<double>(rendered_frames) * rate);
    auto whole_advance = static_cast<size_t>(advance);
    m_read_fraction = advance - static_cast<double>(whole_advance);
    m_read_index += whole_advance;

    // Keep the frames the kernel still reaches back into and drop what precedes them. The read position may have run
    // past the buffered input, in which case the remainder is skipped as it arrives.
    size_t frames_to_drop = 0;
    if (m_read_index > SincInterpolator::HALF_SPAN)
        frames_to_drop = min(m_read_index - SincInterpolator::HALF_SPAN, m_input.frame_count());
    m_input.drop_front(frames_to_drop);
    m_read_index -= frames_to_drop;
    m_input_front_media_frame = saturating_add(m_input_front_media_frame, AK::clamp_to<i64>(frames_to_drop));

    // Blocks tile media time up to the read position, but never past the input that has actually arrived.
    auto read_media_frame = saturating_add(m_input_front_media_frame, AK::clamp_to<i64>(m_read_index));
    auto media_end_frame = min(read_media_frame, m_expected_next_input_media_frame);
    auto media_time_start = AK::Duration::from_time_units(m_reported_media_end_frame, 1, sample_rate);
    auto media_time_end = AK::Duration::from_time_units(media_end_frame, 1, sample_rate);
    into.set_media_time_start(media_time_start);
    into.set_media_time_duration(media_time_end - media_time_start);
    m_reported_media_end_frame = media_end_frame;

    m_next_output_frame_index = saturating_add(m_next_output_frame_index, AK::clamp_to<i64>(rendered_frames));
    return {};
}

}
