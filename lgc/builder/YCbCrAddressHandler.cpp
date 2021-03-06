/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2016-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  YCbCrAddressHandler.cpp
* @brief LLPC source file: Implementation of LLPC class YCbCrAddressHandler
***********************************************************************************************************************
*/
#include "YCbCrAddressHandler.h"
#include "lgc/util/GfxRegHandler.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Generate base address for image planes
// Note: If input planeCount == 1, it'll generate the base address for plane 0. This function accepts the concept of
// planeCount rather a specific plane for that the calulation of plane[n+1] is always based on plane[n].
//
// @param planeCount : The plane counts
void YCbCrAddressHandler::genBaseAddress(unsigned planeCount) {
  // For YCbCr, the possible plane counts are among 1 and 3.
  assert(planeCount > 0 && planeCount < 4);

  // PlaneBaseAddresses[0] is the same as original base addressed as passed in SRD
  m_planeBaseAddresses.push_back(m_regHandler->getReg(SqRsrcRegs::BaseAddress));

  if (planeCount > 1) {
    // PlaneBaseAddresses[1] = PlaneBaseAddresses[0] + addr256B(PitchY * HeightY)
    m_planeBaseAddresses.push_back(
        m_builder->CreateAdd(m_planeBaseAddresses[0],
                             m_builder->CreateLShr(m_builder->CreateMul(m_pitchY, m_heightY), m_builder->getInt32(8))));
    if (planeCount > 2) {
      // PlaneBaseAddresses[2] = PlaneBaseAddresses[1] + addr256B(PitchCb * HeightCb)
      m_planeBaseAddresses.push_back(m_builder->CreateAdd(
          m_planeBaseAddresses[1],
          m_builder->CreateLShr(m_builder->CreateMul(m_pitchCb, m_heightCb), m_builder->getInt32(8))));
    }
  }
}

// =====================================================================================================================
// Power2Align operation
//
// @param x : Value needs to be aligned
// @param align : Align base
Value *YCbCrAddressHandler::power2Align(Value *x, unsigned align) {
  // Check if align is a power of 2
  assert(align != 0 && (align & (align - 1)) == 0);

  Value *result = m_builder->CreateAdd(x, m_builder->getInt32(align - 1));
  return m_builder->CreateAnd(result, m_builder->getInt32(~(align - 1)));
}

// =====================================================================================================================
// Calculate height and pitch
//
// @param bits : Channel bits
// @param bpp : Bits per pixel
// @param xBitCount : Effective channel bits
// @param isTileOptimal : Is tiling optimal
// @param planeNum : Number of planes
void YCbCrAddressHandler::genHeightAndPitch(unsigned bits, unsigned bpp, unsigned xBitCount, bool isTileOptimal,
                                            unsigned planeNum) {
  switch (m_gfxIp->major) {
  case 9: {
    // Height = SqRsrcRegs::Height
    Value *height = m_regHandler->getReg(SqRsrcRegs::Height);
    // HeightHalf = Height * 0.5
    Value *heightHalf = m_builder->CreateLShr(height, m_one);

    m_heightY = height;
    m_heightCb = heightHalf;

    // Pitch = SqRsrcRegs::Pitch
    Value *pitch = m_regHandler->getReg(SqRsrcRegs::Pitch);
    // PitchHalf = Pitch * 0.5
    Value *pitchHalf = m_builder->CreateLShr(pitch, m_one);

    // PitchY * (xBitCount >> 3)
    m_pitchY = m_builder->CreateMul(pitch, m_builder->CreateLShr(m_builder->getInt32(xBitCount), 3));

    // PitchCb = PitchCb * (xBitCount >> 3)
    m_pitchCb = m_builder->CreateMul(pitchHalf, m_builder->CreateLShr(m_builder->getInt32(xBitCount), 3));

    if (isTileOptimal) {
      Value *isTileOpt = m_regHandler->getReg(SqRsrcRegs::IsTileOpt);

      // PtchYOpt = PitchY * (bits[0] >> 3)
      Value *ptchYOpt = m_builder->CreateMul(pitch, m_builder->CreateLShr(m_builder->getInt32(bits), 3));
      // PitchY = IsTileOpt ? (PtchYOpt << 5) : PitchY
      m_pitchY = m_builder->CreateSelect(isTileOpt, m_builder->CreateShl(ptchYOpt, m_builder->getInt32(5)), m_pitchY);

      // PitchCbOpt = PitchCb * (bits[0] >> 3)
      Value *pitchCbOpt = m_builder->CreateMul(pitchHalf, m_builder->CreateLShr(m_builder->getInt32(bits), 3));
      // PitchCb = IsTileOpt ? (PitchCbOpt << 5) : PitchCb
      m_pitchCb =
          m_builder->CreateSelect(isTileOpt, m_builder->CreateShl(pitchCbOpt, m_builder->getInt32(5)), m_pitchCb);
    }
    break;
  }
  case 10: {
    const unsigned elementBytes = bpp >> 3;
    const unsigned pitchAlign = (256 / elementBytes);

    // Height = SqRsrcRegs::Height
    Value *height = m_regHandler->getReg(SqRsrcRegs::Height);
    m_heightY = height;

    // Width = SqRsrcRegs::Width
    Value *width = m_regHandler->getReg(SqRsrcRegs::Width);

    m_pitchY = power2Align(width, pitchAlign);
    // PitchY = PitchY * ElementBytes
    m_pitchY = m_builder->CreateMul(m_pitchY, m_builder->getInt32(elementBytes));

    // HeightHalf = Height * 0.5
    Value *heightHalf = m_builder->CreateLShr(height, m_one);
    m_heightCb = heightHalf;

    // WidthHalf = Width * 0.5
    Value *widthHalf = m_builder->CreateLShr(width, m_one);

    m_pitchCb = power2Align(widthHalf, pitchAlign);
    // PitchCb = PitchCb * ElementBytes
    m_pitchCb = m_builder->CreateMul(m_pitchCb, m_builder->getInt32(elementBytes));

    if (isTileOptimal) {
      const unsigned log2BlkSize = 16;
      const unsigned log2EleBytes = log2(bpp >> 3);
      const unsigned log2NumEle = log2BlkSize - log2EleBytes;
      const bool widthPrecedent = 1;
      const unsigned log2Width = (log2NumEle + (widthPrecedent ? 1 : 0)) / 2;
      const unsigned pitchAlignOpt = 1u << log2Width;
      const unsigned heightAlignOpt = 1u << (log2NumEle - log2Width);

      // PitchY = PitchY * ElementBytes
      Value *ptchYOpt = m_builder->CreateMul(power2Align(width, pitchAlignOpt), m_builder->getInt32(elementBytes));

      // PitchCb = PitchCb * ElementBytes
      Value *pitchCbOpt =
          m_builder->CreateMul(power2Align(widthHalf, pitchAlignOpt), m_builder->getInt32(elementBytes));

      Value *isTileOpt = m_regHandler->getReg(SqRsrcRegs::IsTileOpt);
      m_pitchY = m_builder->CreateSelect(isTileOpt, ptchYOpt, m_pitchY);
      m_heightY = m_builder->CreateSelect(isTileOpt, power2Align(height, heightAlignOpt), height);

      m_pitchCb = m_builder->CreateSelect(isTileOpt, pitchCbOpt, m_pitchCb);
      m_heightCb = m_builder->CreateSelect(isTileOpt, power2Align(heightHalf, heightAlignOpt), heightHalf);
    }
    break;
  }
  default:
    llvm_unreachable("GFX IP not supported!");
    break;
  }
}
