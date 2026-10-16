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

#ifndef SHMEM_BUFFER__SHMEM_BUFFER_HPP_
#define SHMEM_BUFFER__SHMEM_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "shmem_buffer/shmem_buffer_handle.hpp"
#include "shmem_buffer/shmem_buffer_memory_pool.hpp"
#include "shmem_buffer/visibility_control.h"

namespace shmem_buffer
{

/// Low-level RAII view of a publisher or imported shared-memory payload.
class SHMEM_BUFFER_PUBLIC SharedBuffer
{
public:
  SharedBuffer() = default;

  SharedBuffer(
    void * payload, std::size_t size, std::function<void(std::uint8_t *)> deleter,
    SharedBufferControlHeader * control = nullptr, std::shared_ptr<void> owner = nullptr,
    std::uint32_t block_id = 0, std::uint64_t mapped_size = 0, bool writable = true,
    std::int32_t ipc_pid = 0, std::string ipc_name = {}, std::uint64_t ipc_uid = 0);

  ~SharedBuffer();

  SharedBuffer(const SharedBuffer &) = delete;
  SharedBuffer & operator=(const SharedBuffer &) = delete;

  SharedBuffer(SharedBuffer && other) noexcept;
  SharedBuffer & operator=(SharedBuffer && other) noexcept;

  ReadHandle get_read_handle() const;
  WriteHandle get_write_handle();
  void finalize_write_handle() const;

  void hold_reader_reference();

  std::size_t size() const {return size_;}
  std::uint8_t * get_ptr() {return data_ptr_;}
  const std::uint8_t * get_ptr() const {return data_ptr_;}
  SharedBufferControlHeader * control() const {return control_;}
  std::uint32_t block_id() const {return block_id_;}
  std::uint64_t mapped_size() const {return mapped_size_;}
  bool writable() const {return writable_;}

  /// True when this buffer was imported from another process and can be
  /// described again without looking it up in the local publisher pool.
  bool has_ipc_descriptor() const
  {
    return ipc_pid_ > 0 && !ipc_name_.empty() && ipc_uid_ != 0;
  }

  std::int32_t ipc_pid() const {return ipc_pid_;}
  const std::string & ipc_name() const {return ipc_name_;}
  std::uint64_t ipc_uid() const {return ipc_uid_;}

private:
  void reset() noexcept;

  std::uint8_t * data_ptr_{nullptr};
  std::size_t size_{0};
  std::function<void(std::uint8_t *)> deleter_;
  SharedBufferControlHeader * control_{nullptr};
  std::shared_ptr<void> owner_;
  std::shared_ptr<void> held_reader_lease_;
  mutable std::shared_ptr<HandleState> handle_state_;
  std::uint32_t block_id_{0};
  std::uint64_t mapped_size_{0};
  bool writable_{true};
  std::int32_t ipc_pid_{0};
  std::string ipc_name_;
  std::uint64_t ipc_uid_{0};
};

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__SHMEM_BUFFER_HPP_
