// Copyright 2026 Summon Software Labs.
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

#pragma once

#include <atomic>
#include <memory>

namespace slf {

/// Cooperative cancellation handle.
///
/// Copies share one flag, so a token handed to a long running enumeration can
/// be cancelled from another thread. Cancellation is always reported as
/// c StatusCode::Cancelled; it never produces a partial result that looks
/// complete.
class CancellationToken {
 public:
  CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  /// Returns a token that is already cancelled. Named \c pre_cancelled because
  /// the instance predicate \c cancelled() takes no parameters and C++ forbids
  /// overloading a static and a non-static member with the same parameter list.
  [[nodiscard]] static CancellationToken pre_cancelled() {
    CancellationToken token;
    token.cancel();
    return token;
  }

  void cancel() noexcept { flag_->store(true, std::memory_order_release); }
  void reset() noexcept { flag_->store(false, std::memory_order_release); }
  [[nodiscard]] bool cancelled() const noexcept {
    return flag_->load(std::memory_order_acquire);
  }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

/// Scope guard that cancels a token when it leaves scope unless released.
class CancellationScope {
 public:
  explicit CancellationScope(CancellationToken token) : token_(std::move(token)) {}
  CancellationScope(const CancellationScope&) = delete;
  CancellationScope& operator=(const CancellationScope&) = delete;
  ~CancellationScope() {
    if (!released_) {
      token_.cancel();
    }
  }
  void release() noexcept { released_ = true; }

 private:
  CancellationToken token_;
  bool released_{false};
};

}  // namespace slf
