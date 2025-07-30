/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::TrustedTypes {

class TrustedHTML final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedHTML, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedHTML);

public:
    virtual ~TrustedHTML() override;

    Utf16String const& to_string();
    Utf16String const& to_json();

private:
    TrustedHTML(JS::Realm&, Utf16String data);
    virtual void initialize(JS::Realm&) override;

    // TrustedHTML objects have an associated string data. The value is set when the object is created, and will never
    // change during its lifetime.
    Utf16String m_data;
};

}
