/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    x360-native, 2026 - Sleeping dispatch thread (no spin wait).
 *              The idea (Windows: wait on a wake event plus a high resolution
 *              waitable timer, fall back to a plain timer) follows
 *              OpenPortroid/ArmyOfTwoRecompilation
 *              sdk_changes/files/src/core/timer_queue.cpp (MIT License).
 */

// The upstream queue fed the dispatch thread through a disruptorplus ring
// buffer with spin_wait_strategy, so the thread spun (and yielded) whenever it
// was waiting - for the next due time, or forever when no timer was queued at
// all (the case on Windows, where threading_win.cpp uses native timers but the
// global queue still started its thread). Measured ~10% of a core on the Xbox
// One S. This version:
//  - starts the dispatch thread lazily, on the first queued timer;
//  - hands new items over through a mutex-protected vector;
//  - sleeps until the earliest due time and is woken early when a timer is
//    queued (it may be earlier than the one being waited for) or on shutdown.
// The public API (timer_queue.h) is unchanged.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <forward_list>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"
// Older SDK headers lack the flag; the OS rejects it before Windows 10 1803 and
// Start() then falls back to a regular timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <condition_variable>
#endif

namespace rex::thread {

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

  TimerQueue() = default;

  ~TimerQueue() {
    {
      std::lock_guard lock(pending_mutex_);
      stop_ = true;
    }
    Wake();
    if (dispatch_thread_.joinable()) {
      dispatch_thread_.join();
    }
#if REX_PLATFORM_WIN32
    if (wait_timer_) {
      CloseHandle(wait_timer_);
    }
    if (wake_event_) {
      CloseHandle(wake_event_);
    }
#endif
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = (std::max)(clock::now() - wait_item->interval_, wait_item->due_);

    std::call_once(start_once_, [this] { Start(); });
    {
      std::lock_guard lock(pending_mutex_);
      pending_.push_back(std::move(wait_item));
    }
    Wake();

    return wait_item_weak;
  }

  std::thread::id dispatch_thread_id() const {
    return dispatch_thread_id_.load(std::memory_order_acquire);
  }

 private:
  static bool EarlierDue(const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
    return left->due_ < right->due_;
  }

  void Start() {
#if REX_PLATFORM_WIN32
    // Auto-reset: one SetEvent per batch of queued items is enough, and a
    // signal that arrives while the thread is busy is kept for the next wait.
    wake_event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    // High resolution timers (Windows 10 1803+) wake within well under a
    // millisecond; the regular kind rounds up to the system timer tick.
    wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
    if (!wait_timer_) {
      wait_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    assert_not_null(wake_event_);
#endif
    dispatch_thread_ = std::thread([this] { TimerThreadMain(); });
    dispatch_thread_id_.store(dispatch_thread_.get_id(), std::memory_order_release);
  }

  void Wake() {
#if REX_PLATFORM_WIN32
    if (wake_event_) {
      SetEvent(wake_event_);
    }
#else
    pending_cv_.notify_one();
#endif
  }

  // Moves newly queued items into the sorted wait queue. Returns false once
  // shutdown was requested.
  bool TakePending() {
    std::forward_list<std::shared_ptr<WaitItem>> new_items;
    {
      std::lock_guard lock(pending_mutex_);
      if (stop_) {
        return false;
      }
      for (auto& item : pending_) {
        new_items.push_front(std::move(item));
      }
      pending_.clear();
    }
    new_items.sort(EarlierDue);
    wait_queue_.merge(new_items, EarlierDue);
    return true;
  }

  // Invokes every due callback and reschedules recurring ones.
  void DispatchDue() {
    std::forward_list<std::shared_ptr<WaitItem>> rescheduled;
    while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
      auto wait_item = std::move(wait_queue_.front());
      wait_queue_.pop_front();

      // Ensure that it isn't disarmed
      auto state = WaitItem::State::kIdle;
      if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                    std::memory_order_acq_rel)) {
        assert_not_null(wait_item->callback_);
        wait_item->callback_(wait_item->userdata_);

        if (wait_item->interval_ != clock::duration::zero() &&
            wait_item->state_.load(std::memory_order_acquire) !=
                WaitItem::State::kInCallbackSelfDisarmed) {
          // Item is recurring and didn't self-disarm during callback:
          wait_item->due_ += wait_item->interval_;
          wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
          wait_item->state_.notify_all();
          rescheduled.push_front(std::move(wait_item));
        } else {
          wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
          wait_item->state_.notify_all();
        }
      } else {
        // Specifically, kInCallback is illegal here
        assert_true(WaitItem::State::kDisarmed == state);
      }
    }
    rescheduled.sort(EarlierDue);
    wait_queue_.merge(rescheduled, EarlierDue);
  }

  void TimerThreadMain() {
    set_current_thread_name("rex::thread::TimerQueue");

    while (TakePending()) {
      DispatchDue();
      WaitForWork();
    }
  }

#if REX_PLATFORM_WIN32
  void WaitForWork() {
    HANDLE handles[2] = {wake_event_, wait_timer_};
    DWORD count = 1;
    DWORD timeout_ms = INFINITE;
    if (!wait_queue_.empty()) {
      auto wait = wait_queue_.front()->due_ - clock::now();
      if (wait <= clock::duration::zero()) {
        return;
      }
      // Relative due time, in 100 ns units (negative = relative).
      int64_t wait_100ns =
          std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
              wait)
              .count();
      LARGE_INTEGER due;
      due.QuadPart = -(std::max)(wait_100ns, int64_t(1));
      if (wait_timer_ && SetWaitableTimer(wait_timer_, &due, 0, nullptr, nullptr, FALSE)) {
        count = 2;
      } else {
        // No timer object: millisecond wait, rounded up so the due time is
        // never missed by a too-early wake (an early wake only loops again).
        auto ms = std::chrono::ceil<std::chrono::milliseconds>(wait).count();
        timeout_ms = DWORD(std::clamp<int64_t>(ms, 1, int64_t(INFINITE) - 1));
      }
    }
    WaitForMultipleObjectsEx(count, handles, FALSE, timeout_ms, FALSE);
  }
#else
  void WaitForWork() {
    std::unique_lock lock(pending_mutex_);
    auto has_work = [this] { return stop_ || !pending_.empty(); };
    if (wait_queue_.empty()) {
      pending_cv_.wait(lock, has_work);
    } else {
      pending_cv_.wait_until(lock, wait_queue_.front()->due_, has_work);
    }
  }
#endif

  std::once_flag start_once_;

  // Items queued by the public API, handed to the dispatch thread.
  std::mutex pending_mutex_;
  std::vector<std::shared_ptr<WaitItem>> pending_;
  bool stop_ = false;
#if REX_PLATFORM_WIN32
  HANDLE wake_event_ = nullptr;
  HANDLE wait_timer_ = nullptr;
#else
  std::condition_variable pending_cv_;
#endif

  // This is a _sorted_ (ascending due_) list of active timers, only touched by
  // the dispatch thread.
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;

  std::thread dispatch_thread_;
  std::atomic<std::thread::id> dispatch_thread_id_{};
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
