// HIP-style AMDGPU assembly sample for amdgpu-isaparser
.text
.section .text.simple_kernel,"ax",@progbits
.p2align 8
.globl simple_kernel
.type simple_kernel,@function
simple_kernel:
  s_mov_b32 s0, s1
  s_waitcnt vmcnt(0)
  s_endpgm
.Lfunc_end0:
.size simple_kernel, .Lfunc_end0-simple_kernel

.section .rodata,"a",@progbits
.p2align 6
.rodata_label:
  .long 42
  .quad 0

.section .data,"aw",@progbits
.p2align 2
.global_buffer:
  .byte 1, 2, 3, 4
  .fill 4, 1, 0
