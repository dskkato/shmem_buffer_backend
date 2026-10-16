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

uint64 size                    # Allocation size in bytes
string element_type_name       # Type name for validation (e.g., "h" for uint8_t)

# IPC metadata for VMM-based zero-copy sharing
int32 shared_buffer_pid                # Publisher process ID
uint32 shared_buffer_block_id          # Block ID in the publisher's pool
uint64 shared_buffer_block_size        # Block size (for import mapping)
string shared_buffer_socket_path       # Linux FD broker path or Windows named mapping object name
uint64 ipc_uid                 # Unique ID per allocation/reuse generation for staleness detection
