/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrustedHTMLPrototype.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedHTML);

TrustedHTML::TrustedHTML(JS::Realm& realm, Utf16String data)
    : Bindings::PlatformObject(realm)
    , m_data(move(data))
{
}

TrustedHTML::~TrustedHTML() = default;

void TrustedHTML::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedHTML);
    Base::initialize(realm);
}

// https://www.w3.org/TR/trusted-types/#trustedhtml-stringification-behavior
Utf16String const& TrustedHTML::to_string()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedHTML object are to return the
    // associated data value.
    return m_data;
}

// https://www.w3.org/TR/trusted-types/#dom-trustedhtml-tojson
Utf16String const& TrustedHTML::to_json()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedHTML object are to return the
    // associated data value.
    return m_data;
}

}
