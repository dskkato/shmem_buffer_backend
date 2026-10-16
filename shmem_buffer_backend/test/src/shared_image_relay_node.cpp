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

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "rclcpp/node.hpp"
#include "rclcpp/node_options.hpp"
#include "rclcpp/publisher.hpp"
#include "rclcpp/subscription.hpp"
#include "rclcpp/subscription_options.hpp"
#include "rclcpp_components/register_node_macro.hpp"
#include "sensor_msgs/msg/image.hpp"

/// A zero-copy forwarding node: it subscribes to a shared-buffer-backed image,
/// creates a new Image message, and moves the data buffer into it. The backend
/// serializes the imported buffer's original IPC descriptor instead of copying
/// the payload.
class SharedImageRelay : public rclcpp::Node
{
public:
  explicit SharedImageRelay(const rclcpp::NodeOptions & options)
  : Node("shared_image_relay", options)
  {
    const auto input_topic = declare_parameter<std::string>(
      "input_topic", "test_shared_buffer_image_dso");
    const auto output_topic = declare_parameter<std::string>(
      "output_topic", "test_shared_buffer_image_dso_relay");

    rclcpp::SubscriptionOptions subscription_options;
    subscription_options.acceptable_buffer_backends = "shared_buffer";
    subscription_ = create_subscription<sensor_msgs::msg::Image>(
      input_topic, 10,
      std::bind(&SharedImageRelay::image_callback, this, std::placeholders::_1),
      subscription_options);
    publisher_ = create_publisher<sensor_msgs::msg::Image>(output_topic, 10);
  }

private:
  void image_callback(sensor_msgs::msg::Image::UniquePtr msg)
  {
    // Allocate a different message instance and copy only its metadata. The
    // Buffer move transfers the imported backend implementation; using a copy
    // assignment here would clone the payload and lose zero-copy forwarding.
    auto forwarded = std::make_unique<sensor_msgs::msg::Image>();
    forwarded->header = msg->header;
    forwarded->height = msg->height;
    forwarded->width = msg->width;
    forwarded->encoding = msg->encoding;
    forwarded->is_bigendian = msg->is_bigendian;
    forwarded->step = msg->step;
    forwarded->header.frame_id = "relayed/" + msg->header.frame_id;
    forwarded->data = std::move(msg->data);
    publisher_->publish(std::move(forwarded));
  }

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr subscription_;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr publisher_;
};

RCLCPP_COMPONENTS_REGISTER_NODE(SharedImageRelay)
