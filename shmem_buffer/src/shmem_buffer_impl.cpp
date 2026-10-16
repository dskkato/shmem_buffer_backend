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

#include "shmem_buffer/shmem_buffer_impl.hpp"

#include <memory>

namespace shmem_buffer
{

std::shared_ptr<ShmemBufferMemoryPool> get_or_create_global_pool()
{
  static std::shared_ptr<ShmemBufferMemoryPool> pool = std::make_shared<ShmemBufferMemoryPool>();
  return pool;
}

}  // namespace shmem_buffer
