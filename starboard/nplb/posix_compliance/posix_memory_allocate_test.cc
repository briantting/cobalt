// Copyright 2024 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>

#include "testing/gtest/include/gtest/gtest.h"

namespace starboard {
namespace nplb {
namespace {

const size_t kSize = 1024 * 128;

TEST(PosixSbMemoryAllocateTest, AllocatesNormally) {
  void* memory = malloc(kSize);
  EXPECT_NE(static_cast<void*>(NULL), memory);

  free(memory);
}

TEST(PosixSbMemoryAllocateTest, AllocatesZero) {
  void* memory = malloc(0);

  // We can't expect anything here because some implementations may return an
  // allocated zero-size memory block, and some implementations may return NULL.
  free(memory);
}

TEST(PosixSbMemoryAllocateTest, AllocatesOne) {
  void* memory = malloc(1);
  EXPECT_NE(static_cast<void*>(NULL), memory);
  free(memory);
}

TEST(PosixMemoryAllocateTest, CanReadWriteToResult) {
  void* memory = malloc(kSize);
  ASSERT_NE(static_cast<void*>(NULL), memory);
  char* data = static_cast<char*>(memory);
  for (size_t i = 0; i < kSize; ++i) {
    data[i] = static_cast<char>(i);
  }

  for (size_t i = 0; i < kSize; ++i) {
    EXPECT_EQ(data[i], static_cast<char>(i));
  }

  free(memory);
}

}  // namespace
}  // namespace nplb
}  // namespace starboard
