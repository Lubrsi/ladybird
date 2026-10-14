/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibWeb/Fetch/Engine/BodyIntent.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Infrastructure {

// The state of a fetch that its controller and its fetch params share.
class FetchControlBlock : public RefCounted<FetchControlBlock> {
public:
    enum class State : u8 {
        Ongoing,
        Terminated,
        Aborted,
        Stopped,
    };

    [[nodiscard]] static NonnullRefPtr<FetchControlBlock> create() { return adopt_ref(*new FetchControlBlock); }

    [[nodiscard]] State state() const { return m_state; }
    void set_state(Badge<FetchController>, State state) { m_state = state; }

    [[nodiscard]] RefPtr<FetchTimingInfo> full_timing_info() const { return m_full_timing_info; }
    void set_full_timing_info(NonnullRefPtr<FetchTimingInfo> full_timing_info) { m_full_timing_info = move(full_timing_info); }

    [[nodiscard]] Engine::BodyIntent body_intent() const { return m_body_intent; }
    void set_body_intent(Badge<FetchController>, Engine::BodyIntent body_intent) { m_body_intent = body_intent; }
    void retarget_body_intent(Engine::BodyIntent body_intent)
    {
        VERIFY(!m_response_arrived);
        m_body_intent = body_intent;
    }

    // AD-HOC: Whether this fetch's response has arrived: as headers from the network, or at main fetch. From then on,
    //         its body delivery is queued as event-loop tasks, and its response processing captures its task
    //         destination as it was then — so re-targeting the fetch no longer reaches either.
    //         consume_a_preloaded_resource() checks this before re-targeting an in-flight preload's fetch.
    void set_response_arrived() { m_response_arrived = true; }
    [[nodiscard]] bool response_arrived() const { return m_response_arrived; }

    [[nodiscard]] bool has_started_request() const { return m_has_started_request; }
    void set_has_started_request(Badge<FetchController>) { m_has_started_request = true; }

    [[nodiscard]] bool requires_network() const { return m_requires_network; }
    void set_requires_network() { m_requires_network = true; }

private:
    FetchControlBlock() = default;

    // https://fetch.spec.whatwg.org/#fetch-controller-state
    // state (default "ongoing")
    //    "ongoing", "terminated", or "aborted"
    // NB: This is the state of this block's fetch controller.
    State m_state { State::Ongoing };

    // https://fetch.spec.whatwg.org/#fetch-controller-full-timing-info
    // full timing info (default null)
    //    Null or a fetch timing info.
    // NB: This is the full timing info of this block's fetch controller.
    RefPtr<FetchTimingInfo> m_full_timing_info;

    Engine::BodyIntent m_body_intent { Engine::BodyIntent::DrainAndDiscard };
    bool m_response_arrived { false };
    bool m_has_started_request { false };
    bool m_requires_network { false };
};

}
