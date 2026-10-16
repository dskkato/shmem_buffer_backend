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

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "shmem_buffer/shmem_buffer_ipc_manager.hpp"

namespace shmem_buffer
{

namespace
{

int create_linux_shared_memory_fd()
{
  return static_cast<int>(syscall(SYS_memfd_create, "rosidl_shmem_buffer", MFD_CLOEXEC));
}

sockaddr_un unix_address(const std::string & path)
{
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  return address;
}

int create_fd_server_socket(const std::string & path)
{
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket < 0) {
    return -1;
  }
  unlink(path.c_str());
  const sockaddr_un address = unix_address(path);
  if (
    bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
    listen(socket, 32) != 0 || chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
  {
    close(socket);
    unlink(path.c_str());
    return -1;
  }
  return socket;
}

void send_fd(int client_socket, int fd_to_send)
{
  char data = 'M';
  iovec iov{&data, sizeof(data)};
  char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);

  cmsghdr * cmsg = CMSG_FIRSTHDR(&message);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(fd_to_send));
  (void)sendmsg(client_socket, &message, MSG_NOSIGNAL);
}

int receive_fd_from_socket(const std::string & socket_path)
{
  if (socket_path.empty() || socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    throw std::runtime_error("invalid shared-memory broker socket path");
  }
  const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket < 0) {
    throw std::runtime_error("failed to create shared-memory broker client socket");
  }
  const sockaddr_un address = unix_address(socket_path);
  if (connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
    const std::string message = std::strerror(errno);
    close(socket);
    throw std::runtime_error("failed to connect to shared-memory broker: " + message);
  }

  char data = 0;
  iovec iov{&data, sizeof(data)};
  char control[CMSG_SPACE(sizeof(int))]{};
  msghdr message{};
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  const ssize_t received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
  close(socket);
  if (received != 1) {
    throw std::runtime_error("shared-memory broker returned no descriptor");
  }
  cmsghdr * cmsg = CMSG_FIRSTHDR(&message);
  if (
    cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
    cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
  {
    throw std::runtime_error("shared-memory broker returned an invalid control message");
  }
  int fd = -1;
  std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
  if (fd < 0) {
    throw std::runtime_error("shared-memory broker returned an invalid descriptor");
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

}  // namespace

struct ShmemBufferFdBroker::Impl
{
  struct FDDispatcher
  {
    FDDispatcher()
    {
      epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
      if (epoll_fd_ < 0) {
        return;
      }
      event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (event_fd_ < 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
      }
      epoll_event event{};
      event.events = EPOLLIN;
      event.data.fd = event_fd_;
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &event) != 0) {
        close(event_fd_);
        close(epoll_fd_);
        event_fd_ = -1;
        epoll_fd_ = -1;
        return;
      }
      running_.store(true, std::memory_order_release);
      thread_ = std::thread(&FDDispatcher::run, this);
    }

    ~FDDispatcher() {stop();}

    bool add_socket(int server_socket, int fd_to_serve)
    {
      if (!running_.load(std::memory_order_acquire)) {
        return false;
      }
      const int duplicated = dup(fd_to_serve);
      if (duplicated < 0) {
        return false;
      }
      (void)fcntl(duplicated, F_SETFD, FD_CLOEXEC);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_[server_socket] = duplicated;
      }
      epoll_event event{};
      event.events = EPOLLIN;
      event.data.fd = server_socket;
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_socket, &event) != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        sockets_.erase(server_socket);
        close(duplicated);
        return false;
      }
      return true;
    }

    void remove_socket(int server_socket)
    {
      int descriptor = -1;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sockets_.find(server_socket);
        if (it != sockets_.end()) {
          descriptor = it->second;
          sockets_.erase(it);
        }
      }
      if (epoll_fd_ >= 0) {
        (void)epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, server_socket, nullptr);
      }
      close(server_socket);
      if (descriptor >= 0) {
        close(descriptor);
      }
    }

    void stop()
    {
      bool expected = true;
      if (!running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
      }
      if (event_fd_ >= 0) {
        const std::uint64_t wake = 1;
        const ssize_t wake_result = write(event_fd_, &wake, sizeof(wake));
        (void)wake_result;
      }
      if (thread_.joinable()) {
        thread_.join();
      }
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto & item : sockets_) {
        close(item.first);
        close(item.second);
      }
      sockets_.clear();
      if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
      }
      if (epoll_fd_ >= 0) {
        close(epoll_fd_);
        epoll_fd_ = -1;
      }
    }

private:
    void run()
    {
      epoll_event events[16]{};
      while (running_.load(std::memory_order_acquire)) {
        const int count = epoll_wait(epoll_fd_, events, 16, 1000);
        if (count < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        for (int index = 0; index < count; ++index) {
          const int socket = events[index].data.fd;
          if (socket == event_fd_) {
            std::uint64_t ignored = 0;
            const ssize_t read_result = read(event_fd_, &ignored, sizeof(ignored));
            (void)read_result;
            continue;
          }
          int fd_to_serve = -1;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sockets_.find(socket);
            if (it != sockets_.end()) {
              fd_to_serve = it->second;
            }
          }
          if (fd_to_serve < 0) {
            continue;
          }
          const int client = accept4(socket, nullptr, nullptr, SOCK_CLOEXEC);
          if (client >= 0) {
            send_fd(client, fd_to_serve);
            close(client);
          }
        }
      }
    }

    int epoll_fd_{-1};
    int event_fd_{-1};
    std::unordered_map<int, int> sockets_;
    std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> running_{false};
  };

  std::shared_ptr<FDDispatcher> dispatcher{std::make_shared<FDDispatcher>()};

  struct RegisteredBlock
  {
    int server_socket{-1};
    std::string socket_path;
  };

  std::unordered_map<std::uint32_t, RegisteredBlock> registered_blocks;
  std::mutex mutex;
};

ShmemBufferFdBroker::ShmemBufferFdBroker()
: impl_(std::make_unique<Impl>()) {}

ShmemBufferFdBroker::~ShmemBufferFdBroker()
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  for (const auto & item : impl_->registered_blocks) {
    if (impl_->dispatcher != nullptr && item.second.server_socket >= 0) {
      impl_->dispatcher->remove_socket(item.second.server_socket);
    }
    unlink(item.second.socket_path.c_str());
  }
  impl_->registered_blocks.clear();
}

std::string ShmemBufferFdBroker::register_block(ShmemBufferBlock * block)
{
  if (block == nullptr || block->native_handle < 0 || impl_->dispatcher == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto existing = impl_->registered_blocks.find(block->block_id);
  if (existing != impl_->registered_blocks.end()) {
    return existing->second.socket_path;
  }

  // Include the fd in addition to the process and block IDs. A process can
  // own more than one pool, and each pool starts its block IDs at zero.
  const std::string path = "/tmp/shmem_buffer_" + std::to_string(geteuid()) + "_" +
    std::to_string(getpid()) + "_" + std::to_string(block->block_id) +
    "_" + std::to_string(static_cast<int>(block->native_handle)) + ".sock";
  const int server_socket = create_fd_server_socket(path);
  if (
    server_socket < 0 ||
    !impl_->dispatcher->add_socket(server_socket, static_cast<int>(block->native_handle)))
  {
    if (server_socket >= 0) {
      close(server_socket);
    }
    unlink(path.c_str());
    throw std::runtime_error(
      "failed to create shared-memory FD broker socket: " + std::string(std::strerror(errno)));
  }
  impl_->registered_blocks.emplace(block->block_id, Impl::RegisteredBlock{server_socket, path});
  block->socket_path = path;
  return path;
}

ShmemBufferPlatformMapping create_platform_mapping(
  std::size_t mapped_size, std::uint32_t, std::uint64_t)
{
  if (mapped_size > static_cast<std::size_t>(std::numeric_limits<off_t>::max())) {
    throw std::length_error("shared-memory mapping is too large for ftruncate");
  }

  const int fd = create_linux_shared_memory_fd();
  if (fd < 0) {
    throw std::runtime_error("memfd_create failed: " + std::string(std::strerror(errno)));
  }
  if (ftruncate(fd, static_cast<off_t>(mapped_size)) != 0) {
    const std::string message = std::strerror(errno);
    close(fd);
    throw std::runtime_error("ftruncate for shared-memory mapping failed: " + message);
  }

  void * mapping = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    const std::string message = std::strerror(errno);
    close(fd);
    throw std::runtime_error("mmap for shared-memory mapping failed: " + message);
  }
  return ShmemBufferPlatformMapping{fd, mapping, mapped_size, {}};
}

ShmemBufferPlatformMapping import_platform_mapping(
  const std::string & ipc_name,
  std::size_t mapped_size)
{
  const int fd = receive_fd_from_socket(ipc_name);
  struct stat stat_buffer
  {
  };
  if (
    fstat(fd, &stat_buffer) != 0 || stat_buffer.st_size < 0 ||
    static_cast<std::uint64_t>(stat_buffer.st_size) != mapped_size)
  {
    close(fd);
    throw std::runtime_error("shared-memory size does not match descriptor message");
  }
  void * mapping = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    const std::string message = std::strerror(errno);
    close(fd);
    throw std::runtime_error("failed to map received shared-memory FD: " + message);
  }
  return ShmemBufferPlatformMapping{fd, mapping, mapped_size, ipc_name};
}

void destroy_platform_mapping(
  std::intptr_t native_handle, void * mapping, std::size_t mapped_size) noexcept
{
  if (mapping != nullptr && mapped_size > 0) {
    (void)munmap(mapping, mapped_size);
  }
  if (native_handle >= 0) {
    (void)close(static_cast<int>(native_handle));
  }
}

}  // namespace shmem_buffer
