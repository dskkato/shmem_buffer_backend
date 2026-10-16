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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_API_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_API_HPP_

#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "shmem_buffer/shmem_buffer_impl.hpp"
#include "rosidl_buffer/buffer.hpp"

namespace shmem_buffer
{

/// \brief Allocate a fresh shared-memory-backed \c rosidl::Buffer<std::uint8_t>.
///
/// \details Allocates \p count payload bytes from the shared-memory pool.
/// The returned buffer owns the allocation. This function does not initialize or
/// copy payload data. The caller is responsible to assign the buffer to a message
/// field (e.g. `msg.data = allocate_buffer(n)` for messages that follow the `data`
/// convention.
inline rosidl::Buffer<std::uint8_t> allocate_buffer(std::size_t count)
{
  return rosidl::Buffer<std::uint8_t>(std::make_unique<ShmemBufferImpl<std::uint8_t>>(count));
}

namespace detail
{

/// \brief Allocate a shared, fresh shared-memory-backed \c rosidl::Buffer<std::uint8_t>.
inline std::shared_ptr<rosidl::Buffer<std::uint8_t>> allocate_shmem_buffer_shared(
  std::size_t byte_count)
{
  return std::make_shared<rosidl::Buffer<std::uint8_t>>(
    std::make_unique<ShmemBufferImpl<std::uint8_t>>(byte_count));
}

template<typename T>
inline ShmemBufferImpl<T> * shmem_buffer_impl_of(rosidl::Buffer<T> & buffer)
{
  return dynamic_cast<ShmemBufferImpl<T> *>(buffer.get_impl());
}

}  // namespace detail

/// \brief Acquire exclusive mutable access to a shared-memory-backed buffer payload.
///
/// \details The input buffer must have a non-null implementation, contain at
/// least one element, and already be shared-memory-backed. This function does not
/// convert or replace buffers from another backend. Rejecting those buffers is
/// intentional: a handle for a newly allocated, detached shared-memory buffer would
/// not be reflected by the original buffer during message publication.
template<typename T>
WriteHandle from_output_buffer(rosidl::Buffer<T> & buffer)
{
  auto * impl = buffer.get_impl();
  if (impl == nullptr) {
    throw ShmemBufferError("from_output_buffer called with null buffer implementation");
  }
  if (buffer.size() == 0) {
    throw ShmemBufferError("from_output_buffer called on empty buffer");
  }

  auto * shmem_buffer_impl = dynamic_cast<ShmemBufferImpl<T> *>(impl);
  if (shmem_buffer_impl == nullptr) {
    throw ShmemBufferError("from_output_buffer requires a shared-memory-backed buffer");
  }
  return shmem_buffer_impl->get_shmem_buffer().get_write_handle();
}

/// \brief Acquire read-only access to a buffer payload.
///
/// \details The input buffer must contain at least one element. If \p buffer
/// is already shared-memory-backed, a read handle for that allocation is returned directly.
/// Otherwise, a fresh shared-memory-backed \c rosidl::Buffer<std::uint8_t> of the same byte
/// size is allocated and the source contents are copied synchronously
template<typename T>
ReadHandle from_input_buffer(const rosidl::Buffer<T> & buffer)
{
  const auto * impl = buffer.get_impl();
  if (impl == nullptr) {
    throw ShmemBufferError("from_input_buffer called with null buffer implementation");
  }
  if (buffer.size() == 0) {
    throw ShmemBufferError("from_input_buffer called on empty buffer");
  }

  if (const auto * shmem_buffer_impl = dynamic_cast<const ShmemBufferImpl<T> *>(impl)) {
    return shmem_buffer_impl->get_shmem_buffer().get_read_handle();
  }

  if (buffer.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    throw ShmemBufferError("input buffer size overflows size_t");
  }
  const std::size_t bytes = buffer.size() * sizeof(T);
  auto promoted = detail::allocate_shmem_buffer_shared(bytes);
  auto * promoted_impl = detail::shmem_buffer_impl_of(*promoted);
  const std::vector<T> source = buffer.to_vector();
  {
    auto write = promoted_impl->get_shmem_buffer().get_write_handle();
    std::memcpy(write.get_ptr(), source.data(), bytes);
  }
  auto read = promoted_impl->get_shmem_buffer().get_read_handle();
  read.set_promoted_buffer(std::move(promoted));
  return read;
}

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_API_HPP_
