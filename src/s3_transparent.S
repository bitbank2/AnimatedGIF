//
// SPDX-FileCopyrightText: 2026 Larry Bank <bitbank@pobox.com>
// SPDX-License-Identifier: Apache-2.0
//
// ESP32S3 SIMD optimized transparent pixel merging
// This asm code quickly combines a row of decoded 8-bit GIF pixels
// with an existing row of pixels and skips the transparent pixels.
// This is simple to do with both scalar and vector instructions.
// The challenge with the ESP32-S3 is that when using SIMD instructions
// reading and writing to memory must be on 16-byte aligned addresses.
//
// Copyright 2026 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#if defined (ARDUINO_ARCH_ESP32) && !defined(NO_SIMD)
#if __has_include ("dsps_fft2r_platform.h")
#include "dsps_fft2r_platform.h"
#if (dsps_fft2r_sc16_aes3_enabled == 1)
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
//
//                                       A2            A3            A4                          A5          A6               A7
// Call as void s3_merge_transparent(uint8_t *pSrc, uint8_t *pDest, uint8_t *pTransparent, int iLen, uint32_t *pPalette, uint16_t *pRGB565);
  s3_merge_transparent:
    # no idea what this frequency keyword does
#    .frequency 1.000 0.000
    .align 4
    entry    a1,16
  movi.n  a8,15             # mask to test address alignment
  beqz.n a4, .no_trans0
  ee.vldbc.8 q3,a4          # broadcast transparent color to all 16 slots
 .no_trans0:
  and a9,a3,a8              # test destination pointer alignment
  bnez.n a9,.unaligned_dest
  srli a9,a5,4            # number of 16-byte blocks to process
  loopnez a9, .big_chunk_loop
  ee.vld.128.ip    q0,a2,16   # load 16 source pixels
  beqz.n a4, .no_trans1
  ee.vld.128.ip    q1,a3,0    # load 16 destination pixels
  ee.vcmp.eq.s8 q2,q0,q3      # compare source pixels with transparent color
  ee.andq q4,q2,q1            # keep destination pixels using the mask
  ee.notq q2,q2               # invert the mask
  ee.andq q0,q2,q0            # keep source opaque pixels
  ee.orq q0,q0,q4             # combine src+dst pixels
 .no_trans1:
  ee.vst.128.ip q0,a3,16      # store 16 finished 8-bit pixels
  ee.xorq q1,q1,q1            # a register of 0's for zipping
  ee.vzip.8 q0,q1             # stretch 8bit->16bit data
# first 8 pixels
  ee.ldxq.32 q4,q0,a6,0,0     # load Q4:0 with first pixel's color
  ee.ldxq.32 q4,q0,a6,1,1     # load Q4:1 with second pixel's color
  ee.ldxq.32 q4,q0,a6,2,2
  ee.ldxq.32 q4,q0,a6,3,3
  ee.ldxq.32 q5,q0,a6,0,4
  ee.ldxq.32 q5,q0,a6,1,5
  ee.ldxq.32 q5,q0,a6,2,6
  ee.ldxq.32 q5,q0,a6,3,7
  ee.vunzip.16 q4,q5          # fold 32-bit values to 16-bits
  ee.vst.128.ip q4,a7,16      # write 8 x RGB565 pixels
# second 8 pixels
  ee.ldxq.32 q4,q1,a6,0,0
  ee.ldxq.32 q4,q1,a6,1,1
  ee.ldxq.32 q4,q1,a6,2,2
  ee.ldxq.32 q4,q1,a6,3,3
  ee.ldxq.32 q5,q1,a6,0,4
  ee.ldxq.32 q5,q1,a6,1,5
  ee.ldxq.32 q5,q1,a6,2,6
  ee.ldxq.32 q5,q1,a6,3,7
  ee.vunzip.16 q4,q5          # fold 32-bit values to 16-bits
  ee.vst.128.ip q4,a7,16      # write 8 x RGB565 pixels
  .big_chunk_loop:
  and a7,a5,a8                # leftover count
  bnez.n a7,.leftover_aligned 
  retw.n                      # length is a multiple of 16
.leftover_aligned:
// Read and process 16 pixels, but store the actual length requested
  ee.vld.128.ip    q1,a3,0    # load 16 destination pixels
.leftover_unaligned:
  ee.vld.128.ip    q0,a2,16   # load 16 source pixels
  beqz.n a4,.no_trans2
  ee.vcmp.eq.s8 q2,q0,q3      # compare source pixels with transparent color
  ee.andq q4,q2,q1            # keep destination pixels using the mask
  ee.notq q2,q2               # invert the mask
  ee.andq q0,q2,q0            # keep source opaque pixels
  ee.orq q0,q4,q0             # combine src+dst pixels
 .no_trans2:
  mv.qr q2,q0                 # keep a copy of merged pixels for palette use
  movi.n a2,12
  and a9,a5,a2                # loop over groups of 4 bytes
  beqz.n a9,.last3_aligned
  srli a9,a9,2                # number of groups of 4 bytes
  loopnez a9, .last3_aligned
  ee.movi.32.a q0,a2,0        # move the first 32-bit slot into a6
  s32i a2,a3,0                # store 4 bytes
  addi.n a3,a3,4
  ee.srci.2q q1,q0,3          # shift q0 right 4 bytes
 .last3_aligned:
  ee.movi.32.a q0,a2,0        # move the first 32-bit slot into a6
  movi.n a4,3
  and a9,a5,a4
  loopnez a9, .last3_bytes
  s8i a2,a3,0                 # store low byte
  addi.n a3,a3,1
  srli a2,a2,8                # shift down the next byte
 .last3_bytes:
# Convert and store the last 0-15 RGB565 pixels
# Prepare 16 pixels since it's easier to not compare/branch the count
  ee.xorq q3,q3,q3            # a register of 0's for zipping
  ee.vzip.8 q2,q3             # stretch 8bit->16bit data
# first 8 pixels
  ee.ldxq.32 q0,q2,a6,0,0
  ee.ldxq.32 q0,q2,a6,1,1
  ee.ldxq.32 q0,q2,a6,2,2
  ee.ldxq.32 q0,q2,a6,3,3
  ee.ldxq.32 q1,q2,a6,0,4
  ee.ldxq.32 q1,q2,a6,1,5
  ee.ldxq.32 q1,q2,a6,2,6
  ee.ldxq.32 q1,q2,a6,3,7
  ee.vunzip.16 q0,q1            # fold 32-bit values to 16-bits
# second 8 pixels
  ee.ldxq.32 q1,q3,a6,0,0
  ee.ldxq.32 q1,q3,a6,1,1
  ee.ldxq.32 q1,q3,a6,2,2
  ee.ldxq.32 q1,q3,a6,3,3
  ee.ldxq.32 q2,q3,a6,0,4
  ee.ldxq.32 q2,q3,a6,1,5
  ee.ldxq.32 q2,q3,a6,2,6
  ee.ldxq.32 q2,q3,a6,3,7
  ee.vunzip.16 q1,q2            # fold 32-bit values to 16-bits
  movi.n a4,15
  and a2,a5,a4                # Remaining pixel count (0-15)
  loopnez a2, .last_palette
  ee.movi.32.a q0,a6,0        # move the first 32-bit slot into a6
  s16i a6,a7,0                # store 2 bytes
  addi.n a7,a7,2
  ee.srci.2q q1,q0,1          # shift q0+q1 right 2 bytes
.last_palette:
  retw.n
# The destination 8-bit pointer is not aligned on a 16-byte address
# We need to go through all sorts of pain to make it work
.unaligned_dest:
  mov a9,a3                   # keep original destination pointer
  ee.ld.128.usar.ip q1,a3,16  # pre-load 32 bytes and SAR_BYTE with address inc
  ee.vld.128.ip q2,a3,16      # second half for shifting
  srli a10,a5,4               # number of 16-byte blocks to process
  loopnez a10, .big_chunk_unaligned
  ee.vld.128.ip    q0,a2,16   # load 16 source pixels
  ee.src.q q4,q1,q2           # shift unaligned dest bytes into position
  beqz.n a4,.no_trans3 
  ee.vcmp.eq.s8 q6,q0,q3      # compare source pixels with transparent color
  ee.andq q5,q4,q6            # keep destination pixels using the mask
  ee.notq q6,q6               # invert the mask
  ee.andq q0,q6,q0            # keep source opaque pixels
  ee.orq q0,q0,q5             # combine src+dst pixels
# We need to store the result 4-bytes at a time since the non-vector write
# instruction allows unaligned addresses
 .no_trans3:
  ee.movi.32.a q0,a8,0        # move the first 32-bit slot into a8
  s32i a8,a9,0                # store first 4 finished pixels
  ee.movi.32.a q0,a8,1        # second word
  s32i a8,a9,4
  ee.movi.32.a q0,a8,2        # third word
  s32i a8,a9,8
  ee.movi.32.a q0,a8,3        # fourth word
  s32i a8,a9,12
  addi.n a9,a9,16             # advance dest address by 16 bytes
  mv.qr q1,q2                 # prepare for next set of 16
  ee.vld.128.ip q2,a3,16      # read next 16 unaligned bytes
# prepare and store these 16 pixels as RGB565
  ee.xorq q5,q5,q5            # a register of 0's for zipping
  mv.qr q4,q0
  ee.vzip.8 q4,q5             # stretch 8bit->16bit data
# first 8 pixels
  ee.ldxq.32 q6,q4,a6,0,0     # load Q6:0 with first pixel's color
  ee.ldxq.32 q6,q4,a6,1,1     # load Q6:1 with second pixel's color
  ee.ldxq.32 q6,q4,a6,2,2
  ee.ldxq.32 q6,q4,a6,3,3
  ee.ldxq.32 q7,q4,a6,0,4
  ee.ldxq.32 q7,q4,a6,1,5
  ee.ldxq.32 q7,q4,a6,2,6
  ee.ldxq.32 q7,q4,a6,3,7
  ee.vunzip.16 q6,q7          # fold 32-bit values to 16-bits
  ee.vst.128.ip q6,a7,16      # write 8 x RGB565 pixels
# second 8 pixels
  ee.ldxq.32 q6,q5,a6,0,0     # load Q6:0 with first pixel's color
  ee.ldxq.32 q6,q5,a6,1,1     # load Q6:1 with second pixel's color
  ee.ldxq.32 q6,q5,a6,2,2
  ee.ldxq.32 q6,q5,a6,3,3
  ee.ldxq.32 q7,q5,a6,0,4
  ee.ldxq.32 q7,q5,a6,1,5
  ee.ldxq.32 q7,q5,a6,2,6
  ee.ldxq.32 q7,q5,a6,3,7
  ee.vunzip.16 q6,q7          # fold 32-bit values to 16-bits
  ee.vst.128.ip q6,a7,16      # write 8 x RGB565 pixels
  .big_chunk_unaligned:
  ee.src.q q1,q1,q2           # prepare last set of (0-15) dest pixels
  mov a3,a9                   # put correct destination address back in A3
  movi.n a8,15
  and a9,a5,a8                # leftover count
  bnez.n a9,.leftover_unaligned
  retw.n
#endif // dsps_fft2r_sc16_aes3_enabled
#endif // __has_include
#endif // ESP32
