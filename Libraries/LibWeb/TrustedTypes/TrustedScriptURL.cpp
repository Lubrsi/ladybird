/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrustedScriptURLPrototype.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedScriptURL);

TrustedScriptURL::TrustedScriptURL(JS::Realm& realm, Utf16String data)
    : Bindings::PlatformObject(realm)
    , m_data(move(data))
{
}

TrustedScriptURL::~TrustedScriptURL() = default;

void TrustedScriptURL::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedScriptURL);
    Base::initialize(realm);
}

// https://www.w3.org/TR/trusted-types/#trustedscripturl-stringification-behavior
Utf16String const& TrustedScriptURL::to_string()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedScriptURL object are to return the
    // associated data value.
    return m_data;
}

// https://www.w3.org/TR/trusted-types/#dom-trustedscripturl-tojson
Utf16String const& TrustedScriptURL::to_json()
{
    // toJSON() method steps and the stringification behavior steps of a TrustedScriptURL object are to return the
    // associated data value.
    return m_data;
}

}
