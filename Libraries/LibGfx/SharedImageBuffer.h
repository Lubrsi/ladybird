/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Variant.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImage.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

namespace Gfx {

class SharedImageBuffer final : public AtomicRefCounted<SharedImageBuffer> {
    AK_MAKE_NONCOPYABLE(SharedImageBuffer);
    AK_MAKE_NONMOVABLE(SharedImageBuffer);

public:
#ifdef USE_VULKAN_DMABUF_IMAGES
    static ErrorOr<NonnullRefPtr<SharedImageBuffer>> create(IntSize, VulkanContext const&);
    static NonnullRefPtr<SharedImageBuffer> import_from_shared_image(SharedImage, VulkanContext const&);
#else
    static NonnullRefPtr<SharedImageBuffer> create(IntSize);
    static NonnullRefPtr<SharedImageBuffer> import_from_shared_image(SharedImage);
#endif

    using ImportedSharedImage = Variant<NonnullRefPtr<SharedImageBuffer>, NonnullRefPtr<Bitmap>>;
#ifdef USE_VULKAN_DMABUF_IMAGES
    // Returns a SharedImageBuffer when the SharedImage carries a DMA-BUF and the consumer
    // has a VulkanContext; returns a Bitmap when the SharedImage carries a ShareableBitmap
    // or when no Vulkan context is available.
    static ImportedSharedImage import_shared_image(SharedImage, VulkanContext const*);
#else
    static ImportedSharedImage import_shared_image(SharedImage);
#endif

    // Context-less import for consumers that only need CPU access (e.g. the UI process).
    static NonnullRefPtr<Bitmap> import_bitmap_from_shared_image(SharedImage);

    ~SharedImageBuffer();

    SharedImage export_shared_image() const;

    NonnullRefPtr<Bitmap> bitmap() const { return m_bitmap; }
    IntSize size() const { return m_bitmap->size(); }

#ifdef AK_OS_MACOS
    Core::IOSurfaceHandle const& iosurface_handle() const { return m_iosurface_handle; }
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    NonnullRefPtr<VulkanImage const> vulkan_image() const { return m_vulkan_image; }
#endif

private:
#ifdef AK_OS_MACOS
    SharedImageBuffer(Core::IOSurfaceHandle&&, NonnullRefPtr<Bitmap>);
    Core::IOSurfaceHandle m_iosurface_handle;
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    SharedImageBuffer(NonnullRefPtr<VulkanImage>, NonnullRefPtr<Bitmap>);
    NonnullRefPtr<VulkanImage> m_vulkan_image;
#else
    explicit SharedImageBuffer(NonnullRefPtr<Bitmap>);
#endif
    NonnullRefPtr<Bitmap> m_bitmap;
};

}
