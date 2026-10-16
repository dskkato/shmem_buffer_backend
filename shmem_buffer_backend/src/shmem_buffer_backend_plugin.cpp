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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <atomic>
#include <stdexcept>
#include <typeinfo>

#include "shmem_buffer/shmem_buffer.hpp"
#include "shmem_buffer/shmem_buffer_impl.hpp"
#include "shmem_buffer/shmem_buffer_ipc_manager.hpp"
#include "shmem_buffer/shmem_buffer_memory_pool.hpp"
#include "shmem_buffer_backend/shmem_buffer_backend.hpp"
#include "shmem_buffer_backend_msgs/msg/shmem_buffer_descriptor.hpp"
#include "pluginlib/class_list_macros.hpp"
#include "rosidl_buffer/buffer_impl_base.hpp"
#include "rosidl_typesupport_cpp/message_type_support.hpp"

namespace shmem_buffer
{

ShmemBufferBackend::ShmemBufferBackend() = default;

std::shared_ptr<HostEndpointManager> ShmemBufferBackend::get_endpoint_manager() const
{
  std::lock_guard<std::mutex> lock(manager_mutex_);
  if (endpoint_manager_ == nullptr) {
    endpoint_manager_ = std::make_shared<HostEndpointManager>();
  }
  return endpoint_manager_;
}

std::string ShmemBufferBackend::get_backend_metadata() const
{
  return get_endpoint_manager()->metadata();
}

const rosidl_message_type_support_t * ShmemBufferBackend::get_descriptor_type_support() const
{
  return rosidl_typesupport_cpp::get_message_type_support_handle<
    shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor>();
}

std::shared_ptr<void> ShmemBufferBackend::create_empty_descriptor() const
{
  return std::make_shared<shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor>();
}

void ShmemBufferBackend::on_creating_endpoint(
  const rmw_topic_endpoint_info_t & endpoint_info) const
{
  (void)endpoint_info;
}

std::pair<bool, std::vector<std::set<std::uint32_t>>> ShmemBufferBackend::on_discovering_endpoint(
  const rmw_topic_endpoint_info_t & endpoint_info,
  const std::vector<rmw_topic_endpoint_info_t> & existing_endpoints,
  const std::unordered_map<std::string, std::string> & endpoint_supported_backends)
{
  (void)existing_endpoints;

  const auto it = endpoint_supported_backends.find("shmem_buffer");
  const bool is_compatible =
    it != endpoint_supported_backends.end() && it->second == get_backend_metadata();
  {
    std::lock_guard<std::mutex> lock(compatibility_mutex_);
    compatibility_cache_[GidKey(endpoint_info.endpoint_gid)] = is_compatible;
  }
  return {is_compatible, {}};
}

std::shared_ptr<void> ShmemBufferBackend::create_descriptor_with_endpoint(
  const void * impl, const rmw_topic_endpoint_info_t & endpoint_info) const
{
  {
    std::lock_guard<std::mutex> lock(compatibility_mutex_);
    const auto it = compatibility_cache_.find(GidKey(endpoint_info.endpoint_gid));
    if (it != compatibility_cache_.end() && !it->second) {
      return nullptr;
    }
  }
  if (impl == nullptr) {
    return nullptr;
  }
  const auto * base = static_cast<const rosidl::BufferImplBase<std::uint8_t> *>(impl);
  const auto * shmem_buffer_impl = dynamic_cast<const ShmemBufferImpl<std::uint8_t> *>(base);
  if (shmem_buffer_impl == nullptr) {
    return nullptr;
  }

  try {
    const auto pool = get_global_shmem_buffer_pool();
    auto & shmem_buffer = shmem_buffer_impl->get_shmem_buffer();
    shmem_buffer.finalize_write_handle();

    // An imported buffer is a read-only view of a block owned by an upstream
    // process. It is not present in this process's publisher pool, so looking
    // it up with find_block_for_ptr() would force an unnecessary CPU fallback.
    // Reuse the original descriptor identity instead. The imported buffer's
    // reader reference keeps the generation alive while this node handles the
    // message, and the UID check prevents forwarding a stale generation.
    if (shmem_buffer.has_ipc_descriptor()) {
      auto * control = shmem_buffer.control();
      if (
        control == nullptr || control->magic != kShmemBufferControlMagic ||
        control->abi_version != kShmemBufferControlAbiVersion ||
        control->payload_size != shmem_buffer.size() ||
        control->ipc_uid.load(std::memory_order_acquire) != shmem_buffer.ipc_uid())
      {
        return nullptr;
      }

      // Re-publishing an imported buffer extends the source block's reuse
      // grace period, but must not create a new generation or alter its UID.
      pool->refresh_publish_timestamp(control);

      auto descriptor =
        std::make_shared<shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor>();
      descriptor->size = shmem_buffer.size();
      descriptor->element_type_name = typeid(std::uint8_t).name();
      descriptor->shmem_buffer_pid = shmem_buffer.ipc_pid();
      descriptor->shmem_buffer_block_id = shmem_buffer.block_id();
      descriptor->shmem_buffer_block_size = shmem_buffer.mapped_size();
      descriptor->shmem_buffer_socket_path = shmem_buffer.ipc_name();
      descriptor->ipc_uid = shmem_buffer.ipc_uid();
      return descriptor;
    }

    ShmemBufferBlock * block = pool->find_block_for_ptr(shmem_buffer.get_ptr());
    if (block == nullptr || block->control == nullptr) {
      return nullptr;
    }
    const std::string ipc_name = pool->register_block_for_ipc(block);
    const std::uint64_t uid = pool->assign_uid(block);
    if (ipc_name.empty() || uid == 0) {
      return nullptr;
    }

    auto descriptor = std::make_shared<shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor>();
    descriptor->size = shmem_buffer_impl->size();
    descriptor->element_type_name = typeid(std::uint8_t).name();
#ifdef _WIN32
    descriptor->shmem_buffer_pid = static_cast<std::int32_t>(GetCurrentProcessId());
#else
    descriptor->shmem_buffer_pid = static_cast<std::int32_t>(getpid());
#endif
    descriptor->shmem_buffer_block_id = block->block_id;
    descriptor->shmem_buffer_block_size = block->mapped_size;
    descriptor->shmem_buffer_socket_path = ipc_name;
    descriptor->ipc_uid = uid;
    pool->mark_published(block);
    return descriptor;
  } catch (const std::exception &) {
    return nullptr;
  }
}

std::unique_ptr<void, void (*)(void *)> ShmemBufferBackend::from_descriptor_with_endpoint(
  const void * descriptor_ptr, const rmw_topic_endpoint_info_t & endpoint_info) const
{
  (void)endpoint_info;
  if (descriptor_ptr == nullptr) {
    throw std::runtime_error("null shared-memory descriptor");
  }
  const auto & descriptor =
    *static_cast<const shmem_buffer_backend_msgs::msg::ShmemBufferDescriptor *>(descriptor_ptr);
  if (descriptor.element_type_name != typeid(std::uint8_t).name()) {
    throw std::runtime_error("shared-memory descriptor element type mismatch");
  }
  if (
    descriptor.shmem_buffer_pid <= 0 || descriptor.shmem_buffer_socket_path.empty() ||
    descriptor.ipc_uid == 0 ||
    descriptor.shmem_buffer_block_size < kShmemBufferPayloadOffset ||
    descriptor.size > descriptor.shmem_buffer_block_size - kShmemBufferPayloadOffset)
  {
    throw std::runtime_error("invalid shared-memory descriptor metadata");
  }

  auto imported = ShmemBufferHandleCache::import_block(
    descriptor.shmem_buffer_socket_path, descriptor.shmem_buffer_pid,
      descriptor.shmem_buffer_block_id,
    descriptor.shmem_buffer_block_size, descriptor.size, descriptor.ipc_uid);
  imported->acquire_reader();
  if (imported->control()->ipc_uid.load(std::memory_order_acquire) != descriptor.ipc_uid) {
    imported->release_reader();
    throw std::runtime_error("stale shared-memory descriptor raced with block reuse");
  }

  auto reader_release = [imported](std::uint8_t *) {imported->release_reader();};
  ShmemBuffer buffer(
    imported->payload(), descriptor.size, std::move(reader_release), imported->control(), imported,
    descriptor.shmem_buffer_block_id, descriptor.shmem_buffer_block_size, false,
    descriptor.shmem_buffer_pid, descriptor.shmem_buffer_socket_path, descriptor.ipc_uid);
  auto result = std::make_unique<ShmemBufferImpl<std::uint8_t>>(
    std::move(buffer), static_cast<std::size_t>(descriptor.size));
  return {result.release(), [](void * ptr) {
      delete static_cast<rosidl::BufferImplBase<std::uint8_t> *>(ptr);
    }};
}

}  // namespace shmem_buffer

PLUGINLIB_EXPORT_CLASS(shmem_buffer::ShmemBufferBackend, rosidl::BufferBackend)
