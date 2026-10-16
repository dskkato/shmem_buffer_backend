cmake_minimum_required(VERSION 3.20)
project(shmem_buffer_backend_py)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR
    "shmem_buffer_backend_py supports Linux and Windows only "
    "(CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}').")
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_python REQUIRED)
find_package(shmem_buffer REQUIRED)
find_package(rosidl_buffer REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
find_package(pybind11 REQUIRED)

pybind11_add_module(_shmem_buffer_py
  src/shmem_buffer_py.cpp
)
target_link_libraries(_shmem_buffer_py PRIVATE
  shmem_buffer::shmem_buffer
  rosidl_buffer::rosidl_buffer
)

# Install the Python module provided by this ROS package.
ament_python_install_package(shmem_buffer)

install(TARGETS _shmem_buffer_py
  LIBRARY DESTINATION "${PYTHON_INSTALL_DIR}/shmem_buffer"
  RUNTIME DESTINATION "${PYTHON_INSTALL_DIR}/shmem_buffer"
)

if(BUILD_TESTING)
  find_package(ament_cmake_pytest REQUIRED)

  find_package(ament_lint_auto REQUIRED)

  ament_lint_auto_find_test_dependencies()

  ament_add_pytest_test(test_shmem_buffer_py
    test/test_shmem_buffer_py.py
    # This is a standalone pytest test. Avoid loading unrelated ROS pytest
    # plugins, whose launch_testing hook versions may not match in CI.
    ENV PYTEST_DISABLE_PLUGIN_AUTOLOAD=1
  )
endif()

ament_export_dependencies(shmem_buffer rosidl_buffer)
ament_package()
