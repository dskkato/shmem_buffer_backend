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

import ctypes
import gc

import numpy
import pytest
from rosidl_buffer import Buffer
from sensor_msgs.msg import Image
from shared_buffer import allocate_buffer
from shared_buffer import read_buffer
from shared_buffer import write_buffer


def test_allocate_and_write_with_memoryview() -> None:
    buffer = allocate_buffer(16)

    assert isinstance(buffer, Buffer)
    assert buffer.backend_type == 'shared_buffer'
    assert len(buffer) == 16

    with write_buffer(buffer) as view:
        assert view.format == 'B'
        assert view.ndim == 1
        assert view.shape == (16,)
        assert not view.readonly
        view[:] = bytes(range(16))

    assert buffer.to_bytes() == bytes(range(16))


def test_numpy_view_is_zero_copy() -> None:
    buffer = allocate_buffer(32)

    with write_buffer(buffer) as view:
        array = numpy.frombuffer(view, dtype=numpy.uint8)
        view_address = ctypes.addressof(ctypes.c_uint8.from_buffer(view))
        assert array.__array_interface__['data'][0] == view_address
        array[:] = numpy.arange(32, dtype=numpy.uint8)
        del array

    view = read_buffer(buffer)
    array = numpy.frombuffer(view, dtype=numpy.uint8)
    assert isinstance(view, memoryview)
    assert view.readonly
    assert not array.flags.writeable
    assert array.tolist() == list(range(32))


def test_ros_message_field_retains_shared_buffer() -> None:
    buffer = allocate_buffer(8)
    with write_buffer(buffer) as view:
        view[:] = b'ros2data'

    message = Image()
    message.data = buffer

    assert message.data is buffer
    assert message.data.backend_type == 'shared_buffer'
    view = read_buffer(message.data)
    assert view.tobytes() == b'ros2data'


def test_numpy_view_keeps_read_lease_alive() -> None:
    buffer = allocate_buffer(8)
    with write_buffer(buffer) as view:
        view[:] = b'lifetime'

    view = read_buffer(buffer)
    array = numpy.frombuffer(view, dtype=numpy.uint8)
    del view
    del buffer
    gc.collect()

    assert array.tobytes() == b'lifetime'


def test_access_keeps_buffer_alive() -> None:
    buffer = allocate_buffer(8)
    access = write_buffer(buffer)
    del buffer
    gc.collect()

    with access as view:
        view[:] = b'abcdefgh'


def test_exported_numpy_view_prevents_close() -> None:
    buffer = allocate_buffer(8)
    access = write_buffer(buffer)
    view = access.__enter__()
    array = numpy.frombuffer(view, dtype=numpy.uint8)

    with pytest.raises(BufferError, match='exported views'):
        access.close()
    assert not access.closed

    del array
    gc.collect()
    access.close()
    assert access.closed


def test_exported_memoryview_prevents_close() -> None:
    buffer = allocate_buffer(8)
    access = write_buffer(buffer)
    view = access.__enter__()
    derived = memoryview(view)

    with pytest.raises(BufferError, match='exported views'):
        access.close()
    assert not access.closed

    del derived
    gc.collect()
    access.close()
    assert access.closed


def test_access_state_rules() -> None:
    buffer = allocate_buffer(8)
    writer = write_buffer(buffer)

    with pytest.raises(RuntimeError, match='while writing'):
        read_buffer(buffer)
    with pytest.raises(RuntimeError, match='already in use'):
        write_buffer(buffer)

    with writer as view:
        view[0] = 42

    with pytest.raises(RuntimeError, match='already been finalized'):
        write_buffer(buffer)

    first = read_buffer(buffer)
    second = read_buffer(buffer)
    assert first[0] == second[0] == 42


def test_invalid_inputs_and_closed_access() -> None:
    with pytest.raises(ValueError, match='greater than zero'):
        allocate_buffer(0)
    with pytest.raises(TypeError):
        allocate_buffer(-1)
    with pytest.raises(TypeError):
        read_buffer(object())

    buffer = allocate_buffer(1)
    access = write_buffer(buffer)
    with access:
        pass
    access.close()
    assert access.closed
    with pytest.raises(ValueError, match='closed'):
        access.__enter__()
