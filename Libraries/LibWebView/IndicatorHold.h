/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Export.h>

namespace WebView {

// Keeps a tab's indicator up after a page that lit it leaves the tab, for as long as the indicator would have lingered
// had the page stopped as it left. A page leaves once each time it begins hosting the tab's documents. Closing isn't
// leaving: a closed page holds nothing, and ends no hold.
class WEBVIEW_API IndicatorHold {
public:
    explicit IndicatorHold(AK::Duration linger)
        : m_linger(linger)
    {
    }

    void page_began_hosting(Web::PageId);
    bool page_left(Web::PageId, MonotonicTime now);
    void page_closed(Web::PageId);

    bool is_held(MonotonicTime now) const;
    Optional<MonotonicTime> held_until() const { return m_held_until; }

private:
    AK::Duration m_linger;
    HashTable<Web::PageId> m_left_pages;
    Optional<MonotonicTime> m_held_until;
};

}
