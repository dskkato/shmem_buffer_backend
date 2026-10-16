// Copyright 2026 Daisuke Kato
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

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "shmem_buffer/shmem_buffer_api.hpp"
#include "shmem_buffer/shmem_buffer_impl.hpp"
#include "shmem_buffer/shmem_buffer_memory_pool.hpp"

namespace
{

TEST(ShmemBufferTest, ReuseClaimSynchronizesReaderAcquisition)
{
  shmem_buffer::ShmemBufferControlHeader control;

  EXPECT_TRUE(shmem_buffer::try_acquire_shmem_buffer_reader(&control));
  EXPECT_FALSE(shmem_buffer::try_claim_shmem_buffer_reuse(&control));
  shmem_buffer::release_shmem_buffer_reader(&control);

  EXPECT_TRUE(shmem_buffer::try_claim_shmem_buffer_reuse(&control));
  EXPECT_EQ(
    shmem_buffer::kShmemBufferReuseClaimed, control.reader_state.load(std::memory_order_acquire));
  EXPECT_FALSE(shmem_buffer::try_acquire_shmem_buffer_reader(&control));

  control.ipc_uid.store(42, std::memory_order_release);
  shmem_buffer::release_shmem_buffer_reuse_claim(&control);
  EXPECT_TRUE(shmem_buffer::try_acquire_shmem_buffer_reader(&control));
  EXPECT_EQ(1u, control.reader_state.load(std::memory_order_acquire));
  shmem_buffer::release_shmem_buffer_reader(&control);
  EXPECT_EQ(0u, control.reader_state.load(std::memory_order_acquire));
}

TEST(ShmemBufferTest, ControlHeaderDescribesFixedMappingLayout)
{
  auto pool = std::make_shared<shmem_buffer::ShmemBufferMemoryPool>();
  auto * block = pool->allocate(32);
  ASSERT_NE(nullptr, block);
  ASSERT_NE(nullptr, block->control);

  EXPECT_EQ(0u, block->control->ipc_uid.load(std::memory_order_acquire));
  EXPECT_EQ(shmem_buffer::kShmemBufferControlMagic, block->control->magic);
  EXPECT_EQ(shmem_buffer::kShmemBufferControlAbiVersion, block->control->abi_version);
  EXPECT_EQ(32u, block->control->payload_size);
  EXPECT_EQ(shmem_buffer::kShmemBufferPayloadOffset + 32u, block->mapped_size);
  EXPECT_EQ(0u, reinterpret_cast<std::uintptr_t>(block->mapping) % 64u);
  EXPECT_EQ(
    0u,
    (reinterpret_cast<std::uintptr_t>(block->mapping) + shmem_buffer::kShmemBufferPayloadOffset) %
      64u);

  pool->free(block);
}

TEST(ShmemBufferTest, AllocateWriteReadAndCpuCopy)
{
  auto buffer = shmem_buffer::allocate_buffer(64);
  {
    auto write = shmem_buffer::from_output_buffer(buffer);
    ASSERT_NE(nullptr, write.get_ptr());
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      write.get_ptr()[i] = static_cast<std::uint8_t>(i + 7);
    }
  }

  const auto & const_buffer = buffer;
  auto read = shmem_buffer::from_input_buffer(const_buffer);
  ASSERT_NE(nullptr, read.get_ptr());
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    EXPECT_EQ(static_cast<std::uint8_t>(i + 7), read.get_ptr()[i]);
  }
  auto cpu = buffer.to_vector();
  ASSERT_EQ(64u, cpu.size());
  EXPECT_EQ(7u, cpu.front());
  EXPECT_EQ(70u, cpu.back());
}

TEST(ShmemBufferTest, PromotesCpuInputBuffer)
{
  rosidl::Buffer<std::uint8_t> cpu(8);
  for (std::size_t i = 0; i < cpu.size(); ++i) {
    cpu[i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  const auto & const_cpu = cpu;
  auto read = shmem_buffer::from_input_buffer(const_cpu);
  ASSERT_NE(nullptr, read.get_promoted_buffer());
  EXPECT_EQ("shmem", read.get_promoted_buffer()->get_backend_type());
  EXPECT_EQ(0xA0, read.get_ptr()[0]);
  EXPECT_EQ(0xA7, read.get_ptr()[7]);
}

TEST(ShmemBufferTest, RejectsNonShmemBufferOutputBuffer)
{
  rosidl::Buffer<std::uint8_t> cpu(8);
  EXPECT_THROW(shmem_buffer::from_output_buffer(cpu), shmem_buffer::ShmemBufferError);
}

TEST(ShmemBufferTest, RejectsConcurrentAndFinalizedWriters)
{
  auto buffer = shmem_buffer::allocate_buffer(8);
  auto first = shmem_buffer::from_output_buffer(buffer);
  EXPECT_THROW(shmem_buffer::from_output_buffer(buffer), std::runtime_error);
  first = shmem_buffer::WriteHandle();
  EXPECT_THROW(shmem_buffer::from_output_buffer(buffer), std::runtime_error);
}

TEST(ShmemBufferTest, ReadHandleDoesNotImplicitlyFinalizeWriter)
{
  auto buffer = shmem_buffer::allocate_buffer(8);
  auto write = shmem_buffer::from_output_buffer(buffer);
  const auto & const_buffer = buffer;

  EXPECT_THROW(shmem_buffer::from_input_buffer(const_buffer), std::runtime_error);
}

TEST(ShmemBufferTest, WriterAndReaderAccessAreMutuallyExclusive)
{
  auto buffer = shmem_buffer::allocate_buffer(8);
  const auto & const_buffer = buffer;
  auto read = shmem_buffer::from_input_buffer(const_buffer);

  EXPECT_THROW(shmem_buffer::from_output_buffer(buffer), std::runtime_error);

  read = shmem_buffer::ReadHandle();
  EXPECT_NO_THROW(shmem_buffer::from_output_buffer(buffer));
}

TEST(ShmemBufferTest, MultipleReadHandlesAreAllowed)
{
  auto buffer = shmem_buffer::allocate_buffer(8);
  const auto & const_buffer = buffer;
  auto first = shmem_buffer::from_input_buffer(const_buffer);
  auto second = shmem_buffer::from_input_buffer(const_buffer);

  EXPECT_THROW(shmem_buffer::from_output_buffer(buffer), std::runtime_error);

  first = shmem_buffer::ReadHandle();
  EXPECT_THROW(shmem_buffer::from_output_buffer(buffer), std::runtime_error);

  second = shmem_buffer::ReadHandle();
  EXPECT_NO_THROW(shmem_buffer::from_output_buffer(buffer));
}

TEST(ShmemBufferTest, ExplicitFinalizeAllowsReadWhileWriteHandleLives)
{
  auto buffer = shmem_buffer::allocate_buffer(8);
  auto write = shmem_buffer::from_output_buffer(buffer);
  write.get_ptr()[0] = 0x5A;

  auto * impl =
    dynamic_cast<shmem_buffer::ShmemBufferImpl<std::uint8_t> *>(buffer.get_impl());
  ASSERT_NE(nullptr, impl);
  impl->get_shmem_buffer().finalize_write_handle();

  const auto & const_buffer = buffer;
  auto read = shmem_buffer::from_input_buffer(const_buffer);
  EXPECT_EQ(0x5A, read.get_ptr()[0]);
}

TEST(ShmemBufferTest, RejectsWritersForReadOnlyBuffer)
{
  std::uint8_t payload[8]{};
  shmem_buffer::ShmemBuffer buffer(
    payload, sizeof(payload), [](std::uint8_t *) {}, nullptr, nullptr, 0, 0, false);

  EXPECT_FALSE(buffer.writable());
  EXPECT_THROW(buffer.get_write_handle(), std::runtime_error);
}

TEST(ShmemBufferTest, PoolUsesSizeBucketsAndReaderProtection)
{
  auto pool = std::make_shared<shmem_buffer::ShmemBufferMemoryPool>();
  auto * first = pool->allocate(32);
  auto * different = pool->allocate(64);
  ASSERT_NE(first, different);
  pool->free(first);
  pool->free(different);

  auto * reused = pool->allocate(32);
  EXPECT_EQ(first, reused);
  pool->free(reused);
}

#ifdef _WIN32
TEST(ShmemBufferTest, NamedMappingIsRemovedAfterLastHandleCloses)
{
  std::string mapping_name;
  {
    auto pool = std::make_shared<shmem_buffer::ShmemBufferMemoryPool>();
    auto * block = pool->allocate(32);
    ASSERT_NE(nullptr, block);
    ASSERT_FALSE(block->socket_path.empty());
    mapping_name = block->socket_path;
    const std::wstring wide_name(mapping_name.begin(), mapping_name.end());
    HANDLE probe = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide_name.c_str());
    ASSERT_NE(nullptr, probe);
    EXPECT_NE(FALSE, CloseHandle(probe));
  }

  const std::wstring wide_name(mapping_name.begin(), mapping_name.end());
  HANDLE stale = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide_name.c_str());
  EXPECT_EQ(nullptr, stale);
  if (stale != nullptr) {
    (void)CloseHandle(stale);
  }
}
#endif

}  // namespace
