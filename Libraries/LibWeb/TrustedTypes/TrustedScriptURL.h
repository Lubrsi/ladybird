/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::TrustedTypes {

class TrustedScriptURL final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedScriptURL, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedScriptURL);

public:
    virtual ~TrustedScriptURL() override;

    Utf16String const& to_string();
    Utf16String const& to_json();

private:
    TrustedScriptURL(JS::Realm&, Utf16String data);
    virtual void initialize(JS::Realm&) override;

    // TrustedScriptURL objects have an associated string data. The value is set when the object is created, and will
    // never change during its lifetime.
    Utf16String m_data;
};

}
