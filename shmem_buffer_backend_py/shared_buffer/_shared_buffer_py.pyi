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

from rosidl_buffer import Buffer


class _NativeReadAccess:
    def __init__(self, buffer: Buffer) -> None: ...


class _NativeWriteAccess:
    def __init__(self, buffer: Buffer) -> None: ...
    @property
    def byte_count(self) -> int: ...
    @property
    def closed(self) -> bool: ...
    def close(self) -> None: ...


def allocate_buffer(byte_count: int) -> Buffer: ...
