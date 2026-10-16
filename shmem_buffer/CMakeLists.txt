cmake_minimum_required(VERSION 3.20)
project(shmem_buffer)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR
    "shmem_buffer supports Linux and Windows only (CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}').")
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rosidl_buffer REQUIRED)
find_package(shmem_buffer_backend_msgs REQUIRED)
find_package(rmw REQUIRED)
find_package(rcutils REQUIRED)

set(shmem_buffer_sources
  src/shmem_buffer.cpp
  src/shmem_buffer_impl.cpp
  src/shmem_buffer_memory_pool.cpp
  src/host_endpoint_manager.cpp
  src/shmem_buffer_ipc_manager.cpp
)
if(WIN32)
  list(APPEND shmem_buffer_sources src/shmem_buffer_platform_windows.cpp)
else()
  list(APPEND shmem_buffer_sources src/shmem_buffer_platform_linux.cpp)
endif()

add_library(${PROJECT_NAME} SHARED ${shmem_buffer_sources})

target_compile_definitions(${PROJECT_NAME} PRIVATE SHMEM_BUFFER_BUILDING_DLL)

target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>
)

target_link_libraries(${PROJECT_NAME}
  PUBLIC
    rosidl_buffer::rosidl_buffer
    rmw::rmw
    rcutils::rcutils
    ${shmem_buffer_backend_msgs_TARGETS}
)
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE advapi32)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE rt)
endif()

install(
  DIRECTORY include/
  DESTINATION include/${PROJECT_NAME}
)

install(
  TARGETS ${PROJECT_NAME}
  EXPORT ${PROJECT_NAME}
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include/${PROJECT_NAME}
)

ament_export_targets(${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(rosidl_buffer shmem_buffer_backend_msgs rmw rcutils)
ament_export_libraries(${PROJECT_NAME})
ament_export_include_directories(include/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)

  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_shmem_buffer
    test/test_shmem_buffer.cpp
  )
  if(TARGET test_shmem_buffer)
    target_link_libraries(test_shmem_buffer
      ${PROJECT_NAME}
      rosidl_buffer::rosidl_buffer
    )
    if(NOT WIN32)
      target_link_libraries(test_shmem_buffer rt)
    endif()
  endif()

endif()

ament_package()
