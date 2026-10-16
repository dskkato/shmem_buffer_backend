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

#include "shmem_buffer/shmem_buffer_ipc_manager.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "shmem_buffer/shmem_buffer_platform.hpp"

namespace shmem_buffer
{

namespace
{

struct CacheKey
{
  std::int32_t pid;
  std::uint32_t block_id;
  std::string ipc_name;

  bool operator==(const CacheKey & other) const
  {
    return pid == other.pid && block_id == other.block_id && ipc_name == other.ipc_name;
  }
};

struct CacheKeyHash
{
  std::size_t operator()(const CacheKey & key) const
  {
    std::size_t result = std::hash<std::int32_t>{}(key.pid);
    result ^= std::hash<std::uint32_t>{}(key.block_id) + 0x9e3779b9 +
    (result << 6) + (result >> 2);
    result ^= std::hash<std::string>{}(key.ipc_name) + 0x9e3779b9 +
    (result << 6) + (result >> 2);
    return result;
  }
};

using CacheMap = std::unordered_map<CacheKey, std::shared_ptr<SharedBufferImportedBlock>,
    CacheKeyHash>;

CacheMap & cache()
{
  static auto * value = new CacheMap();
  return *value;
}

std::mutex & cache_mutex()
{
  static auto * value = new std::mutex();
  return *value;
}

void validate_mapping(
  const SharedBufferImportedBlock & block, std::uint64_t mapped_size, std::uint64_t payload_size,
  std::uint64_t expected_uid)
{
  const SharedBufferControlHeader * control = block.control();
  if (
    control == nullptr || mapped_size != block.mapped_size() ||
    mapped_size < kSharedBufferPayloadOffset ||
    payload_size > mapped_size - kSharedBufferPayloadOffset)
  {
    throw std::runtime_error("invalid shared memory mapping size");
  }
  if (control->magic != kSharedBufferControlMagic ||
    control->abi_version != kSharedBufferControlAbiVersion)
  {
    throw std::runtime_error("shared memory control header ABI mismatch");
  }
  if (control->payload_size != payload_size) {
    throw std::runtime_error("shared memory control header payload size mismatch");
  }
  if (expected_uid == 0) {
    throw std::runtime_error("shared memory descriptor contains a zero publication UID");
  }
  if (control->ipc_uid.load(std::memory_order_acquire) != expected_uid) {
    throw std::runtime_error("stale shared memory descriptor publication UID");
  }
}

}  // namespace

SharedBufferImportedBlock::SharedBufferImportedBlock(
  std::intptr_t native_handle, void * mapping, std::size_t mapped_size, std::string ipc_name)
: native_handle_(native_handle),
  mapping_(mapping),
  mapped_size_(mapped_size),
  control_(static_cast<SharedBufferControlHeader *>(mapping)),
  ipc_name_(std::move(ipc_name))
{
}

SharedBufferImportedBlock::~SharedBufferImportedBlock()
{
  destroy_platform_mapping(native_handle_, mapping_, mapped_size_);
}

std::uint8_t * SharedBufferImportedBlock::payload() const
{
  return reinterpret_cast<std::uint8_t *>(mapping_) + kSharedBufferPayloadOffset;
}

void SharedBufferImportedBlock::acquire_reader()
{
  if (control_ == nullptr) {
    throw std::runtime_error("cannot acquire a reader for unmapped shared memory");
  }
  if (!try_acquire_shmem_buffer_reader(control_)) {
    throw std::runtime_error("shared memory block is being reused");
  }
}

void SharedBufferImportedBlock::release_reader() noexcept
{
  if (control_ != nullptr) {
    release_shmem_buffer_reader(control_);
  }
}

void SharedBufferHandleCache::validate(
  const SharedBufferImportedBlock & block, std::uint64_t mapped_size, std::uint64_t payload_size,
  std::uint64_t expected_uid)
{
  validate_mapping(block, mapped_size, payload_size, expected_uid);
}

std::shared_ptr<SharedBufferImportedBlock> SharedBufferHandleCache::import_block(
  const std::string & ipc_name, std::int32_t pid, std::uint32_t block_id,
  std::uint64_t mapped_size, std::uint64_t payload_size, std::uint64_t expected_uid)
{
  if (
    ipc_name.empty() || mapped_size < kSharedBufferPayloadOffset ||
    mapped_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
  {
    throw std::runtime_error("invalid shared memory descriptor metadata");
  }

  std::lock_guard<std::mutex> lock(cache_mutex());
  const CacheKey key{pid, block_id, ipc_name};
  auto existing = cache().find(key);
  if (existing != cache().end()) {
    validate(*existing->second, mapped_size, payload_size, expected_uid);
    return existing->second;
  }

  const auto platform_mapping = import_platform_mapping(
    ipc_name, static_cast<std::size_t>(mapped_size));
  std::shared_ptr<SharedBufferImportedBlock> imported;
  try {
    imported = std::make_shared<SharedBufferImportedBlock>(
      platform_mapping.native_handle, platform_mapping.mapping, platform_mapping.mapped_size,
      ipc_name);
  } catch (...) {
    destroy_platform_mapping(
      platform_mapping.native_handle, platform_mapping.mapping, platform_mapping.mapped_size);
    throw;
  }
  try {
    validate(*imported, mapped_size, payload_size, expected_uid);
  } catch (...) {
    imported.reset();
    throw;
  }
  cache().emplace(key, imported);
  return imported;
}

}  // namespace shmem_buffer
