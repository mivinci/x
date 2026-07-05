/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * shared.h - Shared<T>: conditional shared-ownership smart pointer.
 *
 * Default (single-threaded): Rc<T> — non-atomic refcount.
 * With -DXPP_MT (multi-threaded): Arc<T> — atomic refcount.
 *
 * Use for types where ownership is shared between multiple objects
 * (e.g., TcpStream's internal state shared between ReadHalf/WriteHalf
 * after split, or TcpListener's Impl shared with libx callback).
 */

#ifndef XPP_SHARED_H
#define XPP_SHARED_H

#if defined(XPP_MT)
#include <xpp/arc.h>
namespace xpp {
template <class T> using Shared = Arc<T>;
} // namespace xpp
#else
#include <xpp/rc.h>
namespace xpp {
template <class T> using Shared = Rc<T>;
} // namespace xpp
#endif

#endif // XPP_SHARED_H
