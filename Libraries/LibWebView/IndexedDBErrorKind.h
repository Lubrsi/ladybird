/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Types.h>

namespace WebView {

// What went wrong with something that was asked of an IndexedDB database, as far as whoever asked is to know.
enum class IndexedDBErrorKind : u8 {
    Constraint,
    Version,
    // There was no room for what was asked, in the budget of its storage key or in its session.
    Quota,
    // The request is a writer's that has finished: it committed, was aborted, or lost its SQL transaction.
    TransactionLost,
    // The request is one that no renderer running LibWeb sends.
    InvalidClaim,
    OutOfMemory,
    Unknown,
};

}

template<>
struct AK::Formatter<WebView::IndexedDBErrorKind> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, WebView::IndexedDBErrorKind kind)
    {
        switch (kind) {
        case WebView::IndexedDBErrorKind::Constraint:
            return Formatter<StringView>::format(builder, "Constraint"sv);
        case WebView::IndexedDBErrorKind::Version:
            return Formatter<StringView>::format(builder, "Version"sv);
        case WebView::IndexedDBErrorKind::Quota:
            return Formatter<StringView>::format(builder, "Quota"sv);
        case WebView::IndexedDBErrorKind::TransactionLost:
            return Formatter<StringView>::format(builder, "TransactionLost"sv);
        case WebView::IndexedDBErrorKind::InvalidClaim:
            return Formatter<StringView>::format(builder, "InvalidClaim"sv);
        case WebView::IndexedDBErrorKind::OutOfMemory:
            return Formatter<StringView>::format(builder, "OutOfMemory"sv);
        case WebView::IndexedDBErrorKind::Unknown:
            return Formatter<StringView>::format(builder, "Unknown"sv);
        }
        VERIFY_NOT_REACHED();
    }
};
