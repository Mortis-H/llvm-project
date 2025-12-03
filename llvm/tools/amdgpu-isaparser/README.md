# amdgpu-isaparser quickstart

This tool uses LLVM's MC AMDGPU assembler to parse assembly and annotate
instructions, directives, and labels with prefixes.

## Building
1. Configure a small build that only enables the AMDGPU backend:
   ```
   cmake -S llvm -B build-amdgpu -G Ninja \
     -DLLVM_TARGETS_TO_BUILD=AMDGPU \
     -DCMAKE_BUILD_TYPE=Release
   ```
2. Build the tool:
   ```
   ninja -C build-amdgpu amdgpu-isaparser
   ```

## Running a simple smoke test
A tiny AMDGPU kernel sample is provided in `sample.s`. After building:
```
./build-amdgpu/bin/amdgpu-isaparser -mcpu=gfx950 llvm/tools/amdgpu-isaparser/sample.s
```
The output prefixes the leading comment with the selected triple/CPU and
annotates each directive, label, and instruction as it is parsed.

For a more realistic HIP-style code object, `hipcc_sample.s` includes symbol
attributes, section switches, and data directives similar to what `hipcc -S`
produces. Parsing it exercises section and data handling:
```
./build-amdgpu/bin/amdgpu-isaparser -mcpu=gfx950 llvm/tools/amdgpu-isaparser/hipcc_sample.s
```
