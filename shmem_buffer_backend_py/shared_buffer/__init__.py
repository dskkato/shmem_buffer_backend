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

"""Zero-copy Python access to shared-memory ROS 2 buffers."""

from typing import Any
from typing import Optional

from rpyutils import add_dll_directories_from_env

# Since Python 3.8, on Windows we should ensure DLL directories are explicitly
# added to the search path before importing native extensions.
# See https://docs.python.org/3/whatsnew/3.8.html#bpo-36085-whatsnew
with add_dll_directories_from_env('PATH'):
    from shared_buffer._shared_buffer_py import _NativeReadAccess
    from shared_buffer._shared_buffer_py import _NativeWriteAccess
    from shared_buffer._shared_buffer_py import allocate_buffer
    from rosidl_buffer import Buffer


class WriteAccess:
    """Own a scoped native write lease and its directly-created memoryview."""

    def __init__(self, native: Any) -> None:
        self._native: Optional[Any] = native
        self._view: Optional[memoryview] = None

    @property
    def closed(self) -> bool:
        """Whether the native access lease has been released."""
        return self._native is None or self._native.closed

    @property
    def byte_count(self) -> int:
        """Return the number of payload bytes exposed by this access object."""
        if self._native is None:
            raise ValueError('access is closed')
        return self._native.byte_count

    def __enter__(self) -> memoryview:
        if self._native is None or self._native.closed:
            raise ValueError('access is closed')
        if self._view is not None:
            raise RuntimeError('access context is already entered')
        self._view = memoryview(self._native)
        return self._view

    def close(self) -> None:
        """Release the view and lease, rejecting any still-exported derived views."""
        if self._native is None:
            return
        if self._view is not None:
            self._view.release()
        self._native.close()
        self._view = None
        self._native = None

    def __exit__(self, _exc_type: Any, _exc_value: Any, _traceback: Any) -> bool:
        self.close()
        return False


ReadAccess = memoryview


def read_buffer(buffer: Buffer) -> memoryview:
    """
    Return a read-only view whose lifetime owns the native read lease.

    Access to a shared-memory-backed buffer is zero-copy. Other backends follow the
    C++ shared-buffer promotion path and may copy into a temporary shared-memory allocation.
    The lease remains alive through derived buffer objects such as NumPy arrays and is
    released automatically after the last view is destroyed.
    """
    return memoryview(_NativeReadAccess(buffer))


def write_buffer(buffer: Buffer) -> WriteAccess:
    """Acquire scoped writable access to an existing shared-memory-backed buffer."""
    return WriteAccess(_NativeWriteAccess(buffer))


__all__ = [
    'Buffer',
    'ReadAccess',
    'WriteAccess',
    'allocate_buffer',
    'read_buffer',
    'write_buffer',
]
