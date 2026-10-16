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

#include "shmem_buffer/shmem_buffer.hpp"

#include <stdexcept>

namespace shmem_buffer
{

namespace
{

std::shared_ptr<void> make_lease(ShmemBufferControlHeader * control)
{
  if (control == nullptr) {
    return {};
  }
  if (!try_acquire_shmem_buffer_reader(control)) {
    throw std::runtime_error("shared-memory buffer block is being reused");
  }
  return std::shared_ptr<void>(control, [](void * ptr) {
             auto * header = static_cast<ShmemBufferControlHeader *>(ptr);
             release_shmem_buffer_reader(header);
  });
}

}  // namespace

ReadHandle::ReadHandle(
  const std::uint8_t * data_ptr, std::shared_ptr<void> reader_lease, std::shared_ptr<void> owner,
  std::shared_ptr<HandleState> state)
: data_ptr_(data_ptr),
  state_(std::move(state)),
  reader_lease_(std::move(reader_lease)),
  owner_(std::move(owner))
{
}

ReadHandle::ReadHandle(ReadHandle && other) noexcept
: data_ptr_(other.data_ptr_),
  state_(std::move(other.state_)),
  reader_lease_(std::move(other.reader_lease_)),
  owner_(std::move(other.owner_)),
  promoted_buffer_(std::move(other.promoted_buffer_))
{
  other.data_ptr_ = nullptr;
}

ReadHandle & ReadHandle::operator=(ReadHandle && other) noexcept
{
  if (this != &other) {
    release();
    data_ptr_ = other.data_ptr_;
    state_ = std::move(other.state_);
    reader_lease_ = std::move(other.reader_lease_);
    owner_ = std::move(other.owner_);
    promoted_buffer_ = std::move(other.promoted_buffer_);
    other.data_ptr_ = nullptr;
  }
  return *this;
}

ReadHandle::~ReadHandle() {release();}

void ReadHandle::release() noexcept
{
  auto state = std::move(state_);
  if (state != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->active_readers > 0) {
        --state->active_readers;
      }
    }
  }
  reader_lease_.reset();
  owner_.reset();
  promoted_buffer_.reset();
  data_ptr_ = nullptr;
}

WriteHandle::WriteHandle(
  std::uint8_t * data_ptr, std::shared_ptr<HandleState> state, std::shared_ptr<void> owner)
: data_ptr_(data_ptr), state_(std::move(state)), owner_(std::move(owner))
{
}

WriteHandle::WriteHandle(WriteHandle && other) noexcept
: data_ptr_(other.data_ptr_),
  state_(std::move(other.state_)),
  owner_(std::move(other.owner_)),
  promoted_buffer_(std::move(other.promoted_buffer_))
{
  other.data_ptr_ = nullptr;
}

WriteHandle & WriteHandle::operator=(WriteHandle && other) noexcept
{
  if (this != &other) {
    release();
    data_ptr_ = other.data_ptr_;
    state_ = std::move(other.state_);
    owner_ = std::move(other.owner_);
    promoted_buffer_ = std::move(other.promoted_buffer_);
    other.data_ptr_ = nullptr;
  }
  return *this;
}

WriteHandle::~WriteHandle() {release();}

void WriteHandle::release() noexcept
{
  if (state_ != nullptr) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->state = HandleState::State::Finalized;
  }
  promoted_buffer_.reset();
  owner_.reset();
  data_ptr_ = nullptr;
}

ShmemBuffer::ShmemBuffer(
  void * payload, std::size_t size, std::function<void(std::uint8_t *)> deleter,
  ShmemBufferControlHeader * control, std::shared_ptr<void> owner, std::uint32_t block_id,
  std::uint64_t mapped_size, bool writable, std::int32_t ipc_pid, std::string ipc_name,
  std::uint64_t ipc_uid)
: data_ptr_(static_cast<std::uint8_t *>(payload)),
  size_(size),
  deleter_(std::move(deleter)),
  control_(control),
  owner_(std::move(owner)),
  block_id_(block_id),
  mapped_size_(mapped_size),
  writable_(writable),
  ipc_pid_(ipc_pid),
  ipc_name_(std::move(ipc_name)),
  ipc_uid_(ipc_uid)
{
  if (data_ptr_ == nullptr && size_ != 0) {
    throw std::invalid_argument("ShmemBuffer payload must not be null");
  }
  if (!deleter_) {
    deleter_ = [](std::uint8_t *) {};
  }
}

ShmemBuffer::~ShmemBuffer() {reset();}

ShmemBuffer::ShmemBuffer(ShmemBuffer && other) noexcept
: data_ptr_(other.data_ptr_),
  size_(other.size_),
  deleter_(std::move(other.deleter_)),
  control_(other.control_),
  owner_(std::move(other.owner_)),
  held_reader_lease_(std::move(other.held_reader_lease_)),
  handle_state_(std::move(other.handle_state_)),
  block_id_(other.block_id_),
  mapped_size_(other.mapped_size_),
  writable_(other.writable_),
  ipc_pid_(other.ipc_pid_),
  ipc_name_(std::move(other.ipc_name_)),
  ipc_uid_(other.ipc_uid_)
{
  other.data_ptr_ = nullptr;
  other.size_ = 0;
  other.control_ = nullptr;
  other.block_id_ = 0;
  other.mapped_size_ = 0;
  other.writable_ = true;
  other.ipc_pid_ = 0;
  other.ipc_uid_ = 0;
}

ShmemBuffer & ShmemBuffer::operator=(ShmemBuffer && other) noexcept
{
  if (this != &other) {
    reset();
    data_ptr_ = other.data_ptr_;
    size_ = other.size_;
    deleter_ = std::move(other.deleter_);
    control_ = other.control_;
    owner_ = std::move(other.owner_);
    held_reader_lease_ = std::move(other.held_reader_lease_);
    handle_state_ = std::move(other.handle_state_);
    block_id_ = other.block_id_;
    mapped_size_ = other.mapped_size_;
    writable_ = other.writable_;
    ipc_pid_ = other.ipc_pid_;
    ipc_name_ = std::move(other.ipc_name_);
    ipc_uid_ = other.ipc_uid_;
    other.data_ptr_ = nullptr;
    other.size_ = 0;
    other.control_ = nullptr;
    other.block_id_ = 0;
    other.mapped_size_ = 0;
    other.writable_ = true;
    other.ipc_pid_ = 0;
    other.ipc_uid_ = 0;
  }
  return *this;
}

ReadHandle ShmemBuffer::get_read_handle() const
{
  if (handle_state_ == nullptr) {
    handle_state_ = std::make_shared<HandleState>();
  }
  auto state = handle_state_;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->state == HandleState::State::InUse) {
      throw std::runtime_error("cannot acquire a read handle while writing");
    }
    ++state->active_readers;
  }

  try {
    std::shared_ptr<void> reader_lease = make_lease(control_);
    return ReadHandle(data_ptr_, std::move(reader_lease), owner_, std::move(state));
  } catch (...) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->active_readers > 0) {
      --state->active_readers;
    }
    throw;
  }
}

WriteHandle ShmemBuffer::get_write_handle()
{
  if (!writable_) {
    throw std::runtime_error("cannot acquire a write handle for a read-only shared-memory buffer");
  }
  if (data_ptr_ == nullptr || size_ == 0) {
    throw std::runtime_error("cannot acquire a write handle for an empty shared-memory buffer");
  }
  if (handle_state_ == nullptr) {
    handle_state_ = std::make_shared<HandleState>();
  }
  std::lock_guard<std::mutex> lock(handle_state_->mutex);
  if (handle_state_->state == HandleState::State::InUse) {
    throw std::runtime_error("shared-memory buffer write handle already in use");
  }
  if (handle_state_->state == HandleState::State::Finalized) {
    throw std::runtime_error("shared-memory buffer write has already been finalized");
  }
  if (handle_state_->active_readers != 0) {
    throw std::runtime_error("shared-memory buffer read handle already in use");
  }
  handle_state_->state = HandleState::State::InUse;
  return WriteHandle(data_ptr_, handle_state_, owner_);
}

void ShmemBuffer::finalize_write_handle() const
{
  if (handle_state_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(handle_state_->mutex);
  if (handle_state_->state == HandleState::State::InUse) {
    handle_state_->state = HandleState::State::Finalized;
  }
}

void ShmemBuffer::hold_reader_reference()
{
  if (held_reader_lease_ != nullptr) {
    return;
  }
  held_reader_lease_ = make_lease(control_);
}

void ShmemBuffer::reset() noexcept
{
  held_reader_lease_.reset();
  handle_state_.reset();
  owner_.reset();
  if (data_ptr_ != nullptr && deleter_) {
    try {
      deleter_(data_ptr_);
    } catch (...) {
      // A buffer destructor must not propagate a pool cleanup exception.
    }
  }
  data_ptr_ = nullptr;
  size_ = 0;
  control_ = nullptr;
  block_id_ = 0;
  mapped_size_ = 0;
  writable_ = true;
  ipc_pid_ = 0;
  ipc_name_.clear();
  ipc_uid_ = 0;
  deleter_ = {};
}

}  // namespace shmem_buffer
