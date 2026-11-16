//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"

namespace louhi::test {

// Timers a component asked for, fired by the test as stepped time passes
// them. Thread-safe.
class Timers {
 public:
  explicit Timers(dannan::test::SteppedTime& time) : _time(time) {}
  void after(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.push_back({_time.now() + delay, std::move(fn)});
  }
  // Fires every timer due by now.
  void fire_due() {
    std::vector<std::function<void()>> due;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      for (auto it = _pending.begin(); it != _pending.end();) {
        if (it->first <= _time.now()) {
          due.push_back(std::move(it->second));
          it = _pending.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& fn : due) fn();
  }
  size_t pending() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _pending.size();
  }

 private:
  dannan::test::SteppedTime& _time;
  std::mutex _mutex;
  std::vector<std::pair<dannan::contract::TimePoint, std::function<void()>>> _pending;
};

}  // namespace louhi::test
