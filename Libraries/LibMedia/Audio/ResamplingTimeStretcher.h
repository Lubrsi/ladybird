/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/Audio/AudioBuffer.h>
#include <LibMedia/Audio/AudioRingBuffer.h>
#include <LibMedia/Audio/SincInterpolator.h>
#include <LibMedia/Audio/TimeStretcher.h>
#include <LibMedia/Export.h>

namespace Audio {

// Changes the playback rate by resampling, so the pitch shifts along with the speed.
class MEDIA_API ResamplingTimeStretcher final : public TimeStretcher {
public:
    static ErrorOr<NonnullOwnPtr<TimeStretcher>> create(SampleSpecification);
    virtual ~ResamplingTimeStretcher() override;

    virtual i64 preroll_frame_count() const override;
    virtual void flush(AK::Duration media_start_timestamp, i64 output_start_frame_index) override;
    virtual void set_rate(float) override;
    virtual void push_block(Media::AudioBlock const&) override;
    virtual Media::DecoderErrorOr<void> retrieve_block(Media::AudioBlock& into) override;
    virtual void signal_end_of_stream() override;

private:
    static constexpr size_t PHASES_PER_FRAME = 32;

    explicit ResamplingTimeStretcher(SampleSpecification);

    size_t output_chunk_frames() const;
    size_t renderable_frame_count(size_t requested_frames) const;
    void gather_window(size_t frame_count);

    SampleSpecification const m_sample_specification;
    AudioRingBuffer m_input;
    AudioBuffer m_window;
    SincInterpolator m_interpolator;

    float m_rate { 1.0f };

    i64 m_input_front_media_frame { 0 };
    size_t m_read_index { 0 };
    double m_read_fraction { 0.0 };

    i64 m_next_output_frame_index { 0 };
    i64 m_expected_next_input_media_frame { 0 };
    i64 m_reported_media_end_frame { 0 };
    bool m_eos_signalled { false };
};

}
