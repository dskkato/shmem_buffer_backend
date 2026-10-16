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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_IMPL_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_IMPL_HPP_

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <type_traits>

#include "shmem_buffer/shmem_buffer.hpp"
#include "shmem_buffer/visibility_control.h"
#include "rosidl_buffer/buffer_impl_base.hpp"
#include "rosidl_buffer/cpu_buffer_impl.hpp"

namespace shmem_buffer
{

/// Process-wide allocation pool shared by applications and backend plugins.
///
/// The definition is intentionally out-of-line in the shmem_buffer shared
/// library so that different DSOs do not create independent pool instances.
SHMEM_BUFFER_PUBLIC std::shared_ptr<ShmemBufferMemoryPool> get_or_create_global_pool();

class ShmemBufferError : public std::runtime_error
{
public:
  explicit ShmemBufferError(const std::string & message)
  : std::runtime_error(message) {}
};

template<typename T>
class ShmemBufferImpl : public rosidl::BufferImplBase<T>
{
  static_assert(std::is_trivially_copyable_v<T>, "ShmemBufferImpl requires trivially copyable T");

public:
  ShmemBufferImpl() = default;

  explicit ShmemBufferImpl(std::size_t size)
  : size_(size)
  {
    if (size_ > 0) {
      allocate(size_);
    }
  }

  ShmemBufferImpl(ShmemBuffer && buffer, std::size_t size)
  : size_(size), shmem_buffer_(std::move(buffer))
  {
  }

  ~ShmemBufferImpl() override = default;

  ShmemBufferImpl(const ShmemBufferImpl &) = delete;
  ShmemBufferImpl & operator=(const ShmemBufferImpl &) = delete;
  ShmemBufferImpl(ShmemBufferImpl &&) = delete;
  ShmemBufferImpl & operator=(ShmemBufferImpl &&) = delete;

  std::string get_backend_type() const override {return "shmem";}
  std::size_t size() const override {return size_;}

  void resize(std::size_t size)
  {
    if (size == size_) {
      return;
    }
    if (size == 0) {
      shmem_buffer_ = ShmemBuffer();
      size_ = 0;
      return;
    }

    ShmemBuffer new_buffer = allocate_raw(size);
    const std::size_t copy_size = std::min(size, size_) * sizeof(T);
    if (copy_size > 0 && shmem_buffer_.get_ptr() != nullptr) {
      ReadHandle read = shmem_buffer_.get_read_handle();
      WriteHandle write = new_buffer.get_write_handle();
      std::memcpy(write.get_ptr(), read.get_ptr(), copy_size);
    }
    shmem_buffer_ = std::move(new_buffer);
    size_ = size;
  }

  void clear()
  {
    shmem_buffer_ = ShmemBuffer();
    size_ = 0;
  }

  std::unique_ptr<rosidl::BufferImplBase<T>> to_cpu() const override
  {
    auto cpu = std::make_unique<rosidl::CpuBufferImpl<T>>();
    cpu->get_storage().resize(size_);
    if (size_ > 0) {
      ReadHandle read = shmem_buffer_.get_read_handle();
      std::memcpy(cpu->get_storage().data(), read.get_ptr(), size_ * sizeof(T));
    }
    return cpu;
  }

  std::unique_ptr<rosidl::BufferImplBase<T>> clone() const override
  {
    auto copy = std::make_unique<ShmemBufferImpl<T>>(size_);
    if (size_ > 0) {
      ReadHandle read = shmem_buffer_.get_read_handle();
      WriteHandle write = copy->shmem_buffer_.get_write_handle();
      std::memcpy(write.get_ptr(), read.get_ptr(), size_ * sizeof(T));
    }
    return copy;
  }

  ShmemBuffer & get_shmem_buffer() {return shmem_buffer_;}
  const ShmemBuffer & get_shmem_buffer() const {return shmem_buffer_;}

  static std::shared_ptr<ShmemBufferMemoryPool> get_or_create_global_pool()
  {
    return shmem_buffer::get_or_create_global_pool();
  }

  static bool is_pool_ipc_capable() {return get_or_create_global_pool()->is_ipc_capable();}

private:
  static std::size_t byte_count(std::size_t count)
  {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw ShmemBufferError("shared-memory buffer size overflows size_t");
    }
    return count * sizeof(T);
  }

  static ShmemBuffer allocate_raw(std::size_t count)
  {
    auto pool = get_or_create_global_pool();
    const std::size_t bytes = byte_count(count);
    ShmemBufferBlock * block = pool->allocate(bytes);
    auto deleter = pool->deleter(block);
    auto * payload = reinterpret_cast<std::uint8_t *>(block->mapping) + kShmemBufferPayloadOffset;
    return ShmemBuffer(
      payload, bytes, std::move(deleter), block->control, nullptr, block->block_id,
      block->mapped_size);
  }

  void allocate(std::size_t count) {shmem_buffer_ = allocate_raw(count);}

  template<typename U>
  friend class ShmemBufferImpl;
  template<typename U>
  friend class ShmemBufferApiAccess;

  std::size_t size_{0};
  ShmemBuffer shmem_buffer_;
};

inline std::shared_ptr<ShmemBufferMemoryPool> get_global_shmem_buffer_pool()
{
  return ShmemBufferImpl<std::uint8_t>::get_or_create_global_pool();
}

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_IMPL_HPP_
