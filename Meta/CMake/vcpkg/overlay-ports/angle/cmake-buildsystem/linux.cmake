if(is_android OR is_linux OR is_chromeos)
  set(angle_dma_buf_sources
    "src/common/linux/dma_buf_utils.cpp"
    "src/common/linux/dma_buf_utils.h"
  )

  set(angle_spirv_sources
    "src/common/spirv/angle_spirv_utils.cpp"
    "src/common/spirv/spirv_instruction_builder_autogen.cpp"
    "src/common/spirv/spirv_instruction_builder_autogen.h"
    "src/common/spirv/spirv_instruction_parser_autogen.cpp"
    "src/common/spirv/spirv_instruction_parser_autogen.h"
    "src/common/spirv/spirv_types.h"
  )

  set(angle_vk_mem_alloc_wrapper
    "src/libANGLE/renderer/vulkan/vk_mem_alloc_wrapper.cpp"
    "src/libANGLE/renderer/vulkan/vk_mem_alloc_wrapper.h"
  )
endif()
