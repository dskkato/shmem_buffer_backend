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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "shared_buffer/shared_buffer_api.hpp"
#include "rclcpp/node.hpp"
#include "rclcpp/node_options.hpp"
#include "rclcpp/publisher.hpp"
#include "rclcpp/timer.hpp"
#include "rclcpp_components/register_node_macro.hpp"
#include "sensor_msgs/msg/image.hpp"

class SharedImageDsoPublisher : public rclcpp::Node
{
public:
  explicit SharedImageDsoPublisher(const rclcpp::NodeOptions & options)
  : Node("shared_buffer_image_dso_publisher", options)
  {
    published_messages_.reserve(kMessages);
    publisher_ = create_publisher<sensor_msgs::msg::Image>("test_shared_buffer_image_dso", 10);
    timer_ = create_wall_timer(
      std::chrono::milliseconds(100),
      std::bind(&SharedImageDsoPublisher::publish_next, this));
  }

private:
  static constexpr std::size_t kMessages = 5;
  static constexpr std::size_t kPayloadSize = 4096;

  static std::uint8_t pattern_for(std::size_t sequence)
  {
    return static_cast<std::uint8_t>((sequence * 37u + 11u) & 0xffu);
  }

  void publish_next()
  {
    if (sequence_ >= kMessages) {
      timer_->cancel();
      return;
    }
    if (publisher_->get_subscription_count() == 0u) {
      return;
    }

    const std::size_t sequence = sequence_ + 1u;
    const std::uint8_t pattern = pattern_for(sequence);

    sensor_msgs::msg::Image msg;
    msg.header.stamp = now();
    msg.header.frame_id = "shared_buffer_pool_dso_" + std::to_string(sequence);
    msg.height = 16;
    msg.width = 256;
    msg.encoding = "mono8";
    msg.is_bigendian = 0;
    msg.step = 256;
    msg.data = shared_buffer::allocate_buffer(kPayloadSize);

    {
      auto write = shared_buffer::from_output_buffer(msg.data);
      for (std::size_t i = 0; i < kPayloadSize; ++i) {
        write.get_ptr()[i] = pattern;
      }
    }

    // Keep each mapping alive until the test finishes.  A short-period UDP
    // test can otherwise recycle the block while an older descriptor is
    // still in flight, which is intentionally rejected by the UID check.
    published_messages_.push_back(std::move(msg));
    publisher_->publish(published_messages_.back());
    sequence_ = sequence;
  }

  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::vector<sensor_msgs::msg::Image> published_messages_;
  std::size_t sequence_{0};
};

RCLCPP_COMPONENTS_REGISTER_NODE(SharedImageDsoPublisher)
