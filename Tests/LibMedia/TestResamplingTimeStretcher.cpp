/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Vector.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Audio/ResamplingTimeStretcher.h>
#include <LibMedia/Audio/SincInterpolator.h>
#include <LibMedia/AudioBlockTiming.h>
#include <LibTest/TestCase.h>

static constexpr u32 SAMPLE_RATE = 44100;
static constexpr float TONE_FREQUENCY = 440.0f;
static constexpr i64 ONE_SECOND_IN_FRAMES = SAMPLE_RATE;

// The kernel needs this many frames past a position before it can be interpolated.
static constexpr size_t LOOKAHEAD = Audio::SincInterpolator::HALF_SPAN;

static Audio::SampleSpecification stereo_specification()
{
    return { SAMPLE_RATE, Audio::ChannelMap::stereo() };
}

static float tone_sample(i64 media_frame, float frequency = TONE_FREQUENCY)
{
    auto time = static_cast<float>(media_frame) / static_cast<float>(SAMPLE_RATE);
    return AK::sin(2.0f * AK::Pi<float> * frequency * time);
}

// The right channel is the inverted left channel, so channel mix-ups show up in the output.
static Media::AudioBlock make_tone_block(i64 first_frame_index, size_t frame_count, float frequency)
{
    Media::AudioBlock block;
    block.initialize(stereo_specification(), first_frame_index, frame_count);
    for (size_t frame = 0; frame < frame_count; frame++) {
        auto sample = tone_sample(first_frame_index + static_cast<i64>(frame), frequency);
        block.set_sample(0, frame, sample);
        block.set_sample(1, frame, -sample);
    }
    return block;
}

static void push_tone(Audio::TimeStretcher& stretcher, i64 first_frame_index, i64 end_frame_index, float frequency = TONE_FREQUENCY)
{
    constexpr i64 block_size = 1000;
    for (auto frame = first_frame_index; frame < end_frame_index; frame += block_size) {
        auto frame_count = static_cast<size_t>(min(block_size, end_frame_index - frame));
        stretcher.push_block(make_tone_block(frame, frame_count, frequency));
    }
}

struct StretchedOutput {
    Vector<float> left_samples;
    Vector<Media::AudioBlockTiming> timings;

    i64 end_frame_index() const { return timings.last().end_frame_index(); }
    AK::Duration media_time_end() const { return timings.last().media_time_end(); }
};

// Retrieves blocks until the stretcher runs dry, checking that they tile both the output frames and media time.
static Media::DecoderErrorCategory drain(Audio::TimeStretcher& stretcher, StretchedOutput& output)
{
    while (true) {
        Media::AudioBlock block;
        auto result = stretcher.retrieve_block(block);
        if (result.is_error()) {
            EXPECT(block.is_empty());
            return result.error().category();
        }

        EXPECT(!block.is_empty());
        if (!output.timings.is_empty()) {
            EXPECT_EQ(block.first_frame_index(), output.end_frame_index());
            EXPECT_EQ(block.media_time_start(), output.media_time_end());
        }

        for (size_t frame = 0; frame < block.frame_count(); frame++) {
            EXPECT_EQ(block.sample(1, frame), -block.sample(0, frame));
            output.left_samples.append(block.sample(0, frame));
        }
        output.timings.append(block.timing());
    }
}

static float measured_frequency(ReadonlySpan<float> samples)
{
    size_t crossings = 0;
    float previous_sign = 0.0f;
    for (auto sample : samples) {
        if (sample == 0.0f)
            continue;
        auto sign = sample > 0.0f ? 1.0f : -1.0f;
        if (previous_sign != 0.0f && sign != previous_sign)
            crossings++;
        previous_sign = sign;
    }
    auto duration_in_seconds = static_cast<float>(samples.size()) / static_cast<float>(SAMPLE_RATE);
    return static_cast<float>(crossings) / 2.0f / duration_in_seconds;
}

static float root_mean_square(ReadonlySpan<float> samples)
{
    double sum_of_squares = 0;
    for (auto sample : samples)
        sum_of_squares += static_cast<double>(sample) * sample;
    return static_cast<float>(AK::sqrt(sum_of_squares / static_cast<double>(samples.size())));
}

static AK::Duration frames_to_duration(i64 frames)
{
    return AK::Duration::from_time_units(frames, 1, SAMPLE_RATE);
}

TEST_CASE(rate_of_one_passes_input_through)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);

    EXPECT_EQ(output.left_samples.size(), static_cast<size_t>(ONE_SECOND_IN_FRAMES));
    for (size_t frame = 0; frame < output.left_samples.size(); frame++)
        EXPECT_EQ(output.left_samples[frame], tone_sample(static_cast<i64>(frame)));
    EXPECT_EQ(output.timings.first().first_frame_index(), 0);
    EXPECT_EQ(output.end_frame_index(), ONE_SECOND_IN_FRAMES);
    EXPECT_EQ(output.media_time_end(), frames_to_duration(ONE_SECOND_IN_FRAMES));
}

TEST_CASE(speeding_up_raises_the_pitch)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    stretcher->set_rate(2.0f);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);

    EXPECT_EQ(output.left_samples.size(), static_cast<size_t>(ONE_SECOND_IN_FRAMES / 2));
    EXPECT_APPROXIMATE_WITH_ERROR(measured_frequency(output.left_samples), TONE_FREQUENCY * 2.0f, 5.0f);
    EXPECT_APPROXIMATE_WITH_ERROR(root_mean_square(output.left_samples), AK::sqrt(0.5f), 0.01f);
    EXPECT_EQ(output.media_time_end(), frames_to_duration(ONE_SECOND_IN_FRAMES));
}

TEST_CASE(speeding_up_filters_out_what_would_alias)
{
    // At twice the rate, anything above a quarter of the sample rate would fold back into the audible band.
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    stretcher->set_rate(2.0f);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES, 16000.0f);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);

    EXPECT_EQ(output.left_samples.size(), static_cast<size_t>(ONE_SECOND_IN_FRAMES / 2));
    EXPECT(root_mean_square(output.left_samples) < 0.01f * AK::sqrt(0.5f));
}

TEST_CASE(slowing_down_lowers_the_pitch)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    stretcher->set_rate(0.5f);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);

    EXPECT_EQ(output.left_samples.size(), static_cast<size_t>(ONE_SECOND_IN_FRAMES * 2));
    EXPECT_APPROXIMATE_WITH_ERROR(measured_frequency(output.left_samples), TONE_FREQUENCY / 2.0f, 5.0f);
    EXPECT_APPROXIMATE_WITH_ERROR(root_mean_square(output.left_samples), AK::sqrt(0.5f), 0.01f);
    EXPECT_EQ(output.media_time_end(), frames_to_duration(ONE_SECOND_IN_FRAMES));
}

TEST_CASE(slowing_down_interpolates_between_frames)
{
    constexpr float rate = 0.7f;

    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    stretcher->set_rate(rate);
    push_tone(*stretcher, 0, 5000);

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::NeedsMoreInput);

    // Every output frame should match the tone evaluated at its fractional read position, apart from the first ones,
    // whose kernel reaches back before the start of the input.
    auto first_settled_frame = static_cast<size_t>(AK::ceil(static_cast<float>(LOOKAHEAD) / rate));
    EXPECT(output.left_samples.size() > first_settled_frame + 1000);
    for (size_t frame = first_settled_frame; frame < output.left_samples.size(); frame++) {
        auto position = static_cast<double>(frame) * static_cast<double>(rate);
        auto expected = AK::sin(2.0 * AK::Pi<double> * TONE_FREQUENCY * position / SAMPLE_RATE);
        EXPECT_APPROXIMATE_WITH_ERROR(output.left_samples[frame], static_cast<float>(expected), 0.0001f);
    }
}

TEST_CASE(blocks_tile_media_time_across_rate_changes)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    for (auto rate : { 1.7f, 0.3f, 3.1f, 0.9f }) {
        stretcher->set_rate(rate);
        Media::AudioBlock block;
        TRY_OR_FAIL(stretcher->retrieve_block(block));

        if (!output.timings.is_empty()) {
            EXPECT_EQ(block.first_frame_index(), output.end_frame_index());
            EXPECT_EQ(block.media_time_start(), output.media_time_end());
        }
        auto expected_media_frames = static_cast<float>(block.frame_count()) * rate;
        auto media_frames = block.media_time_duration().to_time_units(1, SAMPLE_RATE);
        EXPECT(AK::abs(static_cast<float>(media_frames) - expected_media_frames) <= 1.0f);
        output.timings.append(block.timing());
    }

    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);
    EXPECT_EQ(output.media_time_end(), frames_to_duration(ONE_SECOND_IN_FRAMES));
}

TEST_CASE(gaps_are_filled_with_silence_and_overlaps_are_dropped)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    push_tone(*stretcher, 0, 1000);
    push_tone(*stretcher, 2000, 3000);

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::NeedsMoreInput);

    // At the original rate, a position lands on a frame and reproduces it exactly, silence included.
    EXPECT_EQ(output.left_samples.size(), 3000 - LOOKAHEAD);
    for (size_t frame = 0; frame < 1000; frame++)
        EXPECT_EQ(output.left_samples[frame], tone_sample(static_cast<i64>(frame)));
    for (size_t frame = 1000; frame < 2000; frame++)
        EXPECT_EQ(output.left_samples[frame], 0.0f);
    for (size_t frame = 2000; frame < output.left_samples.size(); frame++)
        EXPECT_EQ(output.left_samples[frame], tone_sample(static_cast<i64>(frame)));

    push_tone(*stretcher, 2500, 3500);
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::NeedsMoreInput);

    EXPECT_EQ(output.left_samples.size(), 3500 - LOOKAHEAD);
    for (size_t frame = 3000 - LOOKAHEAD; frame < output.left_samples.size(); frame++)
        EXPECT_EQ(output.left_samples[frame], tone_sample(static_cast<i64>(frame)));
    EXPECT_EQ(output.media_time_end(), frames_to_duration(3500 - LOOKAHEAD));
}

TEST_CASE(flush_restarts_from_the_given_positions)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    push_tone(*stretcher, 0, 5000);

    stretcher->flush(frames_to_duration(ONE_SECOND_IN_FRAMES), 5000);
    push_tone(*stretcher, ONE_SECOND_IN_FRAMES - 100, ONE_SECOND_IN_FRAMES + 900);

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::NeedsMoreInput);

    EXPECT_EQ(output.timings.first().first_frame_index(), 5000);
    EXPECT_EQ(output.timings.first().media_time_start(), frames_to_duration(ONE_SECOND_IN_FRAMES));
    EXPECT_EQ(output.left_samples.size(), 900 - LOOKAHEAD);
    for (size_t frame = 0; frame < output.left_samples.size(); frame++)
        EXPECT_EQ(output.left_samples[frame], tone_sample(ONE_SECOND_IN_FRAMES + static_cast<i64>(frame)));
}

TEST_CASE(end_of_stream_is_reported_once_the_input_is_used_up)
{
    auto stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    stretcher->flush(AK::Duration::zero(), 0);
    stretcher->set_rate(16.0f);
    push_tone(*stretcher, 0, ONE_SECOND_IN_FRAMES);
    stretcher->signal_end_of_stream();

    StretchedOutput output;
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);

    // Read positions 0, 16, ... up to and including the last one before the end of the input.
    EXPECT_EQ(output.left_samples.size(), static_cast<size_t>((ONE_SECOND_IN_FRAMES - 1) / 16) + 1);
    EXPECT_EQ(output.media_time_end(), frames_to_duration(ONE_SECOND_IN_FRAMES));
    EXPECT_EQ(drain(*stretcher, output), Media::DecoderErrorCategory::EndOfStream);
}

// Feeding the input in small pieces, with the read position regularly running past the buffered frames,
// must produce the same output as feeding it all at once.
static void expect_chunked_input_matches_whole_input(float rate)
{
    auto whole_stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    whole_stretcher->flush(AK::Duration::zero(), 0);
    whole_stretcher->set_rate(rate);
    push_tone(*whole_stretcher, 0, ONE_SECOND_IN_FRAMES);
    whole_stretcher->signal_end_of_stream();
    StretchedOutput whole_output;
    EXPECT_EQ(drain(*whole_stretcher, whole_output), Media::DecoderErrorCategory::EndOfStream);

    auto chunked_stretcher = TRY_OR_FAIL(Audio::ResamplingTimeStretcher::create(stereo_specification()));
    chunked_stretcher->flush(AK::Duration::zero(), 0);
    chunked_stretcher->set_rate(rate);
    StretchedOutput chunked_output;
    for (i64 frame = 0; frame < ONE_SECOND_IN_FRAMES; frame += 100) {
        push_tone(*chunked_stretcher, frame, frame + 100);
        EXPECT_EQ(drain(*chunked_stretcher, chunked_output), Media::DecoderErrorCategory::NeedsMoreInput);
    }
    chunked_stretcher->signal_end_of_stream();
    EXPECT_EQ(drain(*chunked_stretcher, chunked_output), Media::DecoderErrorCategory::EndOfStream);

    // Block boundaries change the floating-point evaluation order of the read positions, so allow rounding noise.
    EXPECT_EQ(chunked_output.left_samples.size(), whole_output.left_samples.size());
    for (size_t frame = 0; frame < whole_output.left_samples.size(); frame++)
        EXPECT_APPROXIMATE_WITH_ERROR(chunked_output.left_samples[frame], whole_output.left_samples[frame], 0.00001f);
    EXPECT_EQ(chunked_output.end_frame_index(), whole_output.end_frame_index());
    EXPECT_EQ(chunked_output.media_time_end(), whole_output.media_time_end());
}

TEST_CASE(chunked_input_matches_whole_input_when_speeding_up)
{
    expect_chunked_input_matches_whole_input(16.0f);
}

TEST_CASE(chunked_input_matches_whole_input_when_slowing_down)
{
    expect_chunked_input_matches_whole_input(0.7f);
}
