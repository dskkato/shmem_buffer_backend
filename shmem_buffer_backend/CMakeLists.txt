cmake_minimum_required(VERSION 3.20)
project(shmem_buffer_backend)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR
    "shmem_buffer_backend supports Linux and Windows only "
    "(CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}').")
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake_auto REQUIRED)
ament_auto_find_build_dependencies()

ament_auto_add_library(${PROJECT_NAME} SHARED
  src/shmem_buffer_backend_plugin.cpp
)
set_target_properties(${PROJECT_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>
)

target_link_libraries(${PROJECT_NAME}
  ${shmem_buffer_backend_msgs_TARGETS}
  rosidl_buffer_backend::rosidl_buffer_backend
  rosidl_runtime_cpp::rosidl_runtime_cpp
  rosidl_buffer_backend_registry::rosidl_buffer_backend_registry
  rosidl_typesupport_cpp::rosidl_typesupport_cpp
  pluginlib::pluginlib
  shmem_buffer::shmem_buffer
  rcutils::rcutils
)

pluginlib_export_plugin_description_file(rosidl_buffer_backend shmem_buffer_plugin.xml)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)

  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(rclcpp_components REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(launch_testing_ament_cmake REQUIRED)

  ament_add_gtest(test_shmem_buffer_backend
    test/test_shmem_buffer_backend.cpp
  )
  if(TARGET test_shmem_buffer_backend)
    target_link_libraries(test_shmem_buffer_backend
      ${PROJECT_NAME}
      shmem_buffer::shmem_buffer
      ${shmem_buffer_backend_msgs_TARGETS}
      rosidl_buffer_backend::rosidl_buffer_backend
      rosidl_runtime_cpp::rosidl_runtime_cpp
      rosidl_typesupport_cpp::rosidl_typesupport_cpp
    )
  endif()

  ament_auto_add_library(shared_image_dso_publisher_component SHARED
    NO_TARGET_LINK_LIBRARIES
    test/src/shared_image_dso_publisher_node.cpp
  )
  set_target_properties(shared_image_dso_publisher_component PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
  target_link_libraries(shared_image_dso_publisher_component
    shmem_buffer::shmem_buffer
    rclcpp::rclcpp
    rclcpp_components::component
    ${sensor_msgs_TARGETS}
    rosidl_runtime_cpp::rosidl_runtime_cpp
  )
  rclcpp_components_register_nodes(shared_image_dso_publisher_component
    "SharedImageDsoPublisher")

  ament_auto_add_library(shared_image_subscriber_component SHARED
    NO_TARGET_LINK_LIBRARIES
    test/src/shared_image_subscriber_node.cpp
  )
  target_link_libraries(shared_image_subscriber_component
    shmem_buffer::shmem_buffer
    rclcpp::rclcpp
    rclcpp_components::component
    ${sensor_msgs_TARGETS}
    ${std_msgs_TARGETS}
    rosidl_runtime_cpp::rosidl_runtime_cpp
  )
  rclcpp_components_register_nodes(shared_image_subscriber_component
    "SharedImageSubscriber")

  ament_auto_add_library(shared_image_relay_component SHARED
    NO_TARGET_LINK_LIBRARIES
    test/src/shared_image_relay_node.cpp
  )
  target_link_libraries(shared_image_relay_component
    shmem_buffer::shmem_buffer
    rclcpp::rclcpp
    rclcpp_components::component
    ${sensor_msgs_TARGETS}
    rosidl_runtime_cpp::rosidl_runtime_cpp
  )
  rclcpp_components_register_nodes(shared_image_relay_component
    "SharedImageRelay")

  add_launch_test(test/test_shmem_buffer_pool_dso_fastrtps_launch.py
    TARGET test_shmem_buffer_pool_dso_fastrtps_launch
    TIMEOUT 45
    ENV "ROS_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/ros_log"
  )

endif()

ament_auto_package(USE_SCOPED_HEADER_INSTALL_DIR)
