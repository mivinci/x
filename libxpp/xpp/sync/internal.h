/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * internal.h - backward-compat shim. Re-exports xpp::sys::_ types
 *              into the xpp::sync::_ namespace for existing modules.
 *
 * New code should include <xpp/loom/internal.h> directly and use
 * xpp::loom::_::{Mutex, Atomic<T>, Lock}.
 */

#ifndef XPP_SYNC_INTERNAL_H
#define XPP_SYNC_INTERNAL_H

#include <xpp/loom/internal.h>

namespace xpp {
namespace sync {
namespace _ {

using loom::_::Atomic;
using loom::_::Lock;
using loom::_::Mutex;

} // namespace _
} // namespace sync
} // namespace xpp

#endif // XPP_SYNC_INTERNAL_H
