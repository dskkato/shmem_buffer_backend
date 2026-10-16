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

#include "shmem_buffer/shmem_buffer_memory_pool.hpp"

#include <limits>
#include <new>
#include <stdexcept>

#include "shmem_buffer/shmem_buffer_ipc_manager.hpp"
#include "shmem_buffer/shmem_buffer_platform.hpp"

namespace shmem_buffer
{

namespace
{

std::uint64_t monotonic_time_us()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
         .count());
}

}  // namespace

ShmemBufferMemoryPool::ShmemBufferMemoryPool()
: broker_(std::make_unique<ShmemBufferFdBroker>()) {}

ShmemBufferMemoryPool::~ShmemBufferMemoryPool()
{
  // The broker owns duplicated descriptor references and socket listeners.
  // Stop unregistering those before closing the publisher descriptors.
  broker_.reset();

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto & block : all_blocks_) {
    if (block->control != nullptr) {
      block->control->~ShmemBufferControlHeader();
      block->control = nullptr;
    }
    destroy_platform_mapping(block->native_handle, block->mapping, block->mapped_size);
    block->native_handle = -1;
    block->mapping = nullptr;
  }
}

ShmemBufferBlock * ShmemBufferMemoryPool::allocate(std::size_t payload_size)
{
  if (payload_size == 0) {
    throw std::invalid_argument("shared-memory buffer pool cannot allocate a zero-size block");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto bucket = free_blocks_.find(payload_size);
  ShmemBufferBlock * block = nullptr;
  if (bucket != free_blocks_.end()) {
    auto & candidates = bucket->second;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (try_claim_block(candidates[i])) {
        block = candidates[i];
        candidates[i] = candidates.back();
        candidates.pop_back();
        if (candidates.empty()) {
          free_blocks_.erase(bucket);
        }
        break;
      }
    }
  }

  if (block == nullptr) {
    block = create_block(payload_size);
  }

  block->in_use = true;
  block->current_uid = uid_dist_(uid_rng_);
  if (block->current_uid == 0) {
    block->current_uid = 1;
  }
  block->control->ipc_uid.store(0, std::memory_order_release);
  block->control->publish_timestamp_us.store(0, std::memory_order_release);
  release_shmem_buffer_reuse_claim(block->control);
  return block;
}

void ShmemBufferMemoryPool::free(ShmemBufferBlock * block)
{
  if (block == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!block->in_use) {
    return;
  }
  block->in_use = false;
  free_blocks_[block->payload_size].push_back(block);
}

std::function<void(std::uint8_t *)> ShmemBufferMemoryPool::deleter(ShmemBufferBlock * block)
{
  std::shared_ptr<ShmemBufferMemoryPool> self;
  try {
    self = shared_from_this();
  } catch (const std::bad_weak_ptr &) {
    throw std::runtime_error("ShmemBufferMemoryPool must be owned by shared_ptr");
  }
  return [self, block](std::uint8_t *) {self->free(block);};
}

std::uint64_t ShmemBufferMemoryPool::assign_uid(ShmemBufferBlock * block)
{
  if (block == nullptr || block->control == nullptr) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (block->current_uid == 0) {
    block->current_uid = uid_dist_(uid_rng_);
    if (block->current_uid == 0) {
      block->current_uid = 1;
    }
  }
  return block->current_uid;
}

void ShmemBufferMemoryPool::mark_published(ShmemBufferBlock * block)
{
  if (block == nullptr || block->control == nullptr) {
    return;
  }
  block->control->ipc_uid.store(block->current_uid, std::memory_order_release);
  refresh_publish_timestamp(block->control);
}

void ShmemBufferMemoryPool::refresh_publish_timestamp(ShmemBufferControlHeader * control)
{
  if (control == nullptr) {
    return;
  }
  control->publish_timestamp_us.store(monotonic_time_us(), std::memory_order_release);
}

std::string ShmemBufferMemoryPool::register_block_for_ipc(ShmemBufferBlock * block)
{
  if (block == nullptr || broker_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return broker_->register_block(block);
}

ShmemBufferBlock * ShmemBufferMemoryPool::find_block_for_ptr(const void * ptr) const
{
  if (ptr == nullptr) {
    return nullptr;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(ptr);
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & block : all_blocks_) {
    const auto start = reinterpret_cast<std::uintptr_t>(block->mapping) +
      kShmemBufferPayloadOffset;
    const auto end = reinterpret_cast<std::uintptr_t>(block->mapping) + block->mapped_size;
    if (address >= start && address < end) {
      return block.get();
    }
  }
  return nullptr;
}

bool ShmemBufferMemoryPool::try_claim_block(ShmemBufferBlock * block) const
{
  if (block == nullptr || block->control == nullptr) {
    return false;
  }
  const auto published = block->control->publish_timestamp_us.load(std::memory_order_acquire);
  if (published != 0) {
    const auto now = monotonic_time_us();
    if (now < published || (now - published) < kShmemBufferGracePeriodUs) {
      return false;
    }
  }
  return try_claim_shmem_buffer_reuse(block->control);
}

ShmemBufferBlock * ShmemBufferMemoryPool::create_block(std::size_t payload_size)
{
  if (payload_size > std::numeric_limits<std::size_t>::max() - kShmemBufferPayloadOffset) {
    throw std::length_error("shared-memory buffer mapping size overflows size_t");
  }
  const std::size_t mapped_size = kShmemBufferPayloadOffset + payload_size;
  const std::uint32_t block_id = next_block_id_++;
  const auto platform_mapping = create_platform_mapping(mapped_size, block_id, uid_dist_(uid_rng_));

  try {
    auto block = std::make_unique<ShmemBufferBlock>();
    block->native_handle = platform_mapping.native_handle;
    block->mapping = platform_mapping.mapping;
    block->mapped_size = platform_mapping.mapped_size;
    block->payload_size = payload_size;
    block->block_id = block_id;
    block->socket_path = platform_mapping.ipc_name;
    block->control = new (platform_mapping.mapping) ShmemBufferControlHeader();
    block->control->payload_size = payload_size;
    block->control->ipc_uid.store(0, std::memory_order_relaxed);
    block->control->reader_state.store(0, std::memory_order_relaxed);
    block->control->publish_timestamp_us.store(0, std::memory_order_relaxed);

    ShmemBufferBlock * result = block.get();
    all_blocks_.push_back(std::move(block));
    initialized_ = true;
    ipc_capable_ = true;
    return result;
  } catch (...) {
    destroy_platform_mapping(
      platform_mapping.native_handle, platform_mapping.mapping, platform_mapping.mapped_size);
    throw;
  }
}

}  // namespace shmem_buffer
