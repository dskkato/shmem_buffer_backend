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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_HANDLE_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_HANDLE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "shmem_buffer/visibility_control.h"
#include "rosidl_buffer/buffer.hpp"

namespace shmem_buffer
{

class ShmemBuffer;

struct HandleState
{
  enum class State
  {
    Unset,
    InUse,
    Finalized
  };

  std::mutex mutex;
  State state{State::Unset};
  std::size_t active_readers{0};
};

/// Scoped const access to a shared-memory payload.
///
/// The handle owns one local read-access slot and, when the buffer is backed
/// by an IPC block, one logical inter-process reader reference until it is
/// destroyed. It does not provide a memory-ordering or asynchronous
/// completion guarantee.
class SHMEM_BUFFER_PUBLIC ReadHandle
{
public:
  ReadHandle() = default;
  ReadHandle(const ReadHandle &) = delete;
  ReadHandle & operator=(const ReadHandle &) = delete;

  ReadHandle(ReadHandle && other) noexcept;
  ReadHandle & operator=(ReadHandle && other) noexcept;
  ~ReadHandle();

  const std::uint8_t * get_ptr() const {return data_ptr_;}

  std::shared_ptr<rosidl::Buffer<std::uint8_t>> get_promoted_buffer() const
  {
    return promoted_buffer_;
  }

  void set_promoted_buffer(std::shared_ptr<rosidl::Buffer<std::uint8_t>> buffer)
  {
    promoted_buffer_ = std::move(buffer);
  }

private:
  friend class ShmemBuffer;

  ReadHandle(
    const std::uint8_t * data_ptr, std::shared_ptr<void> reader_lease, std::shared_ptr<void> owner,
    std::shared_ptr<HandleState> state);

  void release() noexcept;

  const std::uint8_t * data_ptr_{nullptr};
  std::shared_ptr<HandleState> state_;
  std::shared_ptr<void> reader_lease_;
  std::shared_ptr<void> owner_;
  std::shared_ptr<rosidl::Buffer<std::uint8_t>> promoted_buffer_;
};

/// Scoped mutable access to a shared-memory payload.
///
/// Only one writer can be active, and a writer cannot be acquired while a
/// local ReadHandle is alive. Destruction finalizes the local write state
/// idempotently. After explicit finalization through ShmemBuffer, the caller
/// must not use this handle's mutable pointer again.
class SHMEM_BUFFER_PUBLIC WriteHandle
{
public:
  WriteHandle() = default;
  WriteHandle(const WriteHandle &) = delete;
  WriteHandle & operator=(const WriteHandle &) = delete;

  WriteHandle(WriteHandle && other) noexcept;
  WriteHandle & operator=(WriteHandle && other) noexcept;
  ~WriteHandle();

  std::uint8_t * get_ptr() {return data_ptr_;}

  std::shared_ptr<rosidl::Buffer<std::uint8_t>> get_promoted_buffer() const
  {
    return promoted_buffer_;
  }

  void set_promoted_buffer(std::shared_ptr<rosidl::Buffer<std::uint8_t>> buffer)
  {
    promoted_buffer_ = std::move(buffer);
  }

private:
  friend class ShmemBuffer;

  WriteHandle(
    std::uint8_t * data_ptr, std::shared_ptr<HandleState> state, std::shared_ptr<void> owner);

  void release() noexcept;

  std::uint8_t * data_ptr_{nullptr};
  std::shared_ptr<HandleState> state_;
  std::shared_ptr<void> owner_;
  std::shared_ptr<rosidl::Buffer<std::uint8_t>> promoted_buffer_;
};

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_HANDLE_HPP_
