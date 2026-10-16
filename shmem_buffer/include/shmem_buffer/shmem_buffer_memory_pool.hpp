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

inline constexpr std::uint64_t kSharedBufferControlMagic = 0x5348415245425546ULL;  // "SHAREBUF"
inline constexpr std::uint32_t kSharedBufferControlAbiVersion = 3;
inline constexpr std::size_t kSharedBufferPayloadOffset = 64;
inline constexpr std::uint64_t kSharedBufferGracePeriodUs = 100000;
inline constexpr std::uint32_t kSharedBufferReuseClaimed = (std::uint32_t{1} << 31);
inline constexpr std::uint32_t kSharedBufferReaderCountMask = ~kSharedBufferReuseClaimed;

/// \brief Metadata at the beginning of every mapped shared-memory region.
///
/// \details The payload starts at kSharedBufferPayloadOffset, leaving a cache-line
/// boundary between the control metadata and payload.  The non-atomic fields
/// identify the mapping protocol and fixed payload layout; the atomic fields
/// coordinate publication and reuse across processes.
struct alignas(64) SharedBufferControlHeader
{
  std::uint64_t magic{kSharedBufferControlMagic};
  std::uint32_t abi_version{kSharedBufferControlAbiVersion};
  std::uint64_t payload_size{0};
  std::atomic<std::uint64_t> ipc_uid{0};

  // Bit 31 claims the block for reuse; bits 0-30 count active readers.
  // Reader acquisition and reuse claiming both use CAS.
  // This prevents a reader from appearing after the publisher
  // observes zero readers but before it starts reusing the block.
  std::atomic<std::uint32_t> reader_state{0};
  std::atomic<std::uint64_t> publish_timestamp_us{0};

  SharedBufferControlHeader() = default;
  SharedBufferControlHeader(const SharedBufferControlHeader &) = delete;
  SharedBufferControlHeader & operator=(const SharedBufferControlHeader &) = delete;
};
static_assert(
  sizeof(SharedBufferControlHeader) <= kSharedBufferPayloadOffset,
  "shared-buffer control metadata must fit before the fixed payload offset");

inline bool try_acquire_shmem_buffer_reader(SharedBufferControlHeader * control)
{
  if (control == nullptr) {
    return false;
  }

  auto value = control->reader_state.load(std::memory_order_acquire);
  for (;; ) {
    if (
      (value & kSharedBufferReuseClaimed) != 0 ||
      (value & kSharedBufferReaderCountMask) == kSharedBufferReaderCountMask)
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

inline bool try_claim_shmem_buffer_reuse(SharedBufferControlHeader * control)
{
  if (control == nullptr) {
    return false;
  }
  std::uint32_t expected = 0;
  return control->reader_state.compare_exchange_strong(
    expected, kSharedBufferReuseClaimed, std::memory_order_acq_rel, std::memory_order_acquire);
}

inline void release_shmem_buffer_reader(SharedBufferControlHeader * control) noexcept
{
  if (control != nullptr) {
    control->reader_state.fetch_sub(1, std::memory_order_release);
  }
}

inline void release_shmem_buffer_reuse_claim(SharedBufferControlHeader * control) noexcept
{
  if (control != nullptr) {
    control->reader_state.store(0, std::memory_order_release);
  }
}

static_assert(
  sizeof(SharedBufferControlHeader) <= kSharedBufferPayloadOffset,
  "shared-buffer control metadata must fit before the fixed payload offset");
static_assert(
  kSharedBufferPayloadOffset % alignof(SharedBufferControlHeader) == 0,
  "shared-buffer payload offset must preserve control-header alignment");
static_assert(
  std::atomic<std::uint64_t>::is_always_lock_free,
  "shared-buffer control metadata requires lock-free uint64 atomics");
static_assert(
  std::atomic<std::uint32_t>::is_always_lock_free,
  "shared-buffer control metadata requires lock-free uint32 atomics");

/// Publisher-side allocation and its stable physical identity.
struct SharedBufferBlock
{
  // An fd on Linux or a HANDLE represented as intptr_t on Windows.
  std::intptr_t native_handle{-1};
  void * mapping{nullptr};
  std::size_t mapped_size{0};
  std::size_t payload_size{0};
  std::uint32_t block_id{0};
  SharedBufferControlHeader * control{nullptr};
  std::uint64_t current_uid{0};
  std::string socket_path;
  bool in_use{false};
};

class SharedBufferFdBroker;

/// Publisher-side size-aware pool for Linux shared memory or Windows named mappings.
class SHMEM_BUFFER_PUBLIC SharedBufferMemoryPool
  : public std::enable_shared_from_this<SharedBufferMemoryPool>
{
public:
  SharedBufferMemoryPool();
  ~SharedBufferMemoryPool();

  SharedBufferMemoryPool(const SharedBufferMemoryPool &) = delete;
  SharedBufferMemoryPool & operator=(const SharedBufferMemoryPool &) = delete;
  SharedBufferMemoryPool(SharedBufferMemoryPool &&) = delete;
  SharedBufferMemoryPool & operator=(SharedBufferMemoryPool &&) = delete;

  /// Allocate an exact-size payload block, reusing only a safe free block.
  /// Throws std::runtime_error when shared-memory allocation or mapping fails.
  SharedBufferBlock * allocate(std::size_t payload_size);

  /// Return a publisher-side buffer block to its size bucket.
  void free(SharedBufferBlock * block);

  /// Create a deleter suitable for SharedBuffer's publisher-side ownership.
  std::function<void(std::uint8_t *)> deleter(SharedBufferBlock * block);

  /// Assign the UID for the current generation of a reserved block.
  std::uint64_t assign_uid(SharedBufferBlock * block);

  /// Record that a descriptor for the current generation was created.
  void mark_published(SharedBufferBlock * block);

  /// Refresh the reuse grace period without changing the published generation.
  void refresh_publish_timestamp(SharedBufferControlHeader * control);

  /// Register the block's backing object with the platform IPC broker.
  std::string register_block_for_ipc(SharedBufferBlock * block);

  /// Find the block containing a publisher-side payload pointer.
  SharedBufferBlock * find_block_for_ptr(const void * ptr) const;

  bool is_ipc_capable() const {return ipc_capable_;}

private:
  SharedBufferBlock * create_block(std::size_t payload_size);
  bool try_claim_block(SharedBufferBlock * block) const;

  bool initialized_{false};
  bool ipc_capable_{false};
  std::uint32_t next_block_id_{0};
  std::mt19937_64 uid_rng_{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> uid_dist_{1, UINT64_MAX};

  std::map<std::size_t, std::vector<SharedBufferBlock *>> free_blocks_;
  std::vector<std::unique_ptr<SharedBufferBlock>> all_blocks_;
  std::unique_ptr<SharedBufferFdBroker> broker_;
  mutable std::mutex mutex_;
};

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_MEMORY_POOL_HPP_
