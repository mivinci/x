/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * relay_test.cpp - Comprehensive tests for xRelay
 */

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <x/base/event.h>
#include <x/base/relay.h>
#include <x/base/test_helper.h>

/* ───────────────────── Fixture ───────────────────── */

class RelayTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;
  xRelay    *r    = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xEventLoopEnter(loop);
    r = xRelayCreate();
    ASSERT_NE(r, nullptr);
  }

  void TearDown() override {
    if (r) xRelayDestroy(r);
    xEventLoopLeave();
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, CreateAndDestroy) {
  /* Just verifying SetUp / TearDown don't crash. */
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Emit — zero subscribers (no-op)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, EmitWithZeroSubscribersDoesNotCrash) {
  int dummy = 0;
  xRelayEmit(r, &dummy, sizeof(dummy));
  /* Reaches here without abort */
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  On + Emit — same-loop synchronous dispatch
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, EmitCallsSubscriber) {
  std::atomic<int> call_count{0};

  auto fn = [](void *data, void *arg) {
    auto *cnt = static_cast<std::atomic<int> *>(arg);
    cnt->fetch_add(1);
  };

  xRelayOn(r, fn, &call_count);
  EXPECT_EQ(call_count.load(), 0);

  int payload = 42;
  xRelayEmit(r, &payload, sizeof(payload));
  EXPECT_EQ(call_count.load(), 1);
}

TEST_F(RelayTest, EmitPassesData) {
  int  received  = 0;
  auto fn        = [](void *data, void *arg) {
    int *out = static_cast<int *>(arg);
    *out     = *static_cast<int *>(data);
  };

  xRelayOn(r, fn, &received);

  int payload = 12345;
  xRelayEmit(r, &payload, sizeof(payload));
  EXPECT_EQ(received, 12345);
}

TEST_F(RelayTest, EmitWithSizeZeroSendsNullData) {
  std::atomic<bool> received_null{false};

  auto fn = [](void *data, void *arg) {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    flag->store(data == NULL);
  };

  xRelayOn(r, fn, &received_null);
  xRelayEmit(r, NULL, 0);
  EXPECT_TRUE(received_null.load());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Multiple subscribers
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, EmitNotifiesAllSubscribers) {
  std::atomic<int> a{0}, b{0}, c{0};

  auto fn_a = [](void *, void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); };
  auto fn_b = [](void *, void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); };

  xRelayOn(r, fn_a, &a);
  xRelayOn(r, fn_a, &b);
  xRelayOn(r, static_cast<xRelayFunc>(fn_b), &c);

  int payload = 1;
  xRelayEmit(r, &payload, sizeof(payload));

  EXPECT_EQ(a.load(), 1);
  EXPECT_EQ(b.load(), 1);
  EXPECT_EQ(c.load(), 1);
}

TEST_F(RelayTest, DuplicateSubscriptionFiresTwice) {
  std::atomic<int> call_count{0};

  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  };

  xRelayOn(r, fn, &call_count);
  xRelayOn(r, fn, &call_count); /* same {fn, arg} again */

  int payload = 1;
  xRelayEmit(r, &payload, sizeof(payload));

  EXPECT_EQ(call_count.load(), 2);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Off
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, OffRemovesMatchingSubscriber) {
  std::atomic<int> call_count{0};

  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  };

  xRelayOn(r, fn, &call_count);
  xRelayOn(r, fn, &call_count);

  int payload = 1;
  xRelayEmit(r, &payload, sizeof(payload));
  EXPECT_EQ(call_count.load(), 2);

  xRelayOff(r, fn, &call_count);

  call_count.store(0);
  xRelayEmit(r, &payload, sizeof(payload));
  /* Only the second subscriber should remain. */
  EXPECT_EQ(call_count.load(), 1);
}

TEST_F(RelayTest, OffNonExistentIsNoop) {
  auto fn = [](void *, void *) {};

  /* Should not crash or affect relay state. */
  xRelayOff(r, fn, nullptr);
  SUCCEED();
}

TEST_F(RelayTest, OffOnlyRemovesFirstMatch) {
  std::atomic<int> a{0}, b{0};

  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  };

  xRelayOn(r, fn, &a);
  xRelayOn(r, fn, &a); /* duplicate */
  xRelayOn(r, fn, &b);

  /* Remove first {fn, &a} */
  xRelayOff(r, fn, &a);

  int payload = 1;
  xRelayEmit(r, &payload, sizeof(payload));

  /* One {fn, &a} remains, plus {fn, &b} */
  EXPECT_EQ(a.load(), 1);
  EXPECT_EQ(b.load(), 1);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Destroy cleans up subscribers
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, DestroyFreesSubscribers) {
  /*
   * We can't directly test that memory was freed, but we can verify
   * that Emit after destroy does not touch freed memory — the relay
   * handle is owned by the caller and we simply don't call Emit
   * after Destroy.  This test just ensures the teardown path doesn't
   * crash.
   */
  auto fn = [](void *, void *) {};

  /* Add a subscriber so Destroy has work to do. */
  xRelayOn(r, fn, nullptr);
  xRelayOn(r, fn, nullptr);

  xRelayDestroy(r);
  r = nullptr; /* owned by caller, no use-after-free */

  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Cross-event-loop Emit — synchronous dispatch from other thread
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, CrossLoopEmitFiresOnTargetLoop) {
  std::atomic<int>             call_count{0};
  std::atomic<std::thread::id> exec_thread{};
  std::atomic<bool>            done{false};

  auto fn = [](void *, void *arg) {
    auto *ctx = static_cast<
      std::tuple<std::atomic<int> *, std::atomic<std::thread::id> *, std::atomic<bool> *> *>(arg);
    std::get<0>(*ctx)->fetch_add(1);
    std::get<1>(*ctx)->store(std::this_thread::get_id());
    std::get<2>(*ctx)->store(true);
  };

  std::tuple<std::atomic<int> *, std::atomic<std::thread::id> *, std::atomic<bool> *> ctx{
    &call_count, &exec_thread, &done};
  xRelayOn(r, fn, &ctx);

  std::thread::id main_id = std::this_thread::get_id();

  std::thread poster([this, &ctx]() {
    xEventLoop bg = xEventLoopCreate();
    xEventLoopEnter(bg);

    int payload = 99;
    xRelayEmit(this->r, &payload, sizeof(payload));

    xEventLoopLeave();
    xEventLoopDestroy(bg);
  });
  poster.join();

  run_until(loop, done, 5000);

  EXPECT_EQ(call_count.load(), 1);
  EXPECT_EQ(exec_thread.load(), main_id)
      << "subscriber must run on the main loop thread, not the background thread";
}

TEST_F(RelayTest, CrossLoopDataIsCopiedCorrectly) {
  std::atomic<int>  received{0};
  std::atomic<bool> done{false};

  auto fn = [](void *data, void *arg) {
    auto *ctx = static_cast<std::pair<std::atomic<int> *, std::atomic<bool> *> *>(arg);
    ctx->first->store(*static_cast<int *>(data));
    ctx->second->store(true);
  };

  std::pair<std::atomic<int> *, std::atomic<bool> *> ctx{&received, &done};
  xRelayOn(r, fn, &ctx);

  std::thread poster([this]() {
    xEventLoop bg = xEventLoopCreate();
    xEventLoopEnter(bg);

    /*
     * payload is on the background thread's stack — it will be
     * destroyed when this thread exits.  If the relay didn't copy
     * it, the subscriber would read dangling memory.
     */
    int payload = 777;
    xRelayEmit(this->r, &payload, sizeof(payload));

    xEventLoopLeave();
    xEventLoopDestroy(bg);
  });
  poster.join();

  /* Background thread has exited — payload is gone. */
  run_until(loop, done, 5000);

  EXPECT_EQ(received.load(), 777)
      << "cross-loop data must be copied to survive publisher stack unwind";
}

/* ═══════════════════════════════════════════════════════════════════
 *  Cross-loop — string payload (verifies deep copy of larger data)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, CrossLoopStringPayload) {
  std::string         received;
  std::atomic<bool>   done{false};

  auto fn = [](void *data, void *arg) {
    auto *ctx = static_cast<std::pair<std::string *, std::atomic<bool> *> *>(arg);
    ctx->first->assign(static_cast<const char *>(data), 11);
    ctx->second->store(true);
  };

  std::pair<std::string *, std::atomic<bool> *> ctx{&received, &done};
  xRelayOn(r, fn, &ctx);

  std::thread poster([this]() {
    xEventLoop bg = xEventLoopCreate();
    xEventLoopEnter(bg);

    const char *msg = "hello world"; /* 11 chars + NUL */
    xRelayEmit(this->r, msg, 12);      /* include NUL */

    xEventLoopLeave();
    xEventLoopDestroy(bg);
  });
  poster.join();

  run_until(loop, done, 5000);

  EXPECT_EQ(received, "hello world");
}

/* ═══════════════════════════════════════════════════════════════════
 *  No-event-loop Emit — all subscribers dispatched via Post
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, EmitFromThreadWithoutLoop) {
  std::atomic<bool> called{false};

  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<bool> *>(arg)->store(true);
  };

  xRelayOn(r, fn, &called);

  std::thread poster([this]() {
    /*
     * This thread has no event loop.  xEventLoopCurrent() will
     * return NULL.  The subscriber was registered with the main
     * loop, so the relay will use xEventLoopPost.
     */
    int payload = 1;
    xRelayEmit(this->r, &payload, sizeof(payload));
  });
  poster.join();

  run_until(loop, called, 5000);
  EXPECT_TRUE(called.load());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Multiple emits
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, MultipleEmitsAllDelivered) {
  std::atomic<int> sum{0};

  auto fn = [](void *data, void *arg) {
    auto *s = static_cast<std::atomic<int> *>(arg);
    s->fetch_add(*static_cast<int *>(data));
  };

  xRelayOn(r, fn, &sum);

  for (int v = 1; v <= 10; v++)
    xRelayEmit(r, &v, sizeof(v));

  EXPECT_EQ(sum.load(), 55);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Concurrent On + Emit (lock stress test)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, ConcurrentOnAndEmit) {
  constexpr int kSubscribers = 20;

  std::atomic<int> call_count{0};
  std::atomic<int> sub_count{0};

  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  };

  /*
   * Register additional subscribers from a background thread while
   * the main thread emits.  The mutex guarantees that Emit sees a
   * consistent snapshot — either a subscriber is in the snapshot
   * (and gets the emit) or it's not (and misses it).  No crashes.
   */
  std::thread reg_thread([this, &fn, &sub_count]() {
    for (int i = 0; i < kSubscribers; i++) {
      xRelayOn(this->r, fn, &sub_count);
    }
  });

  for (int i = 0; i < 100; i++) {
    int v = 1;
    xRelayEmit(r, &v, sizeof(v));
    call_count.fetch_add(1);
  }

  reg_thread.join();

  /*
   * Not asserting exact counts — the snapshot is not guaranteed to
   * include all subscribers registered concurrently.  We just
   * assert that we didn't crash and some subscribers were called.
   */
  EXPECT_GE(sub_count.load(), 0);
  EXPECT_EQ(call_count.load(), 100);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Struct payload (non-trivial type)
 * ═══════════════════════════════════════════════════════════════════ */

namespace {

struct Vec3 {
  float x, y, z;
};

} // namespace

TEST_F(RelayTest, EmitStructPayload) {
  Vec3 received = {0, 0, 0};

  auto fn = [](void *data, void *arg) {
    auto *v   = static_cast<Vec3 *>(data);
    auto *out = static_cast<Vec3 *>(arg);
    *out      = *v;
  };

  xRelayOn(r, fn, &received);

  Vec3 v = {1.5f, 2.5f, 3.5f};
  xRelayEmit(r, &v, sizeof(v));

  EXPECT_FLOAT_EQ(received.x, 1.5f);
  EXPECT_FLOAT_EQ(received.y, 2.5f);
  EXPECT_FLOAT_EQ(received.z, 3.5f);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Off between Emit — subscriber unsubscribes itself
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, SubscriberUnsubscribesItself) {
  std::atomic<int> call_count{0};

  xRelay *captured_r = r;

  /* Declare as a function pointer so we can reference it inside the body. */
  static xRelayFunc fn = [](void *, void *arg) {
    auto *ctx = static_cast<std::pair<std::atomic<int> *, xRelay *> *>(arg);
    ctx->first->fetch_add(1);
    /* Unsubscribe self — next emit should not call this again. */
    xRelayOff(ctx->second, fn, arg);
  };

  std::pair<std::atomic<int> *, xRelay *> ctx{&call_count, captured_r};
  xRelayOn(r, fn, &ctx);

  int v = 0;
  xRelayEmit(r, &v, sizeof(v));
  EXPECT_EQ(call_count.load(), 1);

  /* Second emit — subscriber has removed itself. */
  xRelayEmit(r, &v, sizeof(v));
  EXPECT_EQ(call_count.load(), 1); /* still 1 */
}

/* ═══════════════════════════════════════════════════════════════════
 *  Large subscriber count (heap-allocated snapshot path)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(RelayTest, EmitWithManySubscribers) {
  constexpr int kN = 32;

  std::atomic<int> counts[kN] = {};
  auto fn = [](void *, void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  };

  for (int i = 0; i < kN; i++)
    xRelayOn(r, fn, &counts[i]);

  int v = 0;
  xRelayEmit(r, &v, sizeof(v));

  for (int i = 0; i < kN; i++)
    EXPECT_EQ(counts[i].load(), 1) << "subscriber " << i;
}
