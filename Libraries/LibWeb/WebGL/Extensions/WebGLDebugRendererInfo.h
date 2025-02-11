/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::WebGL::Extensions {

class WebGLDebugRendererInfo : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLDebugRendererInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(WebGLDebugRendererInfo);

public:
    static JS::ThrowCompletionOr<GC::Ptr<WebGLDebugRendererInfo>> create(JS::Realm&);

protected:
    void initialize(JS::Realm&) override;

private:
    WebGLDebugRendererInfo(JS::Realm&);
};

}
