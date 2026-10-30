/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <AudioServer/CaptureActivity.h>
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>

static constexpr auto LINGER = AK::Duration::from_milliseconds(50);
static constexpr auto PAST_THE_LINGER = AK::Duration::from_milliseconds(150);
static constexpr Audio::PlaybackGroupId GROUP { 7 };

struct Report {
    u64 tab_id;
    Audio::PlaybackGroupId group;
    bool capturing;

    bool operator==(Report const&) const = default;
};

struct ActivityFixture {
    Core::EventLoop loop;
    Vector<Report> reports;
    AudioServer::CaptureActivity activity;

    ActivityFixture()
        : activity(LINGER, [this](u64 tab_id, Audio::PlaybackGroupId group, bool capturing) {
            reports.append({ tab_id, group, capturing });
        })
    {
    }

    void pump_for(AK::Duration duration)
    {
        auto deadline = MonotonicTime::now() + duration;
        while (MonotonicTime::now() < deadline)
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    }
};

TEST_CASE(the_first_stream_of_a_group_reports_capture_at_once)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true } }));
}

TEST_CASE(the_group_captures_until_its_last_stream_has_been_gone_for_the_linger)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    fixture.pump_for(PAST_THE_LINGER);
    EXPECT_EQ(fixture.reports.size(), 1u);

    fixture.activity.stream_stopped(1, GROUP);
    EXPECT_EQ(fixture.reports.size(), 1u);
    fixture.pump_for(PAST_THE_LINGER);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true }, { 1, GROUP, false } }));
}

TEST_CASE(a_stream_started_during_the_linger_carries_the_capture_on)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    fixture.activity.stream_started(1, GROUP);
    fixture.pump_for(PAST_THE_LINGER);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true } }));
}

TEST_CASE(groups_and_tabs_capture_apart)
{
    ActivityFixture fixture;
    fixture.activity.stream_started(1, GROUP);
    fixture.activity.stream_started(1, Audio::PlaybackGroupId { 8 });
    fixture.activity.stream_started(2, GROUP);
    fixture.activity.stream_stopped(1, GROUP);
    fixture.pump_for(PAST_THE_LINGER);
    EXPECT_EQ(fixture.reports, (Vector<Report> { { 1, GROUP, true }, { 1, Audio::PlaybackGroupId { 8 }, true }, { 2, GROUP, true }, { 1, GROUP, false } }));
}

TEST_CASE(a_stop_without_a_start_is_ignored)
{
    ActivityFixture fixture;
    fixture.activity.stream_stopped(1, GROUP);
    fixture.pump_for(PAST_THE_LINGER);
    EXPECT(fixture.reports.is_empty());
}
