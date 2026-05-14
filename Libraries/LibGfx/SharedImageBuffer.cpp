/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibIPC/File.h>
#    include <libdrm/drm_fourcc.h>
#    include <sys/mman.h>
#endif

namespace Gfx {

static constexpr auto shared_image_buffer_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_buffer_alpha_type = AlphaType::Premultiplied;

#ifdef AK_OS_MACOS
static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(Core::IOSurfaceHandle const& iosurface_handle)
{
    auto size = IntSize(static_cast<int>(iosurface_handle.width()), static_cast<int>(iosurface_handle.height()));
    auto bitmap_handle = Core::IOSurfaceHandle::from_mach_port(iosurface_handle.create_mach_port());
    return MUST(Bitmap::create_wrapper(shared_image_buffer_format, shared_image_buffer_alpha_type, size, iosurface_handle.bytes_per_row(), iosurface_handle.data(), [handle = move(bitmap_handle)] { }));
}

SharedImageBuffer::SharedImageBuffer(Core::IOSurfaceHandle&& iosurface_handle, NonnullRefPtr<Bitmap> bitmap)
    : m_iosurface_handle(move(iosurface_handle))
    , m_bitmap(move(bitmap))
{
}
#elif defined(USE_VULKAN_DMABUF_IMAGES)
static constexpr auto shared_image_buffer_drm_format = DRM_FORMAT_ARGB8888;

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf(LinuxDmaBufHandle const& dmabuf)
{
    VERIFY(dmabuf.bitmap_format == shared_image_buffer_format);
    VERIFY(dmabuf.alpha_type == shared_image_buffer_alpha_type);
    VERIFY(dmabuf.drm_format == shared_image_buffer_drm_format);
    VERIFY(dmabuf.modifier == DRM_FORMAT_MOD_LINEAR);
    auto data_size = Bitmap::size_in_bytes(dmabuf.pitch, dmabuf.size.height());
    auto* data = ::mmap(nullptr, data_size, PROT_READ, MAP_SHARED, dmabuf.file.fd(), 0);
    VERIFY(data != MAP_FAILED);
    return MUST(Bitmap::create_wrapper(dmabuf.bitmap_format, dmabuf.alpha_type, dmabuf.size, dmabuf.pitch, data, [data, data_size] {
        VERIFY(::munmap(data, data_size) == 0);
    }));
}

static NonnullRefPtr<Bitmap> create_bitmap_from_vulkan_image(VulkanImage const& vulkan_image)
{
    auto handle = duplicate_linux_dmabuf_handle(vulkan_image);
    return create_bitmap_from_linux_dmabuf(handle);
}

SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<VulkanImage> vulkan_image, NonnullRefPtr<Bitmap> bitmap)
    : m_vulkan_image(move(vulkan_image))
    , m_bitmap(move(bitmap))
{
}
#else
SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<Bitmap> bitmap)
    : m_bitmap(move(bitmap))
{
}
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
ErrorOr<NonnullRefPtr<SharedImageBuffer>> SharedImageBuffer::create(IntSize size, VulkanContext const& vulkan_context)
{
    Array<uint64_t, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto vulkan_image = TRY(create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto bitmap = create_bitmap_from_vulkan_image(*vulkan_image);
    return adopt_ref(*new SharedImageBuffer(move(vulkan_image), move(bitmap)));
}

NonnullRefPtr<SharedImageBuffer> SharedImageBuffer::import_from_shared_image(SharedImage shared_image, VulkanContext const& vulkan_context)
{
    return shared_image.m_data.visit(
        [](ShareableBitmap&) -> NonnullRefPtr<SharedImageBuffer> {
            VERIFY_NOT_REACHED();
        },
        [&](LinuxDmaBufHandle& dmabuf) -> NonnullRefPtr<SharedImageBuffer> {
            auto cloned = MUST(IPC::File::clone_fd(dmabuf.file.fd()));
            auto vulkan_image = MUST(wrap_dmabuf_as_vulkan_image(vulkan_context, cloned.take_fd(), dmabuf.size.width(), dmabuf.size.height(), dmabuf.pitch, VK_FORMAT_B8G8R8A8_UNORM, dmabuf.modifier));
            auto bitmap = create_bitmap_from_linux_dmabuf(dmabuf);
            return adopt_ref(*new SharedImageBuffer(move(vulkan_image), move(bitmap)));
        });
}
#else
NonnullRefPtr<SharedImageBuffer> SharedImageBuffer::create(IntSize size)
{
#    ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return adopt_ref(*new SharedImageBuffer(move(iosurface_handle), move(bitmap)));
#    else
    return adopt_ref(*new SharedImageBuffer(MUST(Bitmap::create_shareable(shared_image_buffer_format, shared_image_buffer_alpha_type, size))));
#    endif
}

NonnullRefPtr<SharedImageBuffer> SharedImageBuffer::import_from_shared_image(SharedImage shared_image)
{
#    ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::from_mach_port(shared_image.m_port);
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return adopt_ref(*new SharedImageBuffer(move(iosurface_handle), move(bitmap)));
#    else
    return shared_image.m_data.visit(
        [](ShareableBitmap& shareable_bitmap) -> NonnullRefPtr<SharedImageBuffer> {
            return adopt_ref(*new SharedImageBuffer(*shareable_bitmap.bitmap()));
        },
        [](LinuxDmaBufHandle&) -> NonnullRefPtr<SharedImageBuffer> {
            VERIFY_NOT_REACHED();
        });
#    endif
}
#endif

NonnullRefPtr<Bitmap> SharedImageBuffer::import_bitmap_from_shared_image(SharedImage shared_image)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::from_mach_port(shared_image.m_port);
    return create_bitmap_from_iosurface(iosurface_handle);
#else
    return shared_image.m_data.visit(
        [](ShareableBitmap& shareable_bitmap) -> NonnullRefPtr<Bitmap> {
            return *shareable_bitmap.bitmap();
        },
        [](LinuxDmaBufHandle& dmabuf) -> NonnullRefPtr<Bitmap> {
#    ifdef USE_VULKAN_DMABUF_IMAGES
            return create_bitmap_from_linux_dmabuf(dmabuf);
#    else
            (void)dmabuf;
            VERIFY_NOT_REACHED();
#    endif
        });
#endif
}

SharedImageBuffer::~SharedImageBuffer() = default;

SharedImage SharedImageBuffer::export_shared_image() const
{
#ifdef AK_OS_MACOS
    return SharedImage { m_iosurface_handle.create_mach_port() };
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    return SharedImage { duplicate_linux_dmabuf_handle(*m_vulkan_image) };
#else
    return SharedImage { ShareableBitmap { m_bitmap, ShareableBitmap::ConstructWithKnownGoodBitmap } };
#endif
}

}
