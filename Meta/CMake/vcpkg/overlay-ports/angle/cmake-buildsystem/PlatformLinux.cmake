list(APPEND ANGLE_DEFINITIONS ANGLE_PLATFORM_LINUX USE_SYSTEM_EGL)
include(linux.cmake)

if (USE_OPENGL)
    # Enable GLSL compiler output.
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_GLSL)
endif ()

# Vulkan backend
if(USE_VULKAN)
    list(APPEND ANGLE_SOURCES
        ${vulkan_backend_sources}

        ${angle_translator_lib_spirv_sources}

        ${angle_translator_glsl_base_sources}
        ${angle_translator_glsl_and_vulkan_base_sources}

        ${angle_spirv_sources}
    )

    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_VULKAN
    )
endif()

# OpenGL backend
if (USE_OPENGL OR ENABLE_WEBGL)
    list(APPEND ANGLE_SOURCES
        ${_gl_backend_sources}

        ${angle_system_utils_sources_linux}
        ${angle_system_utils_sources_posix}

        ${angle_dma_buf_sources}

        ${libangle_gl_egl_dl_sources}
        ${libangle_gl_egl_sources}
        ${libangle_gl_sources}

        ${libangle_gpu_info_util_sources}
        ${libangle_gpu_info_util_linux_sources}
    )

    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_OPENGL
    )

endif ()
