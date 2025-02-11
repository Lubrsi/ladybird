/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLDebugRendererInfoPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebGL/Extensions/WebGLDebugRendererInfo.h>

namespace Web::WebGL::Extensions {

GC_DEFINE_ALLOCATOR(WebGLDebugRendererInfo);

JS::ThrowCompletionOr<GC::Ptr<WebGLDebugRendererInfo>> WebGLDebugRendererInfo::create(JS::Realm& realm)
{
    return realm.create<WebGLDebugRendererInfo>(realm);
}

WebGLDebugRendererInfo::WebGLDebugRendererInfo(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void WebGLDebugRendererInfo::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLDebugRendererInfo);
}

}
