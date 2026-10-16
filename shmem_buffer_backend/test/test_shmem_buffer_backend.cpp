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
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>

#include "shmem_buffer/shmem_buffer_api.hpp"
#include "shmem_buffer/shmem_buffer_impl.hpp"
#include "shmem_buffer_backend/shmem_buffer_backend.hpp"
#include "shmem_buffer_backend_msgs/msg/shmem_buffer_descriptor.hpp"

namespace
{

rmw_topic_endpoint_info_t endpoint()
{
  rmw_topic_endpoint_info_t result{};
  result.topic_type = "test_msgs/msg/Bytes";
  return result;
}

TEST(ShmemBufferBackendTest, MetadataRequiresSamePlatformLocality)
{
  shmem_buffer::ShmemBufferBackend backend;
  const auto info = endpoint();
  std::unordered_map<std::string, std::string> supported;
  supported["shmem"] = backend.get_backend_metadata();
  EXPECT_TRUE(backend.on_discovering_endpoint(info, {}, supported).first);

  supported["shmem"] = "different-host:4294967295";
  EXPECT_FALSE(backend.on_discovering_endpoint(info, {}, supported).first);
}

TEST(ShmemBufferBackendTest, DescriptorRoundTripUsesPlatformIpcAndMappingCache)
{
  shmem_buffer::ShmemBufferBackend backend;
  const auto info = endpoint();
  std::unordered_map<std::string, std::string> supported;
  supported["shmem"] = backend.get_backend_metadata();
  ASSERT_TRUE(backend.on_discovering_endpoint(info, {}, supported).first);

  auto buffer = shmem_buffer::allocate_buffer(128);
  {
    auto write = shmem_buffer::from_output_buffer(buffer);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      write.get_ptr()[i] = static_cast<std::uint8_t>(i ^ 0x5A);
    }
  }

  auto descriptor = backend.create_descriptor_with_endpoint(buffer.get_impl(), info);
  ASSERT_NE(nullptr, descriptor);
  const auto * typed_descriptor =
    static_cast<const shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor *>(descriptor.get());
  ASSERT_EQ(128u, typed_descriptor->size);
  ASSERT_NE(0u, typed_descriptor->ipc_uid);
#ifdef _WIN32
  EXPECT_EQ(0u,
      typed_descriptor->shmem_buffer_socket_path.rfind("Local\\rosidl_shmem_buffer_", 0));
#else
  EXPECT_EQ(0u, typed_descriptor->shmem_buffer_socket_path.rfind("/tmp/shmem_buffer_", 0));
#endif
  EXPECT_EQ(
    shmem_buffer::kShmemBufferPayloadOffset + typed_descriptor->size,
    typed_descriptor->shmem_buffer_block_size);

  auto imported = backend.from_descriptor_with_endpoint(descriptor.get(), info);
  ASSERT_NE(nullptr, imported.get());
  auto * imported_base = static_cast<rosidl::BufferImplBase<std::uint8_t> *>(imported.get());
  auto * imported_impl =
    dynamic_cast<shmem_buffer::ShmemBufferImpl<std::uint8_t> *>(imported_base);
  ASSERT_NE(nullptr, imported_impl);
  EXPECT_FALSE(imported_impl->get_shmem_buffer().writable());
  EXPECT_THROW(imported_impl->get_shmem_buffer().get_write_handle(), std::runtime_error);
  auto first_copy = imported_impl->to_cpu();
  auto * first_cpu = dynamic_cast<rosidl::CpuBufferImpl<std::uint8_t> *>(first_copy.get());
  ASSERT_NE(nullptr, first_cpu);
  EXPECT_EQ(0x5A, first_cpu->get_storage()[0]);
  EXPECT_EQ(static_cast<std::uint8_t>(127 ^ 0x5A), first_cpu->get_storage()[127]);

  // A second import has the same physical key and therefore reuses the cache
  // entry without asking the broker for another descriptor mapping.
  auto imported_again = backend.from_descriptor_with_endpoint(descriptor.get(), info);
  ASSERT_NE(nullptr, imported_again.get());
  auto * imported_again_base =
    static_cast<rosidl::BufferImplBase<std::uint8_t> *>(imported_again.get());
  auto * imported_again_impl =
    dynamic_cast<shmem_buffer::ShmemBufferImpl<std::uint8_t> *>(imported_again_base);
  ASSERT_NE(nullptr, imported_again_impl);
  auto second_copy = imported_again_impl->to_cpu();
  auto * second_cpu = dynamic_cast<rosidl::CpuBufferImpl<std::uint8_t> *>(second_copy.get());
  ASSERT_NE(nullptr, second_cpu);
  EXPECT_EQ(first_cpu->get_storage(), second_cpu->get_storage());

  auto stale_descriptor = *typed_descriptor;
  ++stale_descriptor.ipc_uid;
  EXPECT_THROW(
    backend.from_descriptor_with_endpoint(&stale_descriptor, info), std::runtime_error);
}

TEST(ShmemBufferBackendTest, ImportedBufferCanBeForwardedWithoutCpuCopy)
{
  shmem_buffer::ShmemBufferBackend backend;
  const auto info = endpoint();
  std::unordered_map<std::string, std::string> supported;
  supported["shmem"] = backend.get_backend_metadata();
  ASSERT_TRUE(backend.on_discovering_endpoint(info, {}, supported).first);

  auto source = shmem_buffer::allocate_buffer(32);
  {
    auto write = shmem_buffer::from_output_buffer(source);
    for (std::size_t i = 0; i < source.size(); ++i) {
      write.get_ptr()[i] = static_cast<std::uint8_t>(0xC0u + i);
    }
  }

  auto source_descriptor = backend.create_descriptor_with_endpoint(source.get_impl(), info);
  ASSERT_NE(nullptr, source_descriptor);
  const auto * source_typed =
    static_cast<const shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor *>(
    source_descriptor.get());

  auto imported = backend.from_descriptor_with_endpoint(source_descriptor.get(), info);
  auto * imported_impl = dynamic_cast<shmem_buffer::ShmemBufferImpl<std::uint8_t> *>(
    static_cast<rosidl::BufferImplBase<std::uint8_t> *>(imported.get()));
  ASSERT_NE(nullptr, imported_impl);
  auto * control = imported_impl->get_shmem_buffer().control();
  ASSERT_NE(nullptr, control);
  EXPECT_EQ(1u, control->reader_state.load(std::memory_order_acquire));
  const auto published_before =
    control->publish_timestamp_us.load(std::memory_order_acquire);
  ASSERT_NE(0u, published_before);

  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto forwarded = backend.create_descriptor_with_endpoint(imported_impl, info);
  ASSERT_NE(nullptr, forwarded);
  const auto * forwarded_typed =
    static_cast<const shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor *>(
    forwarded.get());
  EXPECT_EQ(source_typed->shmem_buffer_pid, forwarded_typed->shmem_buffer_pid);
  EXPECT_EQ(source_typed->shmem_buffer_block_id, forwarded_typed->shmem_buffer_block_id);
  EXPECT_EQ(source_typed->shmem_buffer_block_size, forwarded_typed->shmem_buffer_block_size);
  EXPECT_EQ(source_typed->shmem_buffer_socket_path, forwarded_typed->shmem_buffer_socket_path);
  EXPECT_EQ(source_typed->ipc_uid, forwarded_typed->ipc_uid);
  EXPECT_GT(
    control->publish_timestamp_us.load(std::memory_order_acquire), published_before);

  // A second import models the downstream subscriber of the forwarding node.
  // The first imported buffer is still alive, so the upstream generation
  // cannot be claimed for reuse while the forwarded descriptor is consumed.
  auto downstream = backend.from_descriptor_with_endpoint(forwarded.get(), info);
  auto * downstream_impl =
    dynamic_cast<shmem_buffer::ShmemBufferImpl<std::uint8_t> *>(
    static_cast<rosidl::BufferImplBase<std::uint8_t> *>(downstream.get()));
  ASSERT_NE(nullptr, downstream_impl);
  EXPECT_EQ(2u, control->reader_state.load(std::memory_order_acquire));
  auto copy = downstream_impl->to_cpu();
  auto * cpu = dynamic_cast<rosidl::CpuBufferImpl<std::uint8_t> *>(copy.get());
  ASSERT_NE(nullptr, cpu);
  ASSERT_EQ(32u, cpu->get_storage().size());
  EXPECT_EQ(0xC0u, cpu->get_storage().front());
  EXPECT_EQ(0xDFu, cpu->get_storage().back());

  downstream.reset();
  EXPECT_EQ(1u, control->reader_state.load(std::memory_order_acquire));
}

TEST(ShmemBufferBackendTest, IncompatiblePeerFallsBack)
{
  shmem_buffer::ShmemBufferBackend backend;
  const auto info = endpoint();
  std::unordered_map<std::string, std::string> supported;
  supported["shmem"] = "other-host:1000";
  EXPECT_FALSE(backend.on_discovering_endpoint(info, {}, supported).first);

  auto buffer = shmem_buffer::allocate_buffer(8);
  EXPECT_EQ(nullptr, backend.create_descriptor_with_endpoint(buffer.get_impl(), info));
}

}  // namespace
