/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/VulkanContext.h>

namespace Gfx {
static ErrorOr<VkInstance> create_instance(uint32_t api_version)
{
    VkInstance instance;

    VkApplicationInfo app_info {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Ladybird";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = nullptr;
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = api_version;

    Array<char const*, 3> required_extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    Array<char const*, 1> required_layers = {
        "VK_LAYER_KHRONOS_validation",
    };

    VkInstanceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = required_extensions.size();
    create_info.ppEnabledExtensionNames = required_extensions.data();
    create_info.enabledLayerCount = required_layers.size();
    create_info.ppEnabledLayerNames = required_layers.data();

    auto result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateInstance returned {}", to_underlying(result));
        return Error::from_string_literal("Application instance creation failed");
    }

    return instance;
}

static ErrorOr<VkPhysicalDevice> pick_physical_device(VkInstance instance)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0)
        return Error::from_string_literal("Can't find any physical devices available");

    Vector<VkPhysicalDevice> devices;
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    VkPhysicalDevice picked_device = VK_NULL_HANDLE;
    // Pick discrete GPU or the first device in the list
    for (auto const& device : devices) {
        if (picked_device == VK_NULL_HANDLE)
            picked_device = device;

        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);
        if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            picked_device = device;
    }

    if (picked_device != VK_NULL_HANDLE)
        return picked_device;

    VERIFY_NOT_REACHED();
}

static ErrorOr<VkDevice> create_logical_device(VkPhysicalDevice physical_device)
{
    VkDevice device;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

    int graphics_queue_family_index = -1;
    for (int i = 0; i < static_cast<int>(queue_families.size()); i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_index = i;
            break;
        }
    }

    VkDeviceQueueCreateInfo queue_create_info {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index;
    queue_create_info.queueCount = 1;

    float const queue_priority = 1.0f;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures deviceFeatures {};

    Array<char const*, 4> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };

    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;
    create_device_info.enabledExtensionCount = device_extensions.size();
    create_device_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device, &create_device_info, nullptr, &device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    return device;
}

ErrorOr<NonnullRefPtr<VulkanContext>> create_vulkan_context()
{
    uint32_t const api_version = VK_API_VERSION_1_0;
    auto* instance = TRY(create_instance(api_version));
    auto* physical_device = TRY(pick_physical_device(instance));
    auto* logical_device = TRY(create_logical_device(physical_device));

    VkQueue graphics_queue;
    vkGetDeviceQueue(logical_device, 0, 0, &graphics_queue);

    return make_ref_counted<VulkanContext>(
        api_version,
        instance,
        physical_device,
        logical_device,
        graphics_queue);
}

namespace Vulkan {

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    VERIFY_NOT_REACHED();
}

static ErrorOr<int> export_memory_to_dmabuf(VkDevice device, VkDeviceMemory memory)
{
    VkMemoryGetFdInfoKHR get_fd_info {};
    get_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get_fd_info.memory = memory;
    get_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    int dma_buf_fd = -1;
    PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    VERIFY(vkGetMemoryFdKHR);

    if (vkGetMemoryFdKHR(device, &get_fd_info, &dma_buf_fd) != VK_SUCCESS) {
        return Error::from_string_literal("Failed to export memory to dma_buf");
    }

    return dma_buf_fd;
}

ErrorOr<Image> create_image(VulkanContext& context, VkExtent2D extent, VkFormat format)
{
    dbgln("creating image with width {} and height {}", extent.width, extent.height);
    VkImageCreateInfo image_create_info {};
    image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = format;
    image_create_info.extent = { extent.width, extent.height, 1 };
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    ;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExternalMemoryImageCreateInfo external_memory_image_create_info {};
    external_memory_image_create_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_memory_image_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    image_create_info.pNext = &external_memory_image_create_info;

    VkImage image = VK_NULL_HANDLE;
    if (auto maybe_error = vkCreateImage(context.logical_device, &image_create_info, nullptr, &image); maybe_error != VK_SUCCESS) {
        dbgln("error: {}", (i32)maybe_error);
        return Error::from_string_literal("Image creation failed");
    }

    VkMemoryRequirements memory_requirements = {};
    vkGetImageMemoryRequirements(context.logical_device, image, &memory_requirements);

    VkMemoryAllocateInfo alloc_info {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = memory_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(context.physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkExportMemoryAllocateInfo export_memory_allocate_info {};
    export_memory_allocate_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_memory_allocate_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    alloc_info.pNext = &export_memory_allocate_info;

    VkDeviceMemory image_memory = {};
    if (auto maybe_error = vkAllocateMemory(context.logical_device, &alloc_info, nullptr, &image_memory); maybe_error != VK_SUCCESS) {
        dbgln("error: {}", (i32)maybe_error);
        vkDestroyImage(context.logical_device, image, nullptr);
        return Error::from_string_literal("Image memory allocation failed");
    }

    if (vkBindImageMemory(context.logical_device, image, image_memory, 0) != VK_SUCCESS) {
        vkFreeMemory(context.logical_device, image_memory, nullptr);
        vkDestroyImage(context.logical_device, image, nullptr);
        return Error::from_string_literal("Image memory binding failed");
    }

    auto exported_fd = TRY(export_memory_to_dmabuf(context.logical_device, image_memory));

    auto image_create_info_copy = image_create_info;
    image_create_info_copy.pNext = nullptr;

    return Image {
        .device = context.logical_device,
        .image = image,
        .memory = image_memory,
        .alloc_size = memory_requirements.size,
        .create_info = image_create_info_copy,
        .exported_fd = exported_fd
    };
}

bool format_with_drm_modifier_can_be_used_as_color_render_target(VulkanContext& context, VkFormat format, u64 drm_format_modifier)
{
    VkFormatProperties2 format_properties {};
    format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;

    VkDrmFormatModifierPropertiesListEXT modifier_properties {};
    modifier_properties.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    modifier_properties.drmFormatModifierCount = 0;
    modifier_properties.pDrmFormatModifierProperties = nullptr;
    modifier_properties.pNext = nullptr;
    format_properties.pNext = &modifier_properties;

    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_properties);

    auto modifier_properties_buffer = MUST(ByteBuffer::create_zeroed(modifier_properties.drmFormatModifierCount * sizeof(VkDrmFormatModifierPropertiesEXT)));
    auto* modifier_properties_buffer_pointer = reinterpret_cast<VkDrmFormatModifierPropertiesEXT*>(modifier_properties_buffer.data());

    modifier_properties.pDrmFormatModifierProperties = modifier_properties_buffer_pointer;

    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_properties);

    for (size_t modifier_properties_index = 0; modifier_properties_index < modifier_properties.drmFormatModifierCount; ++modifier_properties_index) {
        auto& modifier_properties_at_index = modifier_properties_buffer_pointer[modifier_properties_index];
        if (modifier_properties_at_index.drmFormatModifier == drm_format_modifier) {
            auto tiling_features = modifier_properties_at_index.drmFormatModifierTilingFeatures;
            return (tiling_features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0
                && (tiling_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0
                && (tiling_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        }
    }

    return false;
}

}

}
