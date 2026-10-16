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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <vector>

#include "shared_buffer/shared_buffer_api.hpp"
#include "rclcpp/logging.hpp"
#include "rclcpp/node.hpp"
#include "rclcpp/node_options.hpp"
#include "rclcpp/publisher.hpp"
#include "rclcpp/subscription.hpp"
#include "rclcpp/subscription_options.hpp"
#include "rclcpp_components/register_node_macro.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/u_int32.hpp"

class SharedImageSubscriber : public rclcpp::Node
{
public:
  explicit SharedImageSubscriber(const rclcpp::NodeOptions & options)
  : Node("shared_image_subscriber", options)
  {
    const auto input_topic = declare_parameter<std::string>(
      "input_topic", "test_shared_buffer_image_dso");
    const auto result_prefix = declare_parameter<std::string>(
      "result_prefix", "shared_buffer_dso");
    frame_id_prefix_ = declare_parameter<std::string>(
      "frame_id_prefix", "shared_buffer_pool_dso_");

    rclcpp::SubscriptionOptions subscription_options;
    // Reject CPU fallback at endpoint matching time.  The test must fail if
    // descriptor creation in the publisher process did not find its block.
    subscription_options.acceptable_buffer_backends = "shared_buffer";
    subscription_ = create_subscription<sensor_msgs::msg::Image>(
      input_topic, 10,
      std::bind(&SharedImageSubscriber::image_callback, this, std::placeholders::_1),
      subscription_options);

    count_publisher_ = create_publisher<std_msgs::msg::UInt32>(
      result_prefix + "_subscriber_count", 10);
    validation_publisher_ = create_publisher<std_msgs::msg::Bool>(
      result_prefix + "_validation", 10);
    backend_publisher_ =
      create_publisher<std_msgs::msg::Bool>(result_prefix + "_backend_validation", 10);
    content_publisher_ =
      create_publisher<std_msgs::msg::Bool>(result_prefix + "_content_validation", 10);
    metadata_publisher_ = create_publisher<std_msgs::msg::Bool>(
      result_prefix + "_metadata_validation", 10);
  }

private:
  static std::uint8_t pattern_for(std::size_t sequence)
  {
    return static_cast<std::uint8_t>((sequence * 37u + 11u) & 0xffu);
  }

  void image_callback(std::shared_ptr<const sensor_msgs::msg::Image> msg)
  {
    ++received_count_;
    bool metadata_valid = msg->header.frame_id ==
      frame_id_prefix_ + std::to_string(received_count_);
    metadata_valid = metadata_valid && msg->height == 16u && msg->width == 256u;
    metadata_valid = metadata_valid && msg->encoding == "mono8" && msg->step == 256u;
    metadata_valid = metadata_valid && msg->data.size() == 4096u;

    const bool backend_valid = msg->data.get_backend_type() == "shared_buffer";
    bool content_valid = false;
    try {
      // Check the backend before acquiring the handle so CPU fallback cannot
      // be silently promoted back to shared_buffer by from_input_buffer().
      if (backend_valid) {
        const auto read = shared_buffer::from_input_buffer(msg->data);
        std::vector<std::uint8_t> data(msg->data.size());
        std::memcpy(data.data(), read.get_ptr(), data.size());
        const auto expected = pattern_for(received_count_);
        content_valid = !data.empty();
        for (const auto value : data) {
          if (value != expected) {
            content_valid = false;
            break;
          }
        }
      }
    } catch (const std::exception & exception) {
      RCLCPP_ERROR(get_logger(), "Failed to read shared_buffer payload: %s", exception.what());
    }

    const bool message_valid = metadata_valid && backend_valid && content_valid;
    validation_passed_ = validation_passed_ && message_valid;
    publish_result(message_valid, backend_valid, content_valid, metadata_valid);
  }

  void publish_result(bool valid, bool backend_valid, bool content_valid, bool metadata_valid)
  {
    std_msgs::msg::UInt32 count;
    count.data = received_count_;
    count_publisher_->publish(count);

    std_msgs::msg::Bool validation;
    validation.data = validation_passed_;
    validation_publisher_->publish(validation);

    std_msgs::msg::Bool backend;
    backend.data = backend_valid;
    backend_publisher_->publish(backend);

    std_msgs::msg::Bool content;
    content.data = content_valid;
    content_publisher_->publish(content);

    std_msgs::msg::Bool metadata;
    metadata.data = metadata_valid;
    metadata_publisher_->publish(metadata);

    if (!valid) {
      RCLCPP_ERROR(get_logger(), "Invalid shared_buffer image #%u", received_count_);
    }
  }

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr subscription_;
  rclcpp::Publisher<std_msgs::msg::UInt32>::SharedPtr count_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr validation_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr backend_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr content_publisher_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr metadata_publisher_;
  std::uint32_t received_count_{0};
  bool validation_passed_{true};
  std::string frame_id_prefix_;
};

RCLCPP_COMPONENTS_REGISTER_NODE(SharedImageSubscriber)
