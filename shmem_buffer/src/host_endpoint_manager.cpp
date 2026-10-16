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

#include "shmem_buffer/host_endpoint_manager.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif

namespace shmem_buffer
{

HostEndpointManager::HostEndpointManager()
{
#ifdef _WIN32
  std::array<char, MAX_COMPUTERNAME_LENGTH + 1> hostname{};
  DWORD hostname_size = static_cast<DWORD>(hostname.size());
  const bool have_host = GetComputerNameA(hostname.data(), &hostname_size) != FALSE;
  const std::string host = have_host ? std::string(hostname.data(), hostname_size) : "unknown";

  std::string sid = "unknown";
  bool have_sid = false;
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE) {
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<std::uint8_t> storage(required);
    if (
      required > 0 &&
      GetTokenInformation(token, TokenUser, storage.data(), required, &required) != FALSE)
    {
      const auto * token_user = reinterpret_cast<const TOKEN_USER *>(storage.data());
      char * sid_text = nullptr;
      if (ConvertSidToStringSidA(token_user->User.Sid, &sid_text) != FALSE) {
        sid = sid_text;
        have_sid = true;
        (void)LocalFree(sid_text);
      }
    }
    (void)CloseHandle(token);
  }

  DWORD session_id = 0;
  const bool have_session =
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id) != FALSE;
  if (!have_session) {
    session_id = 0xffffffffUL;
  }
  if (!have_host || !have_sid || !have_session) {
    // A process-unique value prevents an uncertain identity from falsely
    // matching another process. Intra-process endpoints can still match.
    metadata_ = "unavailable:" + std::to_string(GetCurrentProcessId());
  } else {
    metadata_ = host + ":" + sid + ":" + std::to_string(session_id);
  }
#else
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), hostname.size() - 1) != 0) {
    const auto euid = static_cast<std::uint64_t>(geteuid());
    metadata_ = "unknown:" + std::to_string(euid);
    return;
  }
  hostname.back() = '\0';
  const auto euid = static_cast<std::uint64_t>(geteuid());
  metadata_ = std::string(hostname.data()) + ":" + std::to_string(euid);
#endif
}

}  // namespace shmem_buffer
