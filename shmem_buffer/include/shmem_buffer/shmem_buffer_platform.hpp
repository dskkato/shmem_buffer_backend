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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_PLATFORM_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_PLATFORM_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

namespace shmem_buffer
{

/// The native resources needed to access one shared-memory mapping.
///
/// The native handle is an fd on Linux and a HANDLE represented as an
/// intptr_t on Windows.  Keeping this type platform-neutral prevents the
/// memory-pool and cache code from depending on either platform's headers.
struct ShmemBufferPlatformMapping
{
  std::intptr_t native_handle{-1};
  void * mapping{nullptr};
  std::size_t mapped_size{0};
  std::string ipc_name;
};

ShmemBufferPlatformMapping create_platform_mapping(
  std::size_t mapped_size, std::uint32_t block_id, std::uint64_t nonce);

ShmemBufferPlatformMapping import_platform_mapping(
  const std::string & ipc_name, std::size_t mapped_size);

void destroy_platform_mapping(
  std::intptr_t native_handle, void * mapping, std::size_t mapped_size) noexcept;

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_PLATFORM_HPP_
