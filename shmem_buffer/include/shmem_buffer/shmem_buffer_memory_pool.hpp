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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_MEMORY_POOL_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_MEMORY_POOL_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "shmem_buffer/visibility_control.h"

namespace shmem_buffer
{

inline constexpr std::uint64_t kShmemBufferControlMagic = 0x53484D454D425546ULL;  // "SHMEMBUF"
inline constexpr std::uint32_t kShmemBufferControlAbiVersion = 3;
inline constexpr std::size_t kShmemBufferPayloadOffset = 64;
inline constexpr std::uint64_t kShmemBufferGracePeriodUs = 100000;
inline constexpr std::uint32_t kShmemBufferReuseClaimed = (std::uint32_t{1} << 31);
inline constexpr std::uint32_t kShmemBufferReaderCountMask = ~kShmemBufferReuseClaimed;

/// \brief Metadata at the beginning of every mapped shared-memory region.
///
/// \details The payload starts at kShmemBufferPayloadOffset, leaving a cache-line
/// boundary between the control metadata and payload.  The non-atomic fields
/// identify the mapping protocol and fixed payload layout; the atomic fields
/// coordinate publication and reuse across processes.
struct alignas(64) ShmemBufferControlHeader
{
  std::uint64_t magic{kShmemBufferControlMagic};
  std::uint32_t abi_version{kShmemBufferControlAbiVersion};
  std::uint64_t payload_size{0};
  std::atomic<std::uint64_t> ipc_uid{0};

  // Bit 31 claims the block for reuse; bits 0-30 count active readers.
  // Reader acquisition and reuse claiming both use CAS.
  // This prevents a reader from appearing after the publisher
  // observes zero readers but before it starts reusing the block.
  std::atomic<std::uint32_t> reader_state{0};
  std::atomic<std::uint64_t> publish_timestamp_us{0};

  ShmemBufferControlHeader() = default;
  ShmemBufferControlHeader(const ShmemBufferControlHeader &) = delete;
  ShmemBufferControlHeader & operator=(const ShmemBufferControlHeader &) = delete;
};
static_assert(
  sizeof(ShmemBufferControlHeader) <= kShmemBufferPayloadOffset,
  "shared-memory control metadata must fit before the fixed payload offset");

inline bool try_acquire_shmem_buffer_reader(ShmemBufferControlHeader * control)
{
  if (control == nullptr) {
    return false;
  }

  auto value = control->reader_state.load(std::memory_order_acquire);
  for (;; ) {
    if (
      (value & kShmemBufferReuseClaimed) != 0 ||
      (value & kShmemBufferReaderCountMask) == kShmemBufferReaderCountMask)
    {
      return false;
    }
    if (control->reader_state.compare_exchange_weak(
          value, value + 1, std::memory_order_acq_rel, std::memory_order_acquire))
    {
      return true;
    }
  }
}

inline bool try_claim_shmem_buffer_reuse(ShmemBufferControlHeader * control)
{
  if (control == nullptr) {
    return false;
  }
  std::uint32_t expected = 0;
  return control->reader_state.compare_exchange_strong(
    expected, kShmemBufferReuseClaimed, std::memory_order_acq_rel, std::memory_order_acquire);
}

inline void release_shmem_buffer_reader(ShmemBufferControlHeader * control) noexcept
{
  if (control != nullptr) {
    control->reader_state.fetch_sub(1, std::memory_order_release);
  }
}

inline void release_shmem_buffer_reuse_claim(ShmemBufferControlHeader * control) noexcept
{
  if (control != nullptr) {
    control->reader_state.store(0, std::memory_order_release);
  }
}

static_assert(
  sizeof(ShmemBufferControlHeader) <= kShmemBufferPayloadOffset,
  "shared-memory control metadata must fit before the fixed payload offset");
static_assert(
  kShmemBufferPayloadOffset % alignof(ShmemBufferControlHeader) == 0,
  "shared-memory payload offset must preserve control-header alignment");
static_assert(
  std::atomic<std::uint64_t>::is_always_lock_free,
  "shared-memory control metadata requires lock-free uint64 atomics");
static_assert(
  std::atomic<std::uint32_t>::is_always_lock_free,
  "shared-memory control metadata requires lock-free uint32 atomics");

/// Publisher-side allocation and its stable physical identity.
struct ShmemBufferBlock
{
  // An fd on Linux or a HANDLE represented as intptr_t on Windows.
  std::intptr_t native_handle{-1};
  void * mapping{nullptr};
  std::size_t mapped_size{0};
  std::size_t payload_size{0};
  std::uint32_t block_id{0};
  ShmemBufferControlHeader * control{nullptr};
  std::uint64_t current_uid{0};
  std::string socket_path;
  bool in_use{false};
};

class ShmemBufferFdBroker;

/// Publisher-side size-aware pool for Linux shared memory or Windows named mappings.
class SHMEM_BUFFER_PUBLIC ShmemBufferMemoryPool
  : public std::enable_shared_from_this<ShmemBufferMemoryPool>
{
public:
  ShmemBufferMemoryPool();
  ~ShmemBufferMemoryPool();

  ShmemBufferMemoryPool(const ShmemBufferMemoryPool &) = delete;
  ShmemBufferMemoryPool & operator=(const ShmemBufferMemoryPool &) = delete;
  ShmemBufferMemoryPool(ShmemBufferMemoryPool &&) = delete;
  ShmemBufferMemoryPool & operator=(ShmemBufferMemoryPool &&) = delete;

  /// Allocate an exact-size payload block, reusing only a safe free block.
  /// Throws std::runtime_error when shared-memory allocation or mapping fails.
  ShmemBufferBlock * allocate(std::size_t payload_size);

  /// Return a publisher-side buffer block to its size bucket.
  void free(ShmemBufferBlock * block);

  /// Create a deleter suitable for ShmemBuffer's publisher-side ownership.
  std::function<void(std::uint8_t *)> deleter(ShmemBufferBlock * block);

  /// Assign the UID for the current generation of a reserved block.
  std::uint64_t assign_uid(ShmemBufferBlock * block);

  /// Record that a descriptor for the current generation was created.
  void mark_published(ShmemBufferBlock * block);

  /// Refresh the reuse grace period without changing the published generation.
  void refresh_publish_timestamp(ShmemBufferControlHeader * control);

  /// Register the block's backing object with the platform IPC broker.
  std::string register_block_for_ipc(ShmemBufferBlock * block);

  /// Find the block containing a publisher-side payload pointer.
  ShmemBufferBlock * find_block_for_ptr(const void * ptr) const;

  bool is_ipc_capable() const {return ipc_capable_;}

private:
  ShmemBufferBlock * create_block(std::size_t payload_size);
  bool try_claim_block(ShmemBufferBlock * block) const;

  bool initialized_{false};
  bool ipc_capable_{false};
  std::uint32_t next_block_id_{0};
  std::mt19937_64 uid_rng_{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> uid_dist_{1, UINT64_MAX};

  std::map<std::size_t, std::vector<ShmemBufferBlock *>> free_blocks_;
  std::vector<std::unique_ptr<ShmemBufferBlock>> all_blocks_;
  std::unique_ptr<ShmemBufferFdBroker> broker_;
  mutable std::mutex mutex_;
};

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_MEMORY_POOL_HPP_
