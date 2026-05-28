/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SharedImageBufferSkiaImageCache.h>
#include <LibGfx/SkiaBackendContext.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkRefCnt.h>
#include <gpu/ganesh/GrBackendSurface.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#elif defined(USE_VULKAN_DMABUF_IMAGES)
#    include <LibGfx/VulkanImage.h>
#    include <gpu/ganesh/vk/GrVkBackendSurface.h>
#    include <gpu/ganesh/vk/GrVkTypes.h>
#endif

namespace Gfx {

static constexpr size_t image_cache_max_entries = 128;
static constexpr size_t image_cache_max_bytes = 64 * MiB;

#ifdef USE_VULKAN_DMABUF_IMAGES
static SkColorType vk_format_to_sk_color_type(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return kBGRA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
        return kUnknown_SkColorType;
    }
}

static void release_vulkan_image(void* context)
{
    VulkanImage* image = static_cast<VulkanImage*>(context);
    image->unref();
}
#endif

struct SharedImageBufferSkiaImageCache::Impl {
    explicit Impl(RefPtr<SkiaBackendContext> skia_backend_context)
        : skia_backend_context(move(skia_backend_context))
    {
    }

    struct CachedImage {
        sk_sp<SkImage> image;
        NonnullRefPtr<SharedImageBuffer const> keep_alive;
        u64 last_used_sequence_number { 0 };
        size_t approximate_byte_size { 0 };
    };

    u64 next_use_sequence_number()
    {
        return ++use_sequence_number;
    }

    void prune_to_limits()
    {
        while (images.size() > image_cache_max_entries || approximate_byte_size > image_cache_max_bytes) {
            SharedImageBuffer const* least_recently_used_key = nullptr;
            Optional<u64> least_recently_used_sequence_number;
            for (auto const& entry : images) {
                if (!least_recently_used_sequence_number.has_value()
                    || entry.value.last_used_sequence_number < least_recently_used_sequence_number.value()) {
                    least_recently_used_key = entry.key;
                    least_recently_used_sequence_number = entry.value.last_used_sequence_number;
                }
            }
            if (!least_recently_used_key)
                break;
            auto cached_image = images.take(least_recently_used_key).release_value();
            approximate_byte_size -= min(approximate_byte_size, cached_image.approximate_byte_size);
        }
    }

    RefPtr<SkiaBackendContext> skia_backend_context;
    HashMap<SharedImageBuffer const*, CachedImage> images;
    size_t approximate_byte_size { 0 };
    u64 use_sequence_number { 0 };
};

SharedImageBufferSkiaImageCache::SharedImageBufferSkiaImageCache(RefPtr<SkiaBackendContext> skia_backend_context)
    : m_impl(make<Impl>(move(skia_backend_context)))
{
}

SharedImageBufferSkiaImageCache::~SharedImageBufferSkiaImageCache() = default;

#ifdef AK_OS_MACOS
static sk_sp<SkImage> wrap_buffer_as_skimage(GrDirectContext& gr_context, MetalContext& metal_context, SharedImageBuffer const& buffer)
{
    auto metal_texture = metal_context.create_texture_from_iosurface(buffer.iosurface_handle());
    GrMtlTextureInfo mtl_info;
    mtl_info.fTexture = sk_ret_cfp(metal_texture->texture());
    auto backend_texture = GrBackendTextures::MakeMtl(metal_texture->width(), metal_texture->height(), skgpu::Mipmapped::kNo, mtl_info);
    // The underlying Obj-C texture is retained by Skia via sk_ret_cfp; the OwnPtr<MetalTexture>
    // dies at end of scope without affecting the wrapped backend texture.
    return SkImages::BorrowTextureFrom(&gr_context, backend_texture, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB(), nullptr, nullptr);
}
#elif defined(USE_VULKAN_DMABUF_IMAGES)
static sk_sp<SkImage> wrap_buffer_as_skimage(GrDirectContext& gr_context, SharedImageBuffer const& buffer)
{
    auto vulkan_image = buffer.vulkan_image();
    GrVkImageInfo info = {
        .fImage = vulkan_image->image,
        .fAlloc = {},
        .fImageTiling = vulkan_image->info.tiling,
        .fImageLayout = vulkan_image->info.layout,
        .fFormat = vulkan_image->info.format,
        .fImageUsageFlags = vulkan_image->info.usage,
        .fSampleCount = 1,
        .fLevelCount = 1,
        .fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED,
        .fProtected = skgpu::Protected::kNo,
        .fYcbcrConversionInfo = {},
        .fSharingMode = vulkan_image->info.sharing_mode,
    };
    auto backend_texture = GrBackendTextures::MakeVk(vulkan_image->info.extent.width, vulkan_image->info.extent.height, info);
    // Hand Skia an extra ref balanced by release_vulkan_image; matches create_from_vkimage.
    vulkan_image->ref();
    return SkImages::BorrowTextureFrom(&gr_context, backend_texture, kTopLeft_GrSurfaceOrigin, vk_format_to_sk_color_type(vulkan_image->info.format), kPremul_SkAlphaType, SkColorSpace::MakeSRGB(), release_vulkan_image, const_cast<VulkanImage*>(vulkan_image.ptr()));
}
#endif

sk_sp<SkImage> SharedImageBufferSkiaImageCache::image_for_buffer(SharedImageBuffer const& buffer)
{
    auto* gr_context = m_impl->skia_backend_context ? m_impl->skia_backend_context->sk_context() : nullptr;
    if (!gr_context)
        return nullptr;

    if (auto it = m_impl->images.find(&buffer); it != m_impl->images.end()) {
        it->value.last_used_sequence_number = m_impl->next_use_sequence_number();
        return it->value.image;
    }

    sk_sp<SkImage> image;
#ifdef AK_OS_MACOS
    image = wrap_buffer_as_skimage(*gr_context, m_impl->skia_backend_context->metal_context(), buffer);
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    image = wrap_buffer_as_skimage(*gr_context, buffer);
#else
    (void)gr_context;
    return nullptr;
#endif
    if (!image)
        return nullptr;

    Impl::CachedImage cached_image {
        .image = image,
        .keep_alive = buffer,
        .last_used_sequence_number = m_impl->next_use_sequence_number(),
        .approximate_byte_size = buffer.bitmap()->size_in_bytes(),
    };
    m_impl->approximate_byte_size += cached_image.approximate_byte_size;
    m_impl->images.set(&buffer, move(cached_image));
    m_impl->prune_to_limits();
    return image;
}

void SharedImageBufferSkiaImageCache::prune()
{
    m_impl->prune_to_limits();
}

}
