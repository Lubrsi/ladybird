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

    if (m_chunks.is_empty()) {
        dbgln("no chunks, waiting for at least one to be appended");
        VERIFY(m_chunk_index == 0);
        m_waiting_for_more_data.wait_while([this] {
            return !m_closed && m_chunks.is_empty();
        });

        if (m_closed && m_chunks.is_empty()) {
            dbgln("closed and no chunks, returning no data");
            return Bytes {};
        }
    }

    while (read_bytes < bytes.size()) {
        dbgln("read, read_bytes = {}, bytes.size() = {}, m_chunk_index = {}, m_chunks.size() = {}", read_bytes, bytes.size(), m_chunk_index, m_chunks.size());
        VERIFY(m_chunk_index < m_chunks.size());
        auto const& chunk = m_chunks.find(m_chunk_index);

        VERIFY(m_offset_inside_chunk <= chunk->size());
        auto destination_span = bytes.slice(read_bytes);
        auto const chunk_span = chunk->span().slice(m_offset_inside_chunk);

        auto copied_bytes = chunk_span.copy_trimmed_to(destination_span);
        read_bytes += copied_bytes;
        dbgln("read {} bytes (span size = {})", copied_bytes, chunk_span.size());

        if (copied_bytes == chunk_span.size()) {
            auto next_chunk_index = m_chunk_index + 1;

            m_waiting_for_more_data.wait_while([this, next_chunk_index] {
                bool should_wait = !m_closed && next_chunk_index == m_chunks.size();
                dbgln("read, next_chunk_index = {}, m_chunks.size() = {}, should wait for more data? {}", next_chunk_index, m_chunks.size(), should_wait);
                return should_wait;
            });

            if (m_closed && next_chunk_index == m_chunks.size()) {
                m_offset_inside_chunk = chunk->size();
                break;
            }

            ++m_chunk_index;
            m_offset_inside_chunk = 0;
        } else {
            m_offset_inside_chunk += copied_bytes;
        }

        dbgln("new chunk index and offset: {}, {}", m_chunk_index, m_offset_inside_chunk);
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

        if (remaining_bytes_to_seek == 0) {
            m_chunk_index = 0;
            m_offset_inside_chunk = 0;
            return 0;
        }

        while (remaining_bytes_to_seek > 0) {
            dbgln("seek, remaining bytes: {}", remaining_bytes_to_seek);
            m_waiting_for_more_data.wait_while([this, new_chunk_index] {
                bool should_wait = !m_closed && new_chunk_index >= m_chunks.size();
                dbgln("new_chunk_index = {}, m_chunks.size() = {}, should wait for more data? {}", new_chunk_index, m_chunks.size(), should_wait);
                return should_wait;
            });

            dbgln("seek no longer waiting for data");

            if (m_closed && new_chunk_index >= m_chunks.size()) {
                dbgln("seek not enough data");
                return Error::from_string_literal("Offset past the end of the stream memory");
            }

            auto const* chunk = m_chunks.find(new_chunk_index);

            dbgln("seek remaining bytes to seek: {}, chunk size {}", remaining_bytes_to_seek, chunk->size());

            if (remaining_bytes_to_seek <= chunk->size()) {
                m_chunk_index = new_chunk_index;
                m_offset_inside_chunk = remaining_bytes_to_seek;
                dbgln("seek new chunk index and offset: {}, {}", m_chunk_index, m_offset_inside_chunk);
                remaining_bytes_to_seek = 0;
            } else {
                ++new_chunk_index;
                remaining_bytes_to_seek -= chunk->size();
                dbgln("seek moving to chunk {}", new_chunk_index);
            }
        }

        return offset;
    }
    // FIXME: Support these modes.
    case SeekMode::FromCurrentPosition: {
        size_t current_offset = 0;
        for (size_t chunk_index = 0; chunk_index < m_chunk_index; ++chunk_index)
            current_offset += m_chunks.find(chunk_index)->size();

        current_offset += m_offset_inside_chunk;

        if (offset == 0)
            return current_offset;

        size_t target_offset = current_offset + offset;

        if (target_offset > current_offset) {
            size_t remaining_bytes_to_seek = target_offset - current_offset;
            size_t new_chunk_index = m_chunk_index;
            size_t new_offset_inside_chunk = m_offset_inside_chunk;

            while (remaining_bytes_to_seek > 0) {
                dbgln("seek FROM CURRENT, remaining bytes: {}", remaining_bytes_to_seek);
                m_waiting_for_more_data.wait_while([this, new_chunk_index] {
                    bool should_wait = !m_closed && new_chunk_index >= m_chunks.size();
                    dbgln("seek  FROM CURRENT new_chunk_index = {}, m_chunks.size() = {}, should wait for more data? {}", new_chunk_index, m_chunks.size(), should_wait);
                    return should_wait;
                });

                dbgln("seek FROM CURRENT no longer waiting for data");

                if (m_closed && new_chunk_index >= m_chunks.size()) {
                    dbgln("seek  FROM CURRENT not enough data");
                    return Error::from_string_literal("Offset past the end of the stream memory");
                }

                auto const chunk = m_chunks.find(new_chunk_index)->span().slice(new_offset_inside_chunk);

                dbgln("seek FROM CURRENT remaining bytes to seek: {}, chunk size {}", remaining_bytes_to_seek, chunk.size());

                if (remaining_bytes_to_seek <= chunk.size()) {
                    m_chunk_index = new_chunk_index;
                    m_offset_inside_chunk = new_offset_inside_chunk + remaining_bytes_to_seek;
                    dbgln("seek FROM CURRENT new chunk index and offset: {}, {}", m_chunk_index, m_offset_inside_chunk);
                    remaining_bytes_to_seek = 0;
                } else {
                    ++new_chunk_index;
                    new_offset_inside_chunk = 0;
                    remaining_bytes_to_seek -= chunk.size();
                    dbgln("seek FROM CURRENT moving to chunk {} offset {}", new_chunk_index, new_offset_inside_chunk);
                }
            }
        } else {
            dbgln("FIXME: Seek backwards from current position");
            return Error::from_errno(ENOTSUP);
        }

        return target_offset;
    }
    case SeekMode::FromEndPosition:
        dbgln("FIXME: Seek from end");
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

    if (!m_closed)
        return false;

    if (m_chunks.is_empty())
        return true;

    return m_chunk_index == m_chunks.size() - 1
        && m_offset_inside_chunk == m_chunks.find(m_chunk_index)->size();
}

bool SeekableSharedMemoryStream::is_open() const
{
    Threading::MutexLocker locker(m_mutex);
    return !m_closed;
}

void SeekableSharedMemoryStream::close()
{
    Threading::MutexLocker locker(m_mutex);

    dbgln("stream closed");
    m_closed = true;
    m_waiting_for_more_data.broadcast();
}

void SeekableSharedMemoryStream::append_chunk(ByteBuffer&& chunk)
{
    Threading::MutexLocker locker(m_mutex);

    dbgln("new chunk of size {} added", chunk.size());

    if (m_closed)
        return;

    size_t index = m_chunks.size();
    m_chunks.insert(index, move(chunk));
    m_waiting_for_more_data.broadcast();
}

}
