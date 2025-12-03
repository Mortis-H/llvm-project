set(LLVM_LINK_COMPONENTS
  AMDGPUAsmParser
  AMDGPUDesc
  AMDGPUInfo
  AMDGPUUtils
  AsmParser
  MC
  MCParser
  Support
)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_llvm_tool(amdgpu-isaparser
  amdgpu-isaparser.cpp
)
