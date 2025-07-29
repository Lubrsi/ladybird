/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <core/SkSurface.h>

#include <gpu/graphite/Context.h>
#include <gpu/graphite/ContextOptions.h>
#include <gpu/graphite/Image.h>
#include <gpu/graphite/ImageProvider.h>

#include <AK/Platform.h>
#include <AK/HashMap.h>

#ifdef USE_VULKAN
#    include <gpu/ganesh/vk/GrVkDirectContext.h>
#    include <gpu/vk/VulkanBackendContext.h>
#    include <gpu/vk/VulkanExtensions.h>
#endif

#ifdef AK_OS_MACOS
#    include <gpu/graphite/mtl/MtlBackendContext.h>
#endif

#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SkiaBackendContext.h>

namespace Gfx {

class GraphiteImageProvider final : public skgpu::graphite::ImageProvider {
public:
    struct CacheKey {
        skgpu::graphite::Recorder const* sk_recorder;
        SkImage const* sk_image;
        SkImage::RequiredProperties required_properties;
    };

    static sk_sp<GraphiteImageProvider> create()
    {
        return sk_sp(new GraphiteImageProvider());
    }

    virtual sk_sp<SkImage> findOrCreate(skgpu::graphite::Recorder* sk_recorder, SkImage const* sk_image, SkImage::RequiredProperties required_properties) override
    {
        auto cache_key = CacheKey {
            .sk_recorder = sk_recorder,
            .sk_image = sk_image,
            .required_properties = required_properties,
        };

        auto image_cache_iterator = m_image_cache.find(cache_key);
        if (image_cache_iterator != m_image_cache.end())
            return image_cache_iterator->value;

        auto graphite_backed_image = SkImages::TextureFromImage(sk_recorder, sk_image, required_properties);
        m_image_cache.set(cache_key, graphite_backed_image);
        return graphite_backed_image;
    }

private:
    GraphiteImageProvider() = default;

    HashMap<CacheKey, sk_sp<SkImage>> m_image_cache;
};

#ifdef USE_VULKAN
class SkiaVulkanBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaVulkanBackendContext);
    AK_MAKE_NONMOVABLE(SkiaVulkanBackendContext);

public:
    SkiaVulkanBackendContext(sk_sp<GrDirectContext> context, NonnullOwnPtr<skgpu::VulkanExtensions> extensions)
        : m_context(move(context))
        , m_extensions(move(extensions))
    {
    }

    ~SkiaVulkanBackendContext() override { }

    void flush_and_submit(SkSurface* surface) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    skgpu::VulkanExtensions const* extensions() const { return m_extensions.ptr(); }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullOwnPtr<skgpu::VulkanExtensions> m_extensions;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_vulkan_context(Gfx::VulkanContext& vulkan_context)
{
    skgpu::VulkanBackendContext backend_context;

    backend_context.fInstance = vulkan_context.instance;
    backend_context.fDevice = vulkan_context.logical_device;
    backend_context.fQueue = vulkan_context.graphics_queue;
    backend_context.fPhysicalDevice = vulkan_context.physical_device;
    backend_context.fMaxAPIVersion = vulkan_context.api_version;
    backend_context.fGetProc = [](char const* proc_name, VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            return vkGetDeviceProcAddr(device, proc_name);
        }
        return vkGetInstanceProcAddr(instance, proc_name);
    };

    auto extensions = make<skgpu::VulkanExtensions>();
    backend_context.fVkExtensions = extensions.ptr();

    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeVulkan(backend_context);
    VERIFY(ctx);
    return adopt_ref(*new SkiaVulkanBackendContext(ctx, move(extensions)));
}
#endif

#ifdef AK_OS_MACOS
class SkiaMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaMetalBackendContext);

public:
    SkiaMetalBackendContext(std::unique_ptr<skgpu::graphite::Context> context, std::unique_ptr<skgpu::graphite::Recorder> recorder, NonnullRefPtr<MetalContext> metal_context)
        : m_context(move(context))
        , m_recorder(move(recorder))
        , m_metal_context(move(metal_context))
    {
    }

    ~SkiaMetalBackendContext() override { }

    void flush_and_submit() override
    {
        auto recording = m_recorder->snap();
        m_context->insertRecording({ recording.get() });
        m_context->submit(skgpu::graphite::SyncToCpu::kYes);
    }

    skgpu::graphite::Context* sk_context() const override { return m_context.get(); }
    skgpu::graphite::Recorder* sk_recorder() const override { return m_recorder.get(); }
    MetalContext& metal_context() override { return m_metal_context; }

private:
    std::unique_ptr<skgpu::graphite::Context> m_context;
    std::unique_ptr<skgpu::graphite::Recorder> m_recorder;
    NonnullRefPtr<MetalContext> m_metal_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_metal_context(NonnullRefPtr<MetalContext> metal_context)
{
    skgpu::graphite::MtlBackendContext backend_context;
    backend_context.fDevice.retain(metal_context->device());
    backend_context.fQueue.retain(metal_context->queue());

    skgpu::graphite::ContextOptions context_options {};
    std::unique_ptr<skgpu::graphite::Context> ctx = skgpu::graphite::ContextFactory::MakeMetal(backend_context, context_options);

    skgpu::graphite::RecorderOptions recorder_options {};
    recorder_options.fImageProvider = GraphiteImageProvider::create();
    std::unique_ptr<skgpu::graphite::Recorder> recorder = ctx->makeRecorder(recorder_options);

    return adopt_ref(*new SkiaMetalBackendContext(move(ctx), move(recorder), move(metal_context)));
}
#endif

}

namespace AK {

template<>
struct Traits<Gfx::GraphiteImageProvider::CacheKey> : public AK::DefaultTraits<Gfx::GraphiteImageProvider::CacheKey> {
    static unsigned hash(Gfx::GraphiteImageProvider::CacheKey const& cache_key)
    {
        return pair_int_hash(
            Traits<bool>::hash(cache_key.required_properties.fMipmapped),
            pair_int_hash(ptr_hash(cache_key.sk_recorder), ptr_hash(cache_key.sk_image)));
    }

    static bool equals(Gfx::GraphiteImageProvider::CacheKey const& a, Gfx::GraphiteImageProvider::CacheKey const& b)
    {
        return a.sk_recorder == b.sk_recorder
            && a.sk_image == b.sk_image
            && a.required_properties == b.required_properties;
    }
};

}
