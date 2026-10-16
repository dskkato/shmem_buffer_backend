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

#ifndef SHARED_BUFFER_BACKEND__SHARED_BUFFER_BACKEND_HPP_
#define SHARED_BUFFER_BACKEND__SHARED_BUFFER_BACKEND_HPP_

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shared_buffer/host_endpoint_manager.hpp"
#include "rosidl_buffer_backend/buffer_backend.hpp"

namespace shared_buffer
{

class SharedBufferBackend : public rosidl::BufferBackend
{
public:
  SharedBufferBackend();
  ~SharedBufferBackend() override = default;

  std::string get_backend_type() const override {return "shared_buffer";}

  std::string get_backend_metadata() const override;

  const rosidl_message_type_support_t * get_descriptor_type_support() const override;

  std::shared_ptr<void> create_empty_descriptor() const override;

  std::shared_ptr<void> create_descriptor_with_endpoint(
    const void * impl, const rmw_topic_endpoint_info_t & endpoint_info) const override;

  std::unique_ptr<void, void (*)(void *)> from_descriptor_with_endpoint(
    const void * descriptor, const rmw_topic_endpoint_info_t & endpoint_info) const override;

  void on_creating_endpoint(const rmw_topic_endpoint_info_t & endpoint_info) const override;

  std::pair<bool, std::vector<std::set<std::uint32_t>>> on_discovering_endpoint(
    const rmw_topic_endpoint_info_t & endpoint_info,
    const std::vector<rmw_topic_endpoint_info_t> & existing_endpoints,
    const std::unordered_map<std::string, std::string> & endpoint_supported_backends) override;

private:
  struct GidKey
  {
    std::array<std::uint8_t, RMW_GID_STORAGE_SIZE> data{};

    explicit GidKey(const std::uint8_t * raw)
    {
      std::copy(raw, raw + RMW_GID_STORAGE_SIZE, data.begin());
    }

    bool operator==(const GidKey & other) const {return data == other.data;}
  };

  struct GidKeyHash
  {
    std::size_t operator()(const GidKey & key) const
    {
      std::size_t result = 0;
      for (const auto byte : key.data) {
        result = result * 131u + byte;
      }
      return result;
    }
  };

  std::shared_ptr<HostEndpointManager> get_endpoint_manager() const;

  mutable std::shared_ptr<HostEndpointManager> endpoint_manager_;
  mutable std::mutex manager_mutex_;
  mutable std::unordered_map<GidKey, bool, GidKeyHash> compatibility_cache_;
  mutable std::mutex compatibility_mutex_;
};

}  // namespace shared_buffer

#endif  // SHARED_BUFFER_BACKEND__SHARED_BUFFER_BACKEND_HPP_
