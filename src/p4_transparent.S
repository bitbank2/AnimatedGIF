//
// SPDX-FileCopyrightText: 2026 Larry Bank <bitbank@pobox.com>
// SPDX-License-Identifier: Apache-2.0
//
// ESP32P4 SIMD optimized transparent pixel merging
// This asm code quickly combines a row of decoded 8-bit GIF pixels
// with an existing row of pixels and skips the transparent pixels.
// This is simple to do with both scalar and vector instructions.
// The challenge with the ESP32-P4 is that when using SIMD instructions
// reading and writing to memory must be on 16-byte aligned addresses.
//
// Copyright 2026 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agresp. to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// Sesp.the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#if defined (ARDUINO_ESP32P4_DEV)
    .text
    .align 4

    .global s3_merge_transparent
    .type   s3_merge_transparent,@function
//
// Combine opaque 8-bit pixels, skipping transparent ones
// Convert the result through the palette into RGB565
// The source pointer is assumed to be 16-byte aligned
// The destination 8-bit pixel pointer can be unaligned
// If pTransparent is NULL, then there is no transparency
// The destination 16-bit output pointer must be 16-byte aligned
// a0-a7 parameters, t0-t6 temp registers, x0 = 0
//                                       A0            A1            A2                          A3          A4               A5
// Call as void s3_merge_transparent(uint8_t *pSrc, uint8_t *pDest, uint8_t *pTransparent, int iLen, uint32_t *pPalette, uint16_t *pRGB565);
  s3_merge_transparent:
  .align 4
  li  t0,15             # mask to test address alignment
  beq a2, x0, no_trans0
  esp.vldbc.8.ip q3,a2,0       # broadcast transparent color to all 16 slots
no_trans0:
  and t1,a1,t0              # test destination pointer alignment
  bne t1,x0, unaligned_dest
  srli t0,a3,4            # number of 16-byte blocks to process
big_chunk_loop:
  beq t0,x0, big_chunk_exit
  esp.vld.128.ip    q0,a0,16   # load 16 source pixels
  beq a2,x0, no_trans1
  esp.vld.128.ip    q1,a1,0    # load 16 destination pixels
  esp.vcmp.eq.s8 q2,q0,q3      # compare source pixels with transparent color
  esp.andq q4,q2,q1            # keep destination pixels using the mask
  esp.notq q2,q2               # invert the mask
  esp.andq q0,q2,q0            # keep source opaque pixels
  esp.orq q0,q0,q4             # combine src+dst pixels
 no_trans1:
  esp.vst.128.ip q0,a1,16      # store 16 finished 8-bit pixels
  esp.xorq q1,q1,q1            # a register of 0's for zipping
  esp.vzip.8 q0,q1             # stretch 8bit->16bit data
# first 8 pixels
  esp.ldxq.32 q4,q0,a4,0,0     # load Q4:0 with first pixel's color
  esp.ldxq.32 q4,q0,a4,1,1     # load Q4:1 with second pixel's color
  esp.ldxq.32 q4,q0,a4,2,2
  esp.ldxq.32 q4,q0,a4,3,3
  esp.ldxq.32 q5,q0,a4,0,4
  esp.ldxq.32 q5,q0,a4,1,5
  esp.ldxq.32 q5,q0,a4,2,6
  esp.ldxq.32 q5,q0,a4,3,7
  esp.vunzip.16 q4,q5          # fold 32-bit values to 16-bits
  esp.vst.128.ip q4,a5,16      # write 8 x RGB565 pixels
# second 8 pixels
  esp.ldxq.32 q4,q1,a4,0,0
  esp.ldxq.32 q4,q1,a4,1,1
  esp.ldxq.32 q4,q1,a4,2,2
  esp.ldxq.32 q4,q1,a4,3,3
  esp.ldxq.32 q5,q1,a4,0,4
  esp.ldxq.32 q5,q1,a4,1,5
  esp.ldxq.32 q5,q1,a4,2,6
  esp.ldxq.32 q5,q1,a4,3,7
  esp.vunzip.16 q4,q5          # fold 32-bit values to 16-bits
  esp.vst.128.ip q4,a5,16      # write 8 x RGB565 pixels
  addi t0,t0,-1
  j big_chunk_loop
 big_chunk_exit:
  andi x31,a3,15                # leftover count
  bne x31,x0,leftover_aligned 
  ret                      # length is a multiple of 16
leftover_aligned:
// Read and process 16 pixels, but store the actual length requested
  esp.vld.128.ip    q1,a3,0    # load 16 destination pixels
leftover_unaligned:
  esp.vld.128.ip    q0,a2,16   # load 16 source pixels
  beq x28,x31,no_trans2
  esp.vcmp.eq.s8 q2,q0,q3      # compare source pixels with transparent color
  esp.andq q4,q2,q1            # keep destination pixels using the mask
  esp.notq q2,q2               # invert the mask
  esp.andq q0,q2,q0            # keep source opaque pixels
  esp.orq q0,q4,q0             # combine src+dst pixels
 no_trans2:
  esp.orq q2,q0,q0             # keep a copy of merged pixels for palette use
  li t0,12
  and t1,a3,t0                 # loop over groups of 4 bytes
  srli t1,t1,2                 # number of groups of 4 bytes
top_last_12:
  beq t1,x0, last3_aligned
  esp.movi.32.a q0,x31,0        # move the first 32-bit slot into t0
  sw x31, 0(a1)                # store 4 bytes
  addi a1,a1,4
  esp.srci.2q q1,q0,3          # shift q0 right 4 bytes
  addi t1,t1,-1
  j top_last_12
 last3_aligned:
  esp.movi.32.a q0,x31,0        # move the first 32-bit slot into x31
  li t0,3
  and t1,a3,t0
last3_bytes:
  beq t1,x0,last3_end
  sb x31,0(a1)                  # store low byte
  addi a1,a1,1
  srli x31,x31,8                # shift down the next byte
  addi t1,t1,-1
  j last3_bytes
 last3_end:
# Convert and store the last 0-15 RGB565 pixels
# Prepare 16 pixels since it's easier to not compare/branch the count
  esp.xorq q3,q3,q3            # a register of 0's for zipping
  esp.vzip.8 q2,q3             # stretch 8bit->16bit data
# first 8 pixels
  esp.ldxq.32 q0,q2,a4,0,0
  esp.ldxq.32 q0,q2,a4,1,1
  esp.ldxq.32 q0,q2,a4,2,2
  esp.ldxq.32 q0,q2,a4,3,3
  esp.ldxq.32 q1,q2,a4,0,4
  esp.ldxq.32 q1,q2,a4,1,5
  esp.ldxq.32 q1,q2,a4,2,6
  esp.ldxq.32 q1,q2,a4,3,7
  esp.vunzip.16 q0,q1            # fold 32-bit values to 16-bits
# second 8 pixels
  esp.ldxq.32 q1,q3,a4,0,0
  esp.ldxq.32 q1,q3,a4,1,1
  esp.ldxq.32 q1,q3,a4,2,2
  esp.ldxq.32 q1,q3,a4,3,3
  esp.ldxq.32 q2,q3,a4,0,4
  esp.ldxq.32 q2,q3,a4,1,5
  esp.ldxq.32 q2,q3,a4,2,6
  esp.ldxq.32 q2,q3,a4,3,7
  esp.vunzip.16 q1,q2            # fold 32-bit values to 16-bits
  li t0,15
  and t1,a3,t0                # Remaining pixel count (0-15)
last_palette_top:
  beq t1,x0,last_palette_end
  esp.movi.32.a q0,x31,0        # move the first 32-bit slot into x31
  sh x31,0(a5)                  # store 2 bytes
  addi a5,a5,2
  esp.srci.2q q1,q0,1          # shift q0+q1 right 2 bytes
  addi t1,t1,-1
  j last_palette_top
 last_palette_end:
  ret
# The destination 8-bit pointer is not aligned on a 16-byte address
# We need to go through all sorts of pain to make it work
unaligned_dest:
  or t2,a1,a1                 # keep. original destination pointer
  esp.ld.128.usar.ip q1,a1,16  # pre-load 32 bytes and SAR_BYTE with address inc
  esp.vld.128.ip q2,a1,16      # second half for shifting
  srli t1,a3,4                 # number of 16-byte blocks to process
big_chunk_unaligned:
  beq t1,x0,big_chunk_unaligned_end
  esp.vld.128.ip    q0,a0,16   # load 16 source pixels
  esp.src.q q4,q1,q2           # shift unaligned dest bytes into position
  beq a2,x0, no_trans3 
  esp.vcmp.eq.s8 q6,q0,q3      # compare source pixels with transparent color
  esp.andq q5,q4,q6            # keep destination pixels using the mask
  esp.notq q6,q6               # invert the mask
  esp.andq q0,q6,q0            # keep source opaque pixels
  esp.orq q0,q0,q5             # combine src+dst pixels
# We need to store the result 4-bytes at a time since the non-vector write
# instruction allows unaligned addresses
 no_trans3:
  esp.movi.32.a q0,x31,0       # move the first 32-bit slot into x31
  sw x31,0(t2)                 # store first 4 finished pixels
  esp.movi.32.a q0,x31,1       # second word
  sw x31,4(t2)
  esp.movi.32.a q0,x31,2       # third word
  sw x31,8(t2)
  esp.movi.32.a q0,x31,3       # fourth word
  sw x31,12(t2)
  addi t2,t2,16                # advance dest address by 16 bytes
  esp.orq q1,q2,q2             # prepare for next set of 16
  esp.vld.128.ip q2,a1,16      # read next 16 unaligned bytes
# prepare and store these 16 pixels as RGB565
  esp.xorq q5,q5,q5            # a register of 0's for zipping
  esp.xorq q4,q0,q0
  esp.vzip.8 q4,q5             # stretch 8bit->16bit data
# first 8 pixels
  esp.ldxq.32 q6,q4,a4,0,0     # load Q6:0 with first pixel's color
  esp.ldxq.32 q6,q4,a4,1,1     # load Q6:1 with second pixel's color
  esp.ldxq.32 q6,q4,a4,2,2
  esp.ldxq.32 q6,q4,a4,3,3
  esp.ldxq.32 q7,q4,a4,0,4
  esp.ldxq.32 q7,q4,a4,1,5
  esp.ldxq.32 q7,q4,a4,2,6
  esp.ldxq.32 q7,q4,a4,3,7
  esp.vunzip.16 q6,q7          # fold 32-bit values to 16-bits
  esp.vst.128.ip q6,a5,16      # write 8 x RGB565 pixels
# second 8 pixels
  esp.ldxq.32 q6,q5,a4,0,0     # load Q6:0 with first pixel's color
  esp.ldxq.32 q6,q5,a4,1,1     # load Q6:1 with second pixel's color
  esp.ldxq.32 q6,q5,a4,2,2
  esp.ldxq.32 q6,q5,a4,3,3
  esp.ldxq.32 q7,q5,a4,0,4
  esp.ldxq.32 q7,q5,a4,1,5
  esp.ldxq.32 q7,q5,a4,2,6
  esp.ldxq.32 q7,q5,a4,3,7
  esp.vunzip.16 q6,q7          # fold 32-bit values to 16-bits
  esp.vst.128.ip q6,a5,16      # write 8 x RGB565 pixels
  addi t1,t1,-1
  j big_chunk_unaligned
 big_chunk_unaligned_end:
  esp.src.q q1,q1,q2           # prepare last set of (0-15) dest pixels
  or a1,t2,t2                  # put correct destination address back in A1
  li t0,15
  and t1,a3,t0                # leftover count
  bne t1,x0, leftover_unaligned
  ret
#endif // ESP32P4
