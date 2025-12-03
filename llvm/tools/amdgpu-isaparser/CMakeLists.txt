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

add_llvm_tool(amdgpu-isaparser
  amdgpu-isaparser.cpp
)
