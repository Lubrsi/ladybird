/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImage.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Gfx {

class SharedImageBuffer final : public AtomicRefCounted<SharedImageBuffer> {
    AK_MAKE_NONCOPYABLE(SharedImageBuffer);
    AK_MAKE_NONMOVABLE(SharedImageBuffer);

public:
    static NonnullRefPtr<SharedImageBuffer> create(IntSize);
    static NonnullRefPtr<SharedImageBuffer> import_from_shared_image(SharedImage);

    ~SharedImageBuffer();

    SharedImage export_shared_image() const;

    NonnullRefPtr<Bitmap> bitmap() const { return m_bitmap; }
    IntSize size() const { return m_bitmap->size(); }

#ifdef AK_OS_MACOS
    Core::IOSurfaceHandle const& iosurface_handle() const { return m_iosurface_handle; }
#endif

private:
#ifdef AK_OS_MACOS
    SharedImageBuffer(Core::IOSurfaceHandle&&, NonnullRefPtr<Bitmap>);
    Core::IOSurfaceHandle m_iosurface_handle;
#else
    explicit SharedImageBuffer(NonnullRefPtr<Bitmap>);
#endif
    NonnullRefPtr<Bitmap> m_bitmap;
};

}
