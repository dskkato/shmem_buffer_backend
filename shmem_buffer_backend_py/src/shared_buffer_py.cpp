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

#include <pybind11/pybind11.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "shared_buffer/shared_buffer_api.hpp"
#include "rosidl_buffer/buffer.hpp"

namespace py = pybind11;

namespace shared_buffer
{
namespace
{

/// \brief A helper wrapper around `_get_buffer_ptr`.
///
/// Returns a raw pointer to the underlying `rosidl::Buffer<uint8_t>`.
/// Ownership is not transferred; the returned pointer is borrowed and remains
/// valid only while `owner` keeps the corresponding Python buffer object alive.
///
/// Ideally, this would access the underlying buffer through a public C++ API,
/// such as `PyBuffer::get_raw_buffer()`. However, `PyBuffer` is currently an
/// internal implementation detail and is not available to downstream
/// extensions. As a workaround, this function calls the Python-exposed
/// `_get_buffer_ptr` helper and converts the returned address back to the
/// underlying C++ buffer pointer.
rosidl::Buffer<std::uint8_t> * get_buffer_ptr(const py::object & owner)
{
  py::object get_pointer = py::module_::import("rosidl_buffer").attr("_get_buffer_ptr");
  const std::uintptr_t address = get_pointer(owner).cast<std::uintptr_t>();
  if (address == 0) {
    throw std::invalid_argument("rosidl buffer pointer must not be null");
  }
  return reinterpret_cast<rosidl::Buffer<std::uint8_t> *>(address);
}

/// \brief A helper wrapper around `_take_buffer_from_ptr`.
///
/// Transfers ownership of a heap-allocated `rosidl::Buffer<uint8_t>` to the
/// corresponding Python buffer object.
///
/// The buffer must be owned by a `std::unique_ptr` using the default deleter,
/// since `_take_buffer_from_ptr` assumes ownership and will eventually destroy
/// the buffer with `delete`.
///
/// Ideally, ownership would be transferred through a public C++ API, such as a
/// `PyBuffer` constructor or factory function accepting a smart pointer.
/// However, no such API is currently available to downstream extensions.
/// As a workaround, this function passes the raw address to the Python-exposed
/// `_take_buffer_from_ptr` helper and releases the `unique_ptr` only after the
/// Python object has successfully taken ownership.
py::object take_buffer_from_ptr(std::unique_ptr<rosidl::Buffer<std::uint8_t>> buffer_ptr)
{
  py::object take_buffer = py::module_::import("rosidl_buffer").attr("_take_buffer_from_ptr");

  py::object result = take_buffer(py::int_(reinterpret_cast<std::uintptr_t>(buffer_ptr.get())));

  // Ownership has been transferred to the returned Python buffer object.
  [[maybe_unused]] auto * released_buffer = buffer_ptr.release();

  return result;
}

py::object allocate_buffer_internal(std::size_t byte_count)
{
  if (byte_count == 0) {
    throw py::value_error("byte_count must be greater than zero");
  }
  if (byte_count > static_cast<std::size_t>(std::numeric_limits<Py_ssize_t>::max())) {
    throw std::overflow_error("byte_count exceeds Python buffer protocol limits");
  }

  auto buffer = std::make_unique<rosidl::Buffer<std::uint8_t>>(allocate_buffer(byte_count));

  return take_buffer_from_ptr(std::move(buffer));
}

class NativeReadAccess
{
public:
  explicit NativeReadAccess(py::object owner)
  : owner_(std::move(owner))
  {
    buffer_ = get_buffer_ptr(owner_);
    byte_count_ = buffer_->size();
    if (byte_count_ > static_cast<std::size_t>(std::numeric_limits<Py_ssize_t>::max())) {
      throw std::overflow_error("buffer size exceeds Python buffer protocol limits");
    }
    handle_ = from_input_buffer(*buffer_);
  }

  NativeReadAccess(const NativeReadAccess &) = delete;
  NativeReadAccess & operator=(const NativeReadAccess &) = delete;

  py::buffer_info buffer_info()
  {
    return py::buffer_info(handle_.get_ptr(), static_cast<Py_ssize_t>(byte_count_), true);
  }

private:
  py::object owner_;
  rosidl::Buffer<std::uint8_t> * buffer_{nullptr};
  ReadHandle handle_;
  std::size_t byte_count_{0};
};

class NativeWriteAccess
{
public:
  explicit NativeWriteAccess(py::object owner)
  : owner_(std::move(owner))
  {
    buffer_ = get_buffer_ptr(owner_);
    byte_count_ = buffer_->size();
    if (byte_count_ > static_cast<std::size_t>(std::numeric_limits<Py_ssize_t>::max())) {
      throw std::overflow_error("buffer size exceeds Python buffer protocol limits");
    }
    handle_ = from_output_buffer(*buffer_);
  }

  NativeWriteAccess(const NativeWriteAccess &) = delete;
  NativeWriteAccess & operator=(const NativeWriteAccess &) = delete;

  py::buffer_info buffer_info()
  {
    if (closed_) {
      throw py::buffer_error("write access is closed");
    }
    return py::buffer_info(handle_.get_ptr(), static_cast<Py_ssize_t>(byte_count_), false);
  }

  void close()
  {
    if (exports_ != 0) {
      throw py::buffer_error("cannot close write access while exported views are alive");
    }
    if (!closed_) {
      handle_ = WriteHandle();
      owner_ = py::none();
      buffer_ = nullptr;
      closed_ = true;
    }
  }

  bool closed() const {return closed_;}
  std::size_t byte_count() const {return byte_count_;}
  void add_export() {++exports_;}
  void remove_export() noexcept
  {
    if (exports_ != 0) {
      --exports_;
    }
  }

private:
  py::object owner_;
  rosidl::Buffer<std::uint8_t> * buffer_{nullptr};
  WriteHandle handle_;
  std::size_t byte_count_{0};
  std::size_t exports_{0};
  bool closed_{false};
};

struct WriteBufferCallbacks
{
  // Write access must be finalized at context-manager exit, so it still needs
  // to reject outstanding exports. Read access instead follows the exporter's
  // normal object lifetime and needs no manual export tracking.
  static getbufferproc original_getbuffer;
  static releasebufferproc original_releasebuffer;

  static int getbuffer(PyObject * object, Py_buffer * view, int flags)
  {
    const int result = original_getbuffer(object, view, flags);
    if (result == 0) {
      try {
        py::cast<NativeWriteAccess &>(py::handle(object)).add_export();
      } catch (...) {
        original_releasebuffer(object, view);
        PyErr_SetString(PyExc_BufferError, "failed to retain shared-buffer export");
        return -1;
      }
    }
    return result;
  }

  static void releasebuffer(PyObject * object, Py_buffer * view)
  {
    try {
      py::cast<NativeWriteAccess &>(py::handle(object)).remove_export();
    } catch (...) {
      PyErr_Clear();
    }
    original_releasebuffer(object, view);
  }
};

getbufferproc WriteBufferCallbacks::original_getbuffer = nullptr;
releasebufferproc WriteBufferCallbacks::original_releasebuffer = nullptr;

void install_tracked_write_buffer_callbacks(const py::object & type_object)
{
  auto * type = reinterpret_cast<PyTypeObject *>(type_object.ptr());
  if (
    type->tp_as_buffer == nullptr || type->tp_as_buffer->bf_getbuffer == nullptr ||
    type->tp_as_buffer->bf_releasebuffer == nullptr)
  {
    throw std::runtime_error("pybind11 did not install buffer protocol callbacks");
  }
  WriteBufferCallbacks::original_getbuffer = type->tp_as_buffer->bf_getbuffer;
  WriteBufferCallbacks::original_releasebuffer = type->tp_as_buffer->bf_releasebuffer;
  type->tp_as_buffer->bf_getbuffer = &WriteBufferCallbacks::getbuffer;
  type->tp_as_buffer->bf_releasebuffer = &WriteBufferCallbacks::releasebuffer;
}

}  // namespace
}  // namespace shared_buffer

PYBIND11_MODULE(_shared_buffer_py, module)
{
  using shared_buffer::NativeReadAccess;
  using shared_buffer::NativeWriteAccess;

  module.doc() = "Python buffer-protocol access to shared-memory rosidl buffers";

  py::class_<NativeReadAccess> read_class(module, "_NativeReadAccess", py::buffer_protocol());
  read_class.def(py::init<py::object>())
  .def_buffer(&NativeReadAccess::buffer_info);

  py::class_<NativeWriteAccess> write_class(module, "_NativeWriteAccess", py::buffer_protocol());
  write_class.def(py::init<py::object>())
  .def_buffer(&NativeWriteAccess::buffer_info)
  .def("close", &NativeWriteAccess::close)
  .def_property_readonly("closed", &NativeWriteAccess::closed)
  .def_property_readonly("byte_count", &NativeWriteAccess::byte_count);

  shared_buffer::install_tracked_write_buffer_callbacks(write_class);

  module.def(
    "allocate_buffer", &shared_buffer::allocate_buffer_internal, py::arg("byte_count"));
}
