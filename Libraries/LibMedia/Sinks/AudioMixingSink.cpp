/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Audio/TimeStretchProcessor.h>
#include <LibMedia/Providers/AudioDataProvider.h>

#include "AudioMixingSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<AudioMixingSink>> AudioMixingSink::try_create()
{
    auto weak_ref = TRY(try_make_ref_counted<AudioMixingSinkWeakReference>());
    auto sink = TRY(try_make_ref_counted<AudioMixingSink>(weak_ref));
    weak_ref->emplace(sink);
    return sink;
}

AudioMixingSink::AudioMixingSink(AudioMixingSinkWeakReference& weak_ref)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_weak_self(weak_ref)
{
    m_main_thread_event_loop.deferred_invoke([weak_self = m_weak_self] {
        auto self = weak_self->take_strong();
        if (!self)
            return;
        self->create_playback_stream();
    });
}

AudioMixingSink::~AudioMixingSink()
{
    m_weak_self->revoke();
}

void AudioMixingSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    Threading::MutexLocker locker { m_mutex };
    m_track_mixing_datas.remove(track);
    if (provider == nullptr)
        return;

    // The provider must have its output sample specification set before it starts decoding, or
    // we'll drop some samples due to a mismatch.
    m_track_mixing_datas.set(track, TrackMixingData(*provider));
    if (m_sample_specification.is_valid()) {
        provider->set_output_sample_specification(m_sample_specification);
        provider->start();
    }
}

RefPtr<AudioDataProvider> AudioMixingSink::provider(Track const& track) const
{
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

void AudioMixingSink::create_playback_stream()
{
    if (m_playback_stream != nullptr || m_creating_playback_stream)
        return;

    m_creating_playback_stream = true;

    auto data_callback = [weak_self = m_weak_self](Span<float> buffer) -> ReadonlySpan<float> {
        auto self = weak_self->take_strong();
        if (!self)
            return buffer.trim(0);
        return self->write_audio_data_to_playback_stream(buffer);
    };
    constexpr u32 target_latency_ms = 100;

    auto promise = Audio::PlaybackStream::create(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));

    promise->when_resolved([weak_self = m_weak_self](auto& stream) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        self->m_creating_playback_stream = false;
        self->m_playback_stream = stream;
        self->set_volume(self->m_volume);
        if (self->m_temporary_time.has_value())
            self->set_time(self->m_temporary_time.value());

        Threading::MutexLocker locker { self->m_mutex };
        self->m_sample_specification = stream->sample_specification();

        self->m_time_stretch_processor.initialize(
            self->m_sample_specification.sample_rate(),
            self->m_sample_specification.channel_count());

        // Pre-allocate intermediate buffer for maximum rate (16x) plus WSOLA overhead.
        auto max_buffer_frames = stream->sample_specification().sample_rate(); // 1 second of audio should be more than enough.
        self->m_intermediate_buffer.resize(max_buffer_frames * self->m_sample_specification.channel_count());

        for (auto& [track, track_data] : self->m_track_mixing_datas) {
            track_data.provider->set_output_sample_specification(self->m_sample_specification);
            track_data.provider->start();
        }

        if (self->m_playing)
            self->resume();
    });

    promise->when_rejected([weak_self = m_weak_self](auto& error) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        self->m_creating_playback_stream = false;
        if (self->on_audio_output_error)
            self->on_audio_output_error(move(error));
    });
}

void AudioMixingSink::mix_tracks_into_buffer(Span<float> target_buffer, i64 buffer_start, size_t sample_count)
{
    auto channel_count = m_sample_specification.channel_count();
    auto samples_end = buffer_start + static_cast<i64>(sample_count);
    auto write_size = sample_count * channel_count;

    for (auto& [track, track_data] : m_track_mixing_datas) {
        auto next_sample = buffer_start;

        auto go_to_next_block = [&] {
            auto new_block = track_data.provider->retrieve_block();
            if (new_block.is_empty())
                return false;

            track_data.current_block = move(new_block);
            return true;
        };

        if (track_data.current_block.is_empty()) {
            if (!go_to_next_block())
                continue;
        }

        while (!track_data.current_block.is_empty()) {
            auto& current_block = track_data.current_block;
            auto current_block_sample_count = static_cast<i64>(current_block.sample_count());

            if (current_block.sample_specification() != m_sample_specification) {
                if (!go_to_next_block())
                    break;
                current_block.clear();
                continue;
            }

            auto first_sample_offset = current_block.timestamp_in_samples();
            if (first_sample_offset >= samples_end)
                break;

            auto block_end = first_sample_offset + current_block_sample_count;
            if (block_end <= next_sample) {
                if (!go_to_next_block())
                    break;
                continue;
            }

            next_sample = max(next_sample, first_sample_offset);

            VERIFY(next_sample >= first_sample_offset);
            auto index_in_block = static_cast<size_t>((next_sample - first_sample_offset) * channel_count);
            VERIFY(index_in_block < current_block.data_count());

            VERIFY(next_sample >= buffer_start);
            auto index_in_buffer = static_cast<size_t>((next_sample - buffer_start) * channel_count);
            VERIFY(index_in_buffer < write_size);

            VERIFY(current_block.data_count() >= index_in_block);
            auto write_count = current_block.data_count() - index_in_block;
            write_count = min(write_count, write_size - index_in_buffer);
            VERIFY(write_count > 0);
            VERIFY(index_in_buffer + write_count <= write_size);
            VERIFY(write_count % channel_count == 0);

            for (size_t i = 0; i < write_count; i++)
                target_buffer[index_in_buffer + i] += current_block.data()[index_in_block + i];

            auto write_end = index_in_block + write_count;
            if (write_end == current_block.data_count()) {
                if (!go_to_next_block())
                    break;
                continue;
            }
            VERIFY(write_end < current_block.data_count());

            next_sample += static_cast<i64>(write_count / channel_count);
            if (next_sample == samples_end)
                break;
            VERIFY(next_sample < samples_end);
        }
    }
}

ReadonlySpan<float> AudioMixingSink::write_audio_data_to_playback_stream(Span<float> buffer)
{
    VERIFY(m_sample_specification.is_valid());
    VERIFY(buffer.size() > 0);

    auto channel_count = m_sample_specification.channel_count();
    auto output_sample_count = buffer.size() / channel_count;

    Threading::MutexLocker mixing_data_locker { m_mutex };

    auto rate = m_playback_rate;

    // Calculate how many input (media) samples we need.
    size_t input_sample_count;
    if (rate == 1.0)
        input_sample_count = output_sample_count;
    else
        input_sample_count = m_time_stretch_processor.input_frames_needed(output_sample_count);

    auto buffer_start = m_next_sample_to_write.load();
    auto samples_end = buffer_start + static_cast<i64>(input_sample_count);

    // Check for buffering.
    auto buffering = false;
    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!track_data.provider->is_blocked())
            continue;
        auto available_end = track_data.provider->queue_end_sample();
        if (available_end < samples_end) {
            samples_end = available_end;
            buffering = true;
        }
    }

    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!buffering) {
            track_data.buffering = false;
        } else {
            if (!track_data.provider->is_blocked())
                continue;
            if (track_data.buffering)
                continue;
            track_data.buffering = true;

            m_main_thread_event_loop.deferred_invoke([weak_self = m_weak_self, track] {
                auto self = weak_self->take_strong();
                if (self && self->on_start_buffering)
                    self->on_start_buffering(track);
            });
        }
    }

    input_sample_count = max(samples_end - buffer_start, 0);

    if (input_sample_count == 0) {
        buffer.fill(0.0f);
        return buffer;
    }

    if (rate == 1.0) {
        // Fast path: no time stretching needed.
        buffer.fill(0.0f);
        mix_tracks_into_buffer(buffer, buffer_start, input_sample_count);
        m_next_sample_to_write += static_cast<i64>(input_sample_count);
        return buffer;
    }

    // Mute audio at extreme rates but still advance media time.
    if (rate > AUDIO_MUTE_RATE_THRESHOLD || rate < (1.0 / AUDIO_MUTE_RATE_THRESHOLD)) {
        buffer.fill(0.0f);
        m_next_sample_to_write += static_cast<i64>(input_sample_count);
        return buffer;
    }

    // Time-stretched path: mix into intermediate buffer, then process.
    auto input_data_size = input_sample_count * channel_count;
    VERIFY(input_data_size <= m_intermediate_buffer.size());

    auto intermediate = Span<float>(m_intermediate_buffer.data(), input_data_size);
    intermediate.fill(0.0f);

    mix_tracks_into_buffer(intermediate, buffer_start, input_sample_count);

    buffer.fill(0.0f);
    auto result = m_time_stretch_processor.process(
        ReadonlySpan<float>(intermediate.data(), input_data_size),
        buffer);

    m_next_sample_to_write += static_cast<i64>(result.input_frames_consumed);
    return buffer;
}

AK::Duration AudioMixingSink::current_time() const
{
    if (!m_sample_specification.is_valid())
        return AK::Duration::zero();
    if (m_temporary_time.has_value())
        return m_temporary_time.value();
    if (!m_playback_stream)
        return m_last_media_time;

    auto elapsed = m_playback_stream->total_time_played() - m_last_stream_time;
    auto time = m_last_media_time + AK::Duration::from_nanoseconds(static_cast<i64>(static_cast<double>(elapsed.to_nanoseconds()) * m_playback_rate));
    auto max_time = AK::Duration::from_time_units(m_next_sample_to_write.load(MemoryOrder::memory_order_acquire), 1, m_sample_specification.sample_rate());
    time = min(time, max_time);
    return time;
}

void AudioMixingSink::resume()
{
    m_playing = true;

    // If we're in the middle of the set_time() callbacks, let those take care of resuming.
    if (m_temporary_time.has_value())
        return;

    if (!m_playback_stream)
        return;
    m_playback_stream->resume()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream](auto new_device_time) {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            self->m_main_thread_event_loop.deferred_invoke([self, new_device_time]() {
                self->m_last_stream_time = new_device_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::pause()
{
    m_playing = false;

    if (!m_playback_stream)
        return;
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();
            auto new_media_time = AK::Duration::from_time_units(self->m_next_sample_to_write, 1, self->m_sample_specification.sample_rate());

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time, new_media_time]() {
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = new_media_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::set_time(AK::Duration time)
{
    if (!m_playback_stream) {
        m_last_media_time = time;
        m_last_stream_time = AK::Duration::zero();
        return;
    }

    // If we've already started setting the time, we only need to let the last callback complete
    // and set the media time to the temporary time. The callbacks run synchronously, so this will
    // never drop a set_time() call.
    if (m_temporary_time.has_value()) {
        m_temporary_time = time;
        return;
    }

    m_temporary_time = time;

    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                {
                    self->m_last_stream_time = new_stream_time;
                    self->m_last_media_time = self->m_temporary_time.release_value();

                    {
                        Threading::MutexLocker mixing_locker { self->m_mutex };
                        self->m_next_sample_to_write = self->m_last_media_time.to_time_units(1, self->m_sample_specification.sample_rate());
                    }

                    for (auto& [track, track_data] : self->m_track_mixing_datas)
                        track_data.current_block.clear();

                    self->m_time_stretch_processor.reset();
                }

                if (self->m_playing)
                    self->resume();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while setting time on AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::clear_track_data(Track const& track)
{
    auto track_data = m_track_mixing_datas.find(track);
    if (track_data == m_track_mixing_datas.end())
        return;
    track_data->value.current_block.clear();
}

void AudioMixingSink::set_playback_rate(double rate)
{
    Threading::MutexLocker locker { m_mutex };

    // Snapshot current time before changing rate to avoid a time jump.
    if (m_playback_stream) {
        m_last_media_time = current_time();
        m_last_stream_time = m_playback_stream->total_time_played();
    }

    m_playback_rate = rate;
    m_time_stretch_processor.set_rate(rate);
}

void AudioMixingSink::set_preserves_pitch(bool preserves_pitch)
{
    Threading::MutexLocker locker { m_mutex };
    m_time_stretch_processor.set_preserves_pitch(preserves_pitch);
}

void AudioMixingSink::set_volume(double volume)
{
    m_volume = volume;

    if (m_playback_stream) {
        m_playback_stream->set_volume(m_volume)
            ->when_rejected([](Error&&) {
                // FIXME: Do we even need this function to return a promise?
            });
    }
}

}
