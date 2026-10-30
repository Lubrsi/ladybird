/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibAudio/Audibility.h>
#include <LibAudio/PlaybackGroupId.h>
#include <LibCore/Forward.h>

namespace AudioServer {

// Which groups of which tabs capture, for the Browser to show. A group captures from the moment a stream of it may, and
// whether the device delivers anything or not, until its last stream has been gone for the linger.
class CaptureActivity {
    AK_MAKE_NONCOPYABLE(CaptureActivity);
    AK_MAKE_NONMOVABLE(CaptureActivity);

public:
    using Report = Function<void(u64 tab_id, Audio::PlaybackGroupId, bool capturing)>;

    CaptureActivity(AK::Duration linger, Report);
    ~CaptureActivity();

    void stream_started(u64 tab_id, Audio::PlaybackGroupId);
    void stream_stopped(u64 tab_id, Audio::PlaybackGroupId);

private:
    struct Group {
        size_t stream_count { 0 };
        RefPtr<Core::Timer> linger_timer;
    };

    void linger_ended(u64 tab_id, Audio::PlaybackGroupId);

    AK::Duration m_linger;
    Report m_report;
    // A group is here exactly while the Browser has been told it captures.
    HashMap<u64, HashMap<Audio::PlaybackGroupId, Group>> m_tabs;
};

}
