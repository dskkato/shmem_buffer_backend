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

#include "shmem_buffer/shmem_buffer_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdexcept>
#include <string>

#include "shmem_buffer/shmem_buffer_ipc_manager.hpp"

namespace shmem_buffer
{

namespace
{

std::wstring widen_ascii(const std::string & value)
{
  return std::wstring(value.begin(), value.end());
}

}  // namespace

struct SharedBufferFdBroker::Impl
{
};

SharedBufferFdBroker::SharedBufferFdBroker()
: impl_(std::make_unique<Impl>()) {}

SharedBufferFdBroker::~SharedBufferFdBroker() = default;

std::string SharedBufferFdBroker::register_block(SharedBufferBlock * block)
{
  if (block == nullptr || block->native_handle < 0) {
    return {};
  }
  return block->socket_path;
}

SharedBufferPlatformMapping create_platform_mapping(
  std::size_t mapped_size, std::uint32_t block_id, std::uint64_t nonce)
{
  const auto size = static_cast<std::uint64_t>(mapped_size);

  HANDLE mapping_handle = nullptr;
  DWORD mapping_error = ERROR_SUCCESS;
  std::string mapping_name;
  for (unsigned int attempt = 0; attempt < 8; ++attempt) {
    mapping_name = "Local\\rosidl_shmem_buffer_" +
      std::to_string(GetCurrentProcessId()) + "_" + std::to_string(nonce + attempt) + "_" +
      std::to_string(block_id);
    const auto wide_name = widen_ascii(mapping_name);
    SetLastError(ERROR_SUCCESS);
    mapping_handle = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
      static_cast<DWORD>(size & 0xffffffffULL), wide_name.c_str());
    if (mapping_handle == nullptr) {
      mapping_error = GetLastError();
      break;
    }
    mapping_error = GetLastError();
    if (mapping_error != ERROR_ALREADY_EXISTS) {
      break;
    }
    (void)CloseHandle(mapping_handle);
    mapping_handle = nullptr;
  }
  if (mapping_handle == nullptr || mapping_error == ERROR_ALREADY_EXISTS) {
    if (mapping_handle != nullptr) {
      (void)CloseHandle(mapping_handle);
    }
    throw std::runtime_error(
      "CreateFileMappingW failed: Windows error " + std::to_string(mapping_error));
  }

  void * mapping = MapViewOfFile(mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, mapped_size);
  if (mapping == nullptr) {
    const DWORD error = GetLastError();
    (void)CloseHandle(mapping_handle);
    throw std::runtime_error("MapViewOfFile failed: Windows error " + std::to_string(error));
  }
  return SharedBufferPlatformMapping{
    reinterpret_cast<std::intptr_t>(mapping_handle), mapping, mapped_size, mapping_name};
}

SharedBufferPlatformMapping import_platform_mapping(
  const std::string & ipc_name,
  std::size_t mapped_size)
{
  const auto wide_name = widen_ascii(ipc_name);
  HANDLE mapping_handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide_name.c_str());
  if (mapping_handle == nullptr) {
    throw std::runtime_error(
      "OpenFileMappingW failed: Windows error " + std::to_string(GetLastError()));
  }
  void * mapping = MapViewOfFile(mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0, mapped_size);
  if (mapping == nullptr) {
    const DWORD error = GetLastError();
    (void)CloseHandle(mapping_handle);
    throw std::runtime_error("MapViewOfFile failed: Windows error " + std::to_string(error));
  }
  return SharedBufferPlatformMapping{
    reinterpret_cast<std::intptr_t>(mapping_handle), mapping, mapped_size, ipc_name};
}

void destroy_platform_mapping(
  std::intptr_t native_handle, void * mapping, std::size_t) noexcept
{
  if (mapping != nullptr) {
    (void)UnmapViewOfFile(mapping);
  }
  if (native_handle >= 0) {
    (void)CloseHandle(reinterpret_cast<HANDLE>(native_handle));
  }
}

}  // namespace shmem_buffer
