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

#ifndef SHMEM_BUFFER__HOST_ENDPOINT_MANAGER_HPP_
#define SHMEM_BUFFER__HOST_ENDPOINT_MANAGER_HPP_

#include <string>

#include "shmem_buffer/visibility_control.h"

namespace shmem_buffer
{

/// Supplies the conservative same-host/user locality identity used by shared
/// memory endpoint discovery. Windows also includes the login session.
class SHMEM_BUFFER_PUBLIC HostEndpointManager
{
public:
  HostEndpointManager();

  const std::string & metadata() const {return metadata_;}

private:
  std::string metadata_;
};

}  // namespace shmem_buffer

#endif  // SHMEM_BUFFER__HOST_ENDPOINT_MANAGER_HPP_
