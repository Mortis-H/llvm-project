// Minimal AMDGPU kernel fragment for amdgpu-isaparser
.text
.p2align 8
.globl simple_kernel
simple_kernel:
  s_mov_b32 s0, s1
  s_endpgm
