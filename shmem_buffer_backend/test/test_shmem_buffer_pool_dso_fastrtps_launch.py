#!/usr/bin/env python3
# Copyright 2026 Daisuke Kato
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import time
import unittest

from launch import LaunchDescription
from launch.actions import SetEnvironmentVariable
from launch.actions import TimerAction
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode
import launch_testing
import launch_testing.actions
import launch_testing.asserts
import launch_testing.markers
from launch_testing_ros.actions import EnableRmwIsolation
import pytest
import rclpy
from std_msgs.msg import Bool, UInt32


@pytest.mark.launch_test
@launch_testing.markers.keep_alive
def generate_test_description():
    """Verify shmem_buffer pool sharing between a component and the backend plugin."""
    test_domain_id = str(100 + os.getpid() % 100)
    subscriber_container = ComposableNodeContainer(
        name='shmem_buffer_image_dso_subscriber_container',
        namespace='',
        package='rclcpp_components',
        executable='component_container',
        composable_node_descriptions=[
            ComposableNode(
                package='shmem_buffer_backend',
                plugin='SharedImageSubscriber',
                name='shmem_buffer_image_subscriber',
            ),
        ],
        output='screen',
    )

    relay_container = ComposableNodeContainer(
        name='shmem_buffer_image_dso_relay_container',
        namespace='',
        package='rclcpp_components',
        executable='component_container',
        composable_node_descriptions=[
            ComposableNode(
                package='shmem_buffer_backend',
                plugin='SharedImageRelay',
                name='shmem_buffer_image_relay',
                parameters=[{
                    'input_topic': 'test_shmem_buffer_image_dso',
                    'output_topic': 'test_shmem_buffer_image_dso_relay',
                }],
            ),
        ],
        output='screen',
    )

    relay_subscriber_container = ComposableNodeContainer(
        name='shmem_buffer_image_dso_relay_subscriber_container',
        namespace='',
        package='rclcpp_components',
        executable='component_container',
        composable_node_descriptions=[
            ComposableNode(
                package='shmem_buffer_backend',
                plugin='SharedImageSubscriber',
                name='shmem_buffer_image_relay_subscriber',
                parameters=[{
                    'input_topic': 'test_shmem_buffer_image_dso_relay',
                    'result_prefix': 'shmem_buffer_relay_dso',
                    'frame_id_prefix': 'relayed/shmem_buffer_pool_dso_',
                }],
            ),
        ],
        output='screen',
    )

    publisher_container = ComposableNodeContainer(
        name='shmem_buffer_image_dso_publisher_container',
        namespace='',
        package='rclcpp_components',
        executable='component_container',
        composable_node_descriptions=[
            ComposableNode(
                package='shmem_buffer_backend',
                plugin='SharedImageDsoPublisher',
                name='shmem_buffer_image_dso_publisher',
            ),
        ],
        output='screen',
    )

    return LaunchDescription([
        SetEnvironmentVariable('RMW_IMPLEMENTATION', 'rmw_fastrtps_cpp'),
        SetEnvironmentVariable('ROS_DOMAIN_ID', test_domain_id),
        EnableRmwIsolation(),
        subscriber_container,
        relay_container,
        relay_subscriber_container,
        TimerAction(period=1.0, actions=[publisher_container]),
        launch_testing.actions.ReadyToTest(),
    ])


class TestShmemBufferPoolDsoFastRTPS(unittest.TestCase):
    """Verify descriptor transport when allocation and serialization use DSOs."""

    @classmethod
    def setUpClass(cls):
        rclpy.init()

    @classmethod
    def tearDownClass(cls):
        rclpy.shutdown()

    def setUp(self):
        self.node = rclpy.create_node('test_shmem_buffer_pool_dso_fastrtps')
        self.received_count = 0
        self.relay_received_count = 0
        self.validation_count = 0
        self.backend_count = 0
        self.content_count = 0
        self.metadata_count = 0
        self.all_valid = True

        self.node.create_subscription(
            UInt32, 'shmem_buffer_dso_subscriber_count', self._count_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_dso_validation', self._validation_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_dso_backend_validation', self._backend_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_dso_content_validation', self._content_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_dso_metadata_validation', self._metadata_cb, 10)
        self.node.create_subscription(
            UInt32, 'shmem_buffer_relay_dso_subscriber_count', self._relay_count_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_relay_dso_validation', self._validation_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_relay_dso_backend_validation', self._backend_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_relay_dso_content_validation', self._content_cb, 10)
        self.node.create_subscription(
            Bool, 'shmem_buffer_relay_dso_metadata_validation', self._metadata_cb, 10)

    def tearDown(self):
        self.node.destroy_node()

    def _count_cb(self, msg):
        self.received_count = msg.data

    def _relay_count_cb(self, msg):
        self.relay_received_count = msg.data

    def _validation_cb(self, msg):
        self.validation_count += 1
        self.all_valid = self.all_valid and msg.data

    def _backend_cb(self, msg):
        self.backend_count += 1
        self.all_valid = self.all_valid and msg.data

    def _content_cb(self, msg):
        self.content_count += 1
        self.all_valid = self.all_valid and msg.data

    def _metadata_cb(self, msg):
        self.metadata_count += 1
        self.all_valid = self.all_valid and msg.data

    def _complete(self):
        return (
            self.received_count >= 5 and
            self.relay_received_count >= 5 and
            self.validation_count >= 10 and
            self.backend_count >= 10 and
            self.content_count >= 10 and
            self.metadata_count >= 10
        )

    def test_five_shmem_buffer_messages_validate(self):
        """Require explicit shmem_buffer, metadata, and payload validation."""
        deadline = time.monotonic() + 30.0
        while not self._complete() and time.monotonic() < deadline:
            rclpy.spin_once(self.node, timeout_sec=0.1)

        self.assertTrue(
            self._complete(),
            'Did not observe five complete validations: '
            f'received={self.received_count}, relay_received={self.relay_received_count}, '
            f'validation={self.validation_count}, '
            f'backend={self.backend_count}, content={self.content_count}, '
            f'metadata={self.metadata_count}',
        )
        self.assertTrue(self.all_valid, 'At least one shmem_buffer DSO validation failed')


@launch_testing.post_shutdown_test()
class TestShmemBufferPoolDsoFastRTPSShutdown(unittest.TestCase):
    """Check that both component containers shut down cleanly."""

    def test_exit_codes(self, proc_info):
        allowable_exit_codes = [0, -2, -15]
        if os.name == 'nt':
            # CTRL_C_EVENT can be reported as either signed or unsigned
            # STATUS_CONTROL_C_EXIT depending on the Python launcher layer.
            # component_container reports 1 after launch_testing escalates
            # SIGINT to SIGTERM on Windows.
            allowable_exit_codes = [0, 1, -1073741510, 3221225786]
        launch_testing.asserts.assertExitCodes(
            proc_info,
            allowable_exit_codes=allowable_exit_codes,
        )
