/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SeekableSharedMemoryStream.h>

namespace Core {

ErrorOr<Bytes> SeekableSharedMemoryStream::read_some(Bytes bytes)
{
    Threading::MutexLocker locker(m_mutex);

    size_t read_bytes = 0;

    while (read_bytes < bytes.size()) {
        VERIFY(m_chunk_index < m_chunks.size());
        auto const& chunk = m_chunks[m_chunk_index];

        VERIFY(m_offset_inside_chunk <= chunk.size());
        auto destination_span = bytes.slice(read_bytes);
        auto const chunk_span = chunk.readonly_span<u8>().slice(m_offset_inside_chunk);

        auto copied_bytes = chunk_span.copy_trimmed_to(destination_span);
        read_bytes += copied_bytes;

        if (copied_bytes == chunk_span.size()) {
            auto next_chunk_index = m_chunk_index + 1;

            m_waiting_for_more_data.wait_while([this, next_chunk_index] {
                return !m_closed && next_chunk_index == m_chunks.size();
            });

            if (m_closed && next_chunk_index == m_chunks.size()) {
                m_offset_inside_chunk = chunk.size();
                break;
            }

            ++m_chunk_index;
            m_offset_inside_chunk = 0;
        } else {
            m_offset_inside_chunk += copied_bytes;
        }
    }

    return bytes.trim(read_bytes);
}

ErrorOr<size_t> SeekableSharedMemoryStream::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> SeekableSharedMemoryStream::seek(i64 offset, SeekMode seek_mode)
{
    Threading::MutexLocker locker(m_mutex);

    switch (seek_mode) {
    case SeekMode::SetPosition: {
        size_t remaining_bytes_to_seek = offset;
        size_t new_chunk_index = 0;

        while (remaining_bytes_to_seek > 0) {
            m_waiting_for_more_data.wait_while([this, new_chunk_index] {
                return !m_closed && new_chunk_index >= m_chunks.size();
            });

            if (m_closed && new_chunk_index >= m_chunks.size())
                return Error::from_string_literal("Offset past the end of the stream memory");

            auto const& chunk = m_chunks[new_chunk_index];

            if (remaining_bytes_to_seek <= chunk.size()) {
                m_chunk_index = new_chunk_index;
                m_offset_inside_chunk = remaining_bytes_to_seek;
                remaining_bytes_to_seek = 0;
            } else {
                ++new_chunk_index;
                remaining_bytes_to_seek -= chunk.size();
            }
        }

        return offset;
    }
    // FIXME: Support these modes.
    case SeekMode::FromCurrentPosition:
        return Error::from_errno(ENOTSUP);
    case SeekMode::FromEndPosition:
        return Error::from_errno(ENOTSUP);
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<void> SeekableSharedMemoryStream::truncate(size_t)
{
    return Error::from_errno(EBADF);
}

bool SeekableSharedMemoryStream::is_eof() const
{
    Threading::MutexLocker locker(m_mutex);

    return m_closed
        && m_chunk_index == m_chunks.size() - 1
        && m_offset_inside_chunk == m_chunks.last().size();
}

bool SeekableSharedMemoryStream::is_open() const
{
    Threading::MutexLocker locker(m_mutex);
    return !m_closed;
}

void SeekableSharedMemoryStream::close()
{
    Threading::MutexLocker locker(m_mutex);

    m_closed = true;
    m_waiting_for_more_data.broadcast();
}

void SeekableSharedMemoryStream::append_chunk(AnonymousBuffer&& chunk)
{
    Threading::MutexLocker locker(m_mutex);

    if (m_closed)
        return;

    m_chunks.append(move(chunk));
    m_waiting_for_more_data.broadcast();
}

}
