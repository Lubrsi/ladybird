/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/CaptureActivity.h>
#include <LibCore/Timer.h>

namespace AudioServer {

CaptureActivity::CaptureActivity(AK::Duration linger, Report report)
    : m_linger(linger)
    , m_report(move(report))
{
}

CaptureActivity::~CaptureActivity()
{
    for (auto& [tab_id, groups] : m_tabs) {
        for (auto& [group_id, group] : groups) {
            if (group.linger_timer)
                group.linger_timer->stop();
        }
    }
}

void CaptureActivity::stream_started(u64 tab_id, Audio::PlaybackGroupId group_id)
{
    auto& groups = m_tabs.ensure(tab_id);
    auto is_new = !groups.contains(group_id);
    auto& group = groups.ensure(group_id);
    if (group.linger_timer) {
        group.linger_timer->stop();
        group.linger_timer = nullptr;
    }
    ++group.stream_count;
    if (is_new)
        m_report(tab_id, group_id, true);
}

void CaptureActivity::stream_stopped(u64 tab_id, Audio::PlaybackGroupId group_id)
{
    auto groups = m_tabs.get(tab_id);
    if (!groups.has_value())
        return;
    auto group = groups->get(group_id);
    if (!group.has_value() || group->stream_count == 0)
        return;
    if (--group->stream_count > 0)
        return;
    group->linger_timer = Core::Timer::create_single_shot(m_linger, [this, tab_id, group_id] {
        linger_ended(tab_id, group_id);
    });
    group->linger_timer->start();
}

void CaptureActivity::linger_ended(u64 tab_id, Audio::PlaybackGroupId group_id)
{
    auto groups = m_tabs.get(tab_id);
    if (!groups.has_value())
        return;
    auto group = groups->get(group_id);
    if (!group.has_value() || group->stream_count > 0)
        return;
    groups->remove(group_id);
    if (groups->is_empty())
        m_tabs.remove(tab_id);
    m_report(tab_id, group_id, false);
}

}
