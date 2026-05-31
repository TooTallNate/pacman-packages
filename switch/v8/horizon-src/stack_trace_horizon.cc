// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Stack trace implementation for Nintendo Switch (Horizon / devkitA64).
// newlib has no <execinfo.h>/backtrace(), so in-process stack capture is not
// supported. Modeled on stack_trace_fuchsia.cc.

#include "src/base/debug/stack_trace.h"

#include <iomanip>
#include <ostream>

#include "src/base/platform/platform.h"

namespace v8 {
namespace base {
namespace debug {

bool EnableInProcessStackDumping() { return false; }

void DisableSignalStackDump() {}

StackTrace::StackTrace() {}

void StackTrace::Print() const {
  std::string backtrace = ToString();
  OS::Print("%s\n", backtrace.c_str());
}

void StackTrace::OutputToStream(std::ostream* os) const {
  for (size_t i = 0; i < count_; ++i) {
    *os << "#" << std::setw(2) << i << trace_[i] << "\n";
  }
}

}  // namespace debug
}  // namespace base
}  // namespace v8
