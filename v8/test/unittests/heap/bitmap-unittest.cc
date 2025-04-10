// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/common/globals.h"
#include "src/heap/marking-inl.h"
#include "src/heap/spaces.h"
#include "test/unittests/heap/bitmap-test-utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace v8::internal {

constexpr uint32_t kMarkedCell = 0xFFFFFFFF;
constexpr uint32_t kHalfMarkedCell = 0xFFFF0000;
constexpr uint32_t kWhiteCell = 0x00000000;
constexpr uint8_t kMarkedByte = 0xFF;
constexpr uint8_t kUnmarkedByte = 0x00;

using NonAtomicBitmapTest = TestWithBitmap;
using AtomicBitmapTest = TestWithBitmap;

TEST_F(NonAtomicBitmapTest, IsZeroInitialized) {
  // We require all tests to start from a zero-initialized bitmap. Manually
  // verify this invariant here.
  for (size_t i = 0; i < MarkingBitmap::kSize; i++) {
    EXPECT_EQ(raw_bitmap()[i], kUnmarkedByte);
  }
}

TEST_F(NonAtomicBitmapTest, Cells) {
  auto bm = bitmap();
  bm->cells()[1] = kMarkedCell;
  uint8_t* raw = raw_bitmap();
  int second_cell_base = MarkingBitmap::kBytesPerCell;
  for (size_t i = 0; i < MarkingBitmap::kBytesPerCell; i++) {
    EXPECT_EQ(raw[second_cell_base + i], kMarkedByte);
  }
}

TEST_F(NonAtomicBitmapTest, CellsCount) {
  size_t last_cell_index = MarkingBitmap::kCellsCount - 1;
  bitmap()->cells()[last_cell_index] = kMarkedCell;
  // Manually verify on raw memory.
  uint8_t* raw = raw_bitmap();
  for (size_t i = 0; i < MarkingBitmap::kSize; i++) {
    // Last cell should be set.
    if (i >= (MarkingBitmap::kSize - MarkingBitmap::kBytesPerCell)) {
      EXPECT_EQ(raw[i], kMarkedByte);
    } else {
      EXPECT_EQ(raw[i], kUnmarkedByte);
    }
  }
}

TEST_F(NonAtomicBitmapTest, IsClean) {
  auto bm = bitmap();
  EXPECT_TRUE(bm->IsClean());
  bm->cells()[0] = kMarkedCell;
  EXPECT_FALSE(bm->IsClean());
}

namespace {

template <AccessMode access_mode>
void ClearTest(uint8_t* raw_bitmap, MarkingBitmap* bm) {
  for (size_t i = 0; i < MarkingBitmap::kSize; i++) {
    raw_bitmap[i] = 0xFFu;
  }
  bm->Clear<AccessMode::ATOMIC>();
  for (size_t i = 0; i < MarkingBitmap::kSize; i++) {
    EXPECT_EQ(raw_bitmap[i], 0);
  }
}

template <AccessMode access_mode>
void ClearRange1Test(uint8_t* raw_bitmap, MarkingBitmap* bm) {
  bm->cells()[0] = kMarkedCell;
  bm->cells()[1] = kMarkedCell;
  bm->cells()[2] = kMarkedCell;
  bm->ClearRange<access_mode>(
      0, MarkingBitmap::kBitsPerCell + MarkingBitmap::kBitsPerCell / 2);
  EXPECT_EQ(bm->cells()[0], kWhiteCell);
  EXPECT_EQ(bm->cells()[1], kHalfMarkedCell);
  EXPECT_EQ(bm->cells()[2], kMarkedCell);
}

template <AccessMode access_mode>
void ClearRange2Test(uint8_t* raw_bitmap, MarkingBitmap* bm) {
  bm->cells()[0] = kMarkedCell;
  bm->cells()[1] = kMarkedCell;
  bm->cells()[2] = kMarkedCell;
  bm->ClearRange<access_mode>(
      MarkingBitmap::kBitsPerCell,
      MarkingBitmap::kBitsPerCell + MarkingBitmap::kBitsPerCell / 2);
  EXPECT_EQ(bm->cells()[0], kMarkedCell);
  EXPECT_EQ(bm->cells()[1], kHalfMarkedCell);
  EXPECT_EQ(bm->cells()[2], kMarkedCell);
}

template <AccessMode access_mode>
void SetAndClearRangeTest(uint8_t* raw_bitmap, MarkingBitmap* bm) {
  for (int i = 0; i < 3; i++) {
    bm->SetRange<access_mode>(i, MarkingBitmap::kBitsPerCell + i);
    CHECK_EQ(bm->cells()[0], 0xFFFFFFFFu << i);
    CHECK_EQ(bm->cells()[1], (1u << i) - 1);
    bm->ClearRange<access_mode>(i, MarkingBitmap::kBitsPerCell + i);
    CHECK_EQ(bm->cells()[0], 0x0u);
    CHECK_EQ(bm->cells()[1], 0x0u);
  }
}

}  // namespace

TEST_F(AtomicBitmapTest, Clear) {
  ClearTest<AccessMode::ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(NonAtomicBitmapTest, Clear) {
  ClearTest<AccessMode::NON_ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(AtomicBitmapTest, ClearRange1) {
  ClearRange1Test<AccessMode::ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(NonAtomicBitmapTest, ClearRange1) {
  ClearRange1Test<AccessMode::NON_ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(AtomicBitmapTest, ClearRange2) {
  ClearRange2Test<AccessMode::ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(NonAtomicBitmapTest, ClearRange2) {
  ClearRange2Test<AccessMode::NON_ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(AtomicBitmapTest, SetAndClearRange) {
  SetAndClearRangeTest<AccessMode::ATOMIC>(this->raw_bitmap(), this->bitmap());
}

TEST_F(NonAtomicBitmapTest, SetAndClearRange) {
  SetAndClearRangeTest<AccessMode::NON_ATOMIC>(this->raw_bitmap(),
                                               this->bitmap());
}

// AllBitsSetInRange() and AllBitsClearInRange() are only used when verifying
// the heap on the main thread so they don't have atomic implementations.
TEST_F(NonAtomicBitmapTest, ClearMultipleRanges) {
  auto bm = this->bitmap();

  bm->SetRange<AccessMode::NON_ATOMIC>(0, MarkingBitmap::kBitsPerCell * 3);
  CHECK(bm->AllBitsSetInRange(0, MarkingBitmap::kBitsPerCell));

  bm->ClearRange<AccessMode::NON_ATOMIC>(MarkingBitmap::kBitsPerCell / 2,
                                         MarkingBitmap::kBitsPerCell);
  bm->ClearRange<AccessMode::NON_ATOMIC>(
      MarkingBitmap::kBitsPerCell,
      MarkingBitmap::kBitsPerCell + MarkingBitmap::kBitsPerCell / 2);
  bm->ClearRange<AccessMode::NON_ATOMIC>(MarkingBitmap::kBitsPerCell * 2 + 8,
                                         MarkingBitmap::kBitsPerCell * 2 + 16);
  bm->ClearRange<AccessMode::NON_ATOMIC>(MarkingBitmap::kBitsPerCell * 2 + 24,
                                         MarkingBitmap::kBitsPerCell * 3);

  CHECK_EQ(bm->cells()[0], 0xFFFFu);
  CHECK(bm->AllBitsSetInRange(0, MarkingBitmap::kBitsPerCell / 2));
  CHECK(bm->AllBitsClearInRange(MarkingBitmap::kBitsPerCell / 2,
                                MarkingBitmap::kBitsPerCell));

  CHECK_EQ(bm->cells()[1], 0xFFFF0000u);
  CHECK(bm->AllBitsClearInRange(
      MarkingBitmap::kBitsPerCell,
      MarkingBitmap::kBitsPerCell + MarkingBitmap::kBitsPerCell / 2));
  CHECK(bm->AllBitsSetInRange(
      MarkingBitmap::kBitsPerCell + MarkingBitmap::kBitsPerCell / 2,
      MarkingBitmap::kBitsPerCell * 2));

  CHECK_EQ(bm->cells()[2], 0xFF00FFu);
  CHECK(bm->AllBitsSetInRange(
      MarkingBitmap::kBitsPerCell * 2,
      MarkingBitmap::kBitsPerCell * 2 + MarkingBitmap::kBitsPerCell / 4));
  CHECK(bm->AllBitsClearInRange(
      MarkingBitmap::kBitsPerCell * 2 + MarkingBitmap::kBitsPerCell / 4,
      MarkingBitmap::kBitsPerCell * 2 + MarkingBitmap::kBitsPerCell / 2));
  CHECK(bm->AllBitsSetInRange(
      MarkingBitmap::kBitsPerCell * 2 + MarkingBitmap::kBitsPerCell / 2,
      MarkingBitmap::kBitsPerCell * 2 + MarkingBitmap::kBitsPerCell / 2 +
          MarkingBitmap::kBitsPerCell / 4));
  CHECK(bm->AllBitsClearInRange(MarkingBitmap::kBitsPerCell * 2 +
                                    MarkingBitmap::kBitsPerCell / 2 +
                                    MarkingBitmap::kBitsPerCell / 4,
                                MarkingBitmap::kBitsPerCell * 3));
}

}  // namespace v8::internal
