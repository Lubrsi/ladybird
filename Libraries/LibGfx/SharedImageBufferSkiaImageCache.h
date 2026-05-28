/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Forward.h>

class SkImage;

template<typename T>
class sk_sp;

namespace Gfx {

class SharedImageBufferSkiaImageCache final {
public:
    explicit SharedImageBufferSkiaImageCache(RefPtr<SkiaBackendContext>);
    ~SharedImageBufferSkiaImageCache();

    // Returns nullptr when the cache has no SkiaBackendContext, when the buffer has no
    // GPU-importable handle on this platform, or when the cross-context wrap fails.
    sk_sp<SkImage> image_for_buffer(SharedImageBuffer const&);

    void prune();

private:
    struct Impl;
    OwnPtr<Impl> m_impl;
};

}
