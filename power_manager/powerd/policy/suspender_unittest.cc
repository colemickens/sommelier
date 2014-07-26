// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/policy/suspender.h"

#include <base/basictypes.h>
#include <base/bind.h>
#include <base/callback.h>
#include <base/compiler_specific.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>
#include <gtest/gtest.h>

#include "power_manager/common/action_recorder.h"
#include "power_manager/common/dbus_sender_stub.h"
#include "power_manager/common/fake_prefs.h"
#include "power_manager/common/power_constants.h"
#include "power_manager/powerd/system/dark_resume_stub.h"

namespace power_manager {
namespace policy {

namespace {

// Various actions that can be returned by TestDelegate::GetActions().
const char kPrepare[] = "prepare";
const char kSuspend[] = "suspend";
const char kUnprepare[] = "unprepare";
const char kShutDown[] = "shut_down";
const char kNoActions[] = "";

// Test implementation of Suspender::Delegate that just records the actions it
// was asked to perform.
class TestDelegate : public Suspender::Delegate, public ActionRecorder {
 public:
  TestDelegate()
      : lid_closed_(false),
        report_success_for_read_wakeup_count_(true),
        suspend_result_(SUSPEND_SUCCESSFUL),
        wakeup_count_(0),
        suspend_announced_(false),
        suspend_wakeup_count_(0),
        suspend_wakeup_count_valid_(false),
        suspend_was_successful_(false),
        num_suspend_attempts_(0),
        suspend_canceled_while_in_dark_resume_(false),
        can_safely_exit_dark_resume_(true) {
  }

  void set_lid_closed(bool closed) { lid_closed_ = closed; }
  void set_report_success_for_read_wakeup_count(bool success) {
    report_success_for_read_wakeup_count_ = success;
  }
  void set_suspend_announced(bool announced) { suspend_announced_ = announced; }
  void set_suspend_result(SuspendResult result) {
    suspend_result_ = result;
  }
  void set_wakeup_count(uint64_t count) { wakeup_count_ = count; }
  void set_suspend_callback(base::Closure callback) {
    suspend_callback_ = callback;
  }
  void set_completion_callback(base::Closure callback) {
    completion_callback_ = callback;
  }
  void set_shutdown_callback(base::Closure callback) {
    shutdown_callback_ = callback;
  }

  void set_can_safely_exit_dark_resume(bool can_exit) {
    can_safely_exit_dark_resume_ = can_exit;
  }

  bool suspend_announced() const { return suspend_announced_; }
  uint64_t suspend_wakeup_count() const { return suspend_wakeup_count_; }
  bool suspend_wakeup_count_valid() const {
    return suspend_wakeup_count_valid_;
  }
  const base::TimeDelta& suspend_duration() const { return suspend_duration_; }
  bool suspend_was_successful() const { return suspend_was_successful_; }
  int num_suspend_attempts() const { return num_suspend_attempts_; }
  bool suspend_canceled_while_in_dark_resume() const {
    return suspend_canceled_while_in_dark_resume_;
  }

  // Delegate implementation:
  int GetInitialSuspendId() override { return 1; }

  bool IsLidClosedForSuspend() override { return lid_closed_; }

  bool ReadSuspendWakeupCount(uint64_t* wakeup_count) override {
    if (!report_success_for_read_wakeup_count_)
      return false;
    *wakeup_count = wakeup_count_;
    return true;
  }

  void SetSuspendAnnounced(bool announced) override {
    suspend_announced_ = announced;
  }

  bool GetSuspendAnnounced() override { return suspend_announced_; }

  void PrepareToSuspend() override {
    AppendAction(kPrepare);
  }

  SuspendResult DoSuspend(uint64_t wakeup_count,
                                  bool wakeup_count_valid,
                                  base::TimeDelta duration) override {
    AppendAction(kSuspend);
    suspend_wakeup_count_ = wakeup_count;
    suspend_wakeup_count_valid_ = wakeup_count_valid;
    suspend_duration_ = duration;
    RunAndResetCallback(&suspend_callback_);
    return suspend_result_;
  }

  void UndoPrepareToSuspend(bool success,
                            int num_suspend_attempts,
                            bool canceled_while_in_dark_resume) override {
    AppendAction(kUnprepare);
    suspend_was_successful_ = success;
    num_suspend_attempts_ = num_suspend_attempts;
    suspend_canceled_while_in_dark_resume_ = canceled_while_in_dark_resume;
    RunAndResetCallback(&completion_callback_);
  }

  void ShutDownForFailedSuspend() override {
    AppendAction(kShutDown);
    RunAndResetCallback(&shutdown_callback_);
  }

  void ShutDownForDarkResume() override {
    AppendAction(kShutDown);
    RunAndResetCallback(&shutdown_callback_);
  }

  bool CanSafelyExitDarkResume() override {
    return can_safely_exit_dark_resume_;
  }

 private:
  // If |callback| is non-null, runs and resets it.
  static void RunAndResetCallback(base::Closure* callback) {
    if (callback->is_null())
      return;

    base::Closure callback_copy = *callback;
    callback->Reset();
    callback_copy.Run();
  }

  // Value returned by IsLidClosedForSuspend().
  bool lid_closed_;

  // Should ReadSuspendWakeupCount() and DoSuspend() report success?
  bool report_success_for_read_wakeup_count_;
  SuspendResult suspend_result_;

  // Count that should be returned by ReadSuspendWakeupCount().
  uint64_t wakeup_count_;

  // Updated by SetSuspendAnnounced() and returned by GetSuspendAnnounced().
  bool suspend_announced_;

  // Callback that will be run once (if non-null) when DoSuspend() is called.
  base::Closure suspend_callback_;

  // Callback that will be run once (if non-null) when
  // UndoPrepareToSuspend() is called.
  base::Closure completion_callback_;

  // Callback that will be run once (if non-null) when ShutDown*() is called.
  base::Closure shutdown_callback_;

  // Arguments passed to last invocation of DoSuspend().
  uint64_t suspend_wakeup_count_;
  bool suspend_wakeup_count_valid_;
  base::TimeDelta suspend_duration_;

  // Arguments passed to last invocation of UndoPrepareToSuspend().
  bool suspend_was_successful_;
  int num_suspend_attempts_;
  bool suspend_canceled_while_in_dark_resume_;

  // Value returned by CanSafelyExitDarkResume().
  bool can_safely_exit_dark_resume_;

  DISALLOW_COPY_AND_ASSIGN(TestDelegate);
};

}  // namespace

class SuspenderTest : public testing::Test {
 public:
  SuspenderTest()
      : test_api_(&suspender_),
        pref_retry_delay_ms_(10000),
        pref_num_retries_(10) {
  }

 protected:
  void Init() {
    prefs_.SetInt64(kRetrySuspendMsPref, pref_retry_delay_ms_);
    prefs_.SetInt64(kRetrySuspendAttemptsPref, pref_num_retries_);
    suspender_.Init(&delegate_, &dbus_sender_, &dark_resume_, &prefs_);
  }

  // Returns the ID from a SuspendImminent signal at |position|, or -1 if the
  // signal wasn't sent.
  int GetSuspendImminentId(int position) {
    SuspendImminent proto;
    if (!dbus_sender_.GetSentSignal(position, kSuspendImminentSignal, &proto))
      return -1;
    return proto.suspend_id();
  }

  // Returns the ID from a SuspendDone signal at |position|, or -1 if the signal
  // wasn't sent.
  int GetSuspendDoneId(int position) {
    SuspendDone proto;
    if (!dbus_sender_.GetSentSignal(position, kSuspendDoneSignal, &proto))
      return -1;
    return proto.suspend_id();
  }

  FakePrefs prefs_;
  TestDelegate delegate_;
  DBusSenderStub dbus_sender_;
  system::DarkResumeStub dark_resume_;
  Suspender suspender_;
  Suspender::TestApi test_api_;

  int64_t pref_retry_delay_ms_;
  int64_t pref_num_retries_;
};

// Tests the standard suspend/resume cycle.
TEST_F(SuspenderTest, SuspendResume) {
  Init();

  // Suspender shouldn't run powerd_suspend until it receives notice that
  // SuspendDelayController is ready.
  const uint64_t kWakeupCount = 452;
  const base::Time kRequestTime = base::Time::FromInternalValue(123);
  test_api_.SetCurrentWallTime(kRequestTime);
  delegate_.set_wakeup_count(kWakeupCount);
  suspender_.RequestSuspend();
  const int suspend_id = test_api_.suspend_id();
  EXPECT_EQ(suspend_id, GetSuspendImminentId(0));
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_TRUE(delegate_.suspend_announced());

  // Advance the time and register a callback to advance the time again
  // when the suspend request is received.
  const base::Time kSuspendTime = base::Time::FromInternalValue(301);
  test_api_.SetCurrentWallTime(kSuspendTime);
  const base::Time kResumeTime = base::Time::FromInternalValue(567);
  delegate_.set_suspend_callback(
      base::Bind(&Suspender::TestApi::SetCurrentWallTime,
                 base::Unretained(&test_api_), kResumeTime));

  // When Suspender receives notice that the system is ready to be
  // suspended, it should immediately suspend the system.
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(suspend_id);
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_EQ(kWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_TRUE(delegate_.suspend_wakeup_count_valid());
  EXPECT_TRUE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());

  // A SuspendDone signal should be emitted to announce that the attempt is
  // complete.
  SuspendDone done_proto;
  EXPECT_TRUE(dbus_sender_.GetSentSignal(0, kSuspendDoneSignal, &done_proto));
  EXPECT_EQ(suspend_id, done_proto.suspend_id());
  EXPECT_EQ((kResumeTime - kRequestTime).ToInternalValue(),
            done_proto.suspend_duration());
  EXPECT_FALSE(delegate_.suspend_announced());

  // A resuspend timeout shouldn't be set.
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that Suspender doesn't pass a wakeup count to the delegate when it was
// unable to fetch one.
TEST_F(SuspenderTest, MissingWakeupCount) {
  Init();

  delegate_.set_report_success_for_read_wakeup_count(false);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_wakeup_count_valid());
}

// Tests that calls to RequestSuspend() are ignored when a suspend request is
// already underway.
TEST_F(SuspenderTest, IgnoreDuplicateSuspendRequests) {
  Init();

  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  const int orig_suspend_id = test_api_.suspend_id();

  // The suspend ID should be left unchanged after a second call to
  // RequestSuspend().
  suspender_.RequestSuspend();
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  EXPECT_EQ(orig_suspend_id, test_api_.suspend_id());
}

// Tests that suspend is retried on failure.
TEST_F(SuspenderTest, RetryOnFailure) {
  Init();

  const uint64_t kOrigWakeupCount = 46;
  delegate_.set_wakeup_count(kOrigWakeupCount);
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  suspender_.RequestSuspend();
  const int suspend_id = test_api_.suspend_id();
  EXPECT_EQ(suspend_id, GetSuspendImminentId(0));
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_TRUE(delegate_.suspend_announced());

  const uint64_t kRetryWakeupCount = 67;
  delegate_.set_wakeup_count(kRetryWakeupCount);
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(suspend_id);
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(kOrigWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_TRUE(delegate_.suspend_wakeup_count_valid());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  // The timeout should trigger another suspend attempt.
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(kRetryWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_TRUE(delegate_.suspend_wakeup_count_valid());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  // A second suspend request should be ignored so we'll avoid trying to
  // re-suspend immediately if an attempt fails while the lid is closed
  // (http://crbug.com/384610). Also check that an external wakeup count passed
  // in the request gets ignored for the eventual retry.
  const uint64_t kExternalWakeupCount = 32542;
  suspender_.RequestSuspendWithExternalWakeupCount(kExternalWakeupCount);
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  // Report success this time and check that the timer isn't running.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_SUCCESSFUL);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_NE(kExternalWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_TRUE(delegate_.suspend_was_successful());
  EXPECT_EQ(3, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_EQ(suspend_id, GetSuspendDoneId(0));
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // Suspend successfully again and check that the number of attempts are
  // reported as 1 now.
  dbus_sender_.ClearSentSignals();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  const int new_suspend_id = test_api_.suspend_id();
  EXPECT_EQ(new_suspend_id, GetSuspendImminentId(0));

  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(new_suspend_id);
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_TRUE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_EQ(new_suspend_id, GetSuspendDoneId(0));
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that the system is shut down after repeated suspend failures.
TEST_F(SuspenderTest, ShutDownAfterRepeatedFailures) {
  pref_num_retries_ = 5;
  Init();

  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // Proceed through all retries, reporting failure each time.
  for (int i = 1; i <= pref_num_retries_ - 1; ++i) {
    EXPECT_TRUE(test_api_.TriggerResuspendTimeout()) << "Retry #" << i;
    EXPECT_EQ(kSuspend, delegate_.GetActions()) << "Retry #" << i;
  }

  // Check that another suspend request doesn't reset the retry count
  // (http://crbug.com/384610).
  suspender_.RequestSuspend();
  EXPECT_EQ(kNoActions, delegate_.GetActions());

  // After the last failed attempt, the system should shut down immediately.
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kShutDown, NULL), delegate_.GetActions());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that OnReadyForSuspend() doesn't trigger a call to Suspend() if
// activity that should cancel the current suspend attempt was previously
// received.
TEST_F(SuspenderTest, CancelBeforeSuspend) {
  Init();

  // User activity should cancel suspending.
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendImminentId(0));
  EXPECT_TRUE(delegate_.suspend_announced());

  suspender_.HandleUserActivity();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_FALSE(delegate_.suspend_announced());
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(0, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());

  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // The lid being opened should also cancel.
  dbus_sender_.ClearSentSignals();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendImminentId(0));
  suspender_.HandleLidOpened();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(0, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // The request should also be canceled if the system starts shutting down.
  dbus_sender_.ClearSentSignals();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendImminentId(0));
  suspender_.HandleShutdown();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(0, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // Subsequent requests after shutdown has started should be ignored.
  suspender_.RequestSuspend();
  EXPECT_EQ(kNoActions, delegate_.GetActions());
}

// Tests that a suspend-canceling action after a failed suspend attempt
// should remove the retry timeout.
TEST_F(SuspenderTest, CancelAfterSuspend) {
  Init();
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // Fail a second time.
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // This time, report user activity first, which should cancel the request.
  suspender_.HandleUserActivity();
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(2, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that Chrome-reported user activity received while suspending with
// a closed lid doesn't abort the suspend attempt (http://crosbug.com/38819).
TEST_F(SuspenderTest, DontCancelForUserActivityWhileLidClosed) {
  delegate_.set_lid_closed(true);
  Init();

  // Report user activity before powerd_suspend is executed and check that
  // Suspender still suspends when OnReadyForSuspend() is called.
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.HandleUserActivity();
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());

  // Report user activity after powerd_suspend fails and check that the
  // resuspend timer isn't stopped.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_CANCELED);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  suspender_.HandleUserActivity();

  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_SUCCESSFUL);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
}

// Tests that expected wakeup counts passed to
// RequestSuspendWithExternalWakeupCount() are honored.
TEST_F(SuspenderTest, ExternalWakeupCount) {
  Init();

  // Pass a wakeup count less than the one that the delegate returns.
  const uint64_t kWakeupCount = 452;
  delegate_.set_wakeup_count(kWakeupCount);
  suspender_.RequestSuspendWithExternalWakeupCount(kWakeupCount - 1);
  EXPECT_EQ(kPrepare, delegate_.GetActions());

  // Make the delegate report that powerd_suspend reported a wakeup count
  // mismatch. Suspender should avoid retrying after a mismatch when using an
  // external wakeup count.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_CANCELED);
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_EQ(kWakeupCount - 1, delegate_.suspend_wakeup_count());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // Send another suspend request with the current wakeup count. Report failure
  // and check that the suspend attempt is retried using the external wakeup
  // count.
  suspender_.RequestSuspendWithExternalWakeupCount(kWakeupCount);
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(kWakeupCount, delegate_.suspend_wakeup_count());

  // Let the retry succeed and check that another retry isn't scheduled.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_SUCCESSFUL);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_EQ(kWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that the SuspendDone signal contains a zero duration rather than a
// negative duration if the system clock jumps backward between suspend and
// resume.
TEST_F(SuspenderTest, SystemClockGoesBackward) {
  Init();
  test_api_.SetCurrentWallTime(base::Time::FromInternalValue(5000));
  suspender_.RequestSuspend();

  delegate_.set_suspend_callback(
      base::Bind(&Suspender::TestApi::SetCurrentWallTime,
                 base::Unretained(&test_api_),
                 base::Time::FromInternalValue(1000)));
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  SuspendDone done_proto;
  EXPECT_TRUE(dbus_sender_.GetSentSignal(0, kSuspendDoneSignal, &done_proto));
  EXPECT_EQ(base::TimeDelta().ToInternalValue(), done_proto.suspend_duration());
}

// Tests that things don't go haywire when
// Suspender::Delegate::UndoPrepareToSuspend() synchronously starts another
// suspend request. Previously, this could result in the new request being
// started before the previous one had completed.
TEST_F(SuspenderTest, EventReceivedWhileHandlingEvent) {
  // Instruct the delegate to send another suspend request when the current one
  // finishes.
  Init();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendImminentId(0));
  delegate_.set_completion_callback(
      base::Bind(&Suspender::RequestSuspend, base::Unretained(&suspender_)));

  // Check that the SuspendDone signal from the first request contains the first
  // request's ID, and that a second request was started immediately.
  dbus_sender_.ClearSentSignals();
  const int kOldSuspendId = test_api_.suspend_id();
  suspender_.OnReadyForSuspend(kOldSuspendId);
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, kPrepare, NULL),
            delegate_.GetActions());
  EXPECT_EQ(kOldSuspendId, GetSuspendDoneId(0));
  const int kNewSuspendId = test_api_.suspend_id();
  EXPECT_NE(kOldSuspendId, kNewSuspendId);
  EXPECT_EQ(kNewSuspendId, GetSuspendImminentId(1));

  // Finish the second request.
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(kNewSuspendId);
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_EQ(kNewSuspendId, GetSuspendDoneId(0));
  dbus_sender_.ClearSentSignals();

  // Now make the delegate's shutdown method report that the system is shutting
  // down.
  delegate_.set_shutdown_callback(
      base::Bind(&Suspender::HandleShutdown, base::Unretained(&suspender_)));
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  dark_resume_.set_action(system::DarkResumeInterface::SHUT_DOWN);
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kShutDown, delegate_.GetActions());
}

// Tests that a SuspendDone signal is emitted at startup and the "suspend
// announced" state is cleared if the delegate claims that a previous suspend
// attempt was abandoned after being announced.
TEST_F(SuspenderTest, SendSuspendDoneAtStartupForAbandonedAttempt) {
  delegate_.set_suspend_announced(true);
  Init();
  SuspendDone proto;
  EXPECT_TRUE(dbus_sender_.GetSentSignal(0, kSuspendDoneSignal, &proto));
  EXPECT_EQ(0, proto.suspend_id());
  EXPECT_EQ(base::TimeDelta().ToInternalValue(), proto.suspend_duration());
  EXPECT_FALSE(delegate_.suspend_announced());
}

TEST_F(SuspenderTest, DarkResume) {
  Init();
  const int kWakeupCount = 45;
  delegate_.set_wakeup_count(kWakeupCount);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  const int kSuspendId = test_api_.suspend_id();
  EXPECT_EQ(kSuspendId, GetSuspendImminentId(0));

  // Instruct |dark_resume_| to request a ten-second suspend and report that the
  // system did a dark resume.
  const int64_t kSuspendSec = 10;
  dark_resume_.set_action(system::DarkResumeInterface::SUSPEND);
  dark_resume_.set_in_dark_resume(true);
  dark_resume_.set_suspend_duration(base::TimeDelta::FromSeconds(kSuspendSec));
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(kSuspendId);
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(kWakeupCount, delegate_.suspend_wakeup_count());
  EXPECT_TRUE(delegate_.suspend_wakeup_count_valid());
  EXPECT_EQ(kSuspendSec, delegate_.suspend_duration().InSeconds());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  // The system should resuspend for another ten seconds, but without using the
  // wakeup count this time. Make it do a normal resume.
  dark_resume_.set_in_dark_resume(false);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_wakeup_count_valid());
  EXPECT_EQ(kSuspendSec, delegate_.suspend_duration().InSeconds());
  EXPECT_EQ(kSuspendId, GetSuspendDoneId(0));

  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

TEST_F(SuspenderTest, DarkResumeShutDown) {
  Init();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  dark_resume_.set_action(system::DarkResumeInterface::SHUT_DOWN);
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kShutDown, delegate_.GetActions());
}

TEST_F(SuspenderTest, DarkResumeRetry) {
  pref_num_retries_ = 2;
  Init();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());

  // Suspend for ten seconds.
  const int64_t kSuspendSec = 10;
  dark_resume_.set_action(system::DarkResumeInterface::SUSPEND);
  dark_resume_.set_in_dark_resume(true);
  dark_resume_.set_suspend_duration(base::TimeDelta::FromSeconds(kSuspendSec));
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // Now make one resuspend attempt while in dark resume fail and a second
  // attempt succeed. The successful attempt should reset the retry counter.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_SUCCESSFUL);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // Fail to resuspend one time short of the retry limit.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  for (int i = 0; i < pref_num_retries_; ++i) {
    SCOPED_TRACE(base::StringPrintf("Attempt #%d", i));
    EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
    EXPECT_EQ(kSuspend, delegate_.GetActions());
    EXPECT_FALSE(delegate_.suspend_wakeup_count_valid());
    EXPECT_EQ(kSuspendSec, delegate_.suspend_duration().InSeconds());
  }

  // The next failure should result in the system shutting down.
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kShutDown, NULL), delegate_.GetActions());
}

TEST_F(SuspenderTest, DarkResumeCancelBeforeResuspend) {
  Init();

  // Suspend for 10 seconds.
  const int64_t kSuspendSec = 10;
  dark_resume_.set_action(system::DarkResumeInterface::SUSPEND);
  dark_resume_.set_in_dark_resume(true);
  dark_resume_.set_suspend_duration(base::TimeDelta::FromSeconds(kSuspendSec));

  // User activity should trigger the transition to fully resumed.
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  suspender_.HandleUserActivity();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_FALSE(delegate_.suspend_announced());
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_TRUE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // Opening the lid should also trigger the transition.
  dbus_sender_.ClearSentSignals();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  suspender_.HandleLidOpened();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_FALSE(delegate_.suspend_announced());
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_TRUE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());

  // Shutting down the system will also trigger the transition so that clients
  // can perform cleanup.
  dbus_sender_.ClearSentSignals();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  suspender_.HandleShutdown();
  EXPECT_EQ(test_api_.suspend_id(), GetSuspendDoneId(1));
  EXPECT_FALSE(delegate_.suspend_announced());
  EXPECT_EQ(kUnprepare, delegate_.GetActions());
  EXPECT_FALSE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_TRUE(delegate_.suspend_canceled_while_in_dark_resume());
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
}

// Tests that user activity is ignored and that no dbus signals are sent out
// during dark resume on legacy systems.
TEST_F(SuspenderTest, DarkResumeOnLegacySystems) {
  Init();

  // Systems with older kernels cannot safely transition from dark resume to
  // fully resumed.
  delegate_.set_can_safely_exit_dark_resume(false);

  // Suspend for 10 seconds.
  const int64_t kSuspendSec = 10;
  dark_resume_.set_action(system::DarkResumeInterface::SUSPEND);
  dark_resume_.set_suspend_duration(base::TimeDelta::FromSeconds(kSuspendSec));

  // User activity should be ignored.
  dark_resume_.set_in_dark_resume(true);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  suspender_.HandleUserActivity();
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());
  EXPECT_TRUE(delegate_.suspend_announced());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  dark_resume_.set_in_dark_resume(false);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());

  // Opening the lid should also be ignored.
  dbus_sender_.ClearSentSignals();
  dark_resume_.set_in_dark_resume(true);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  suspender_.HandleLidOpened();
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());
  EXPECT_TRUE(delegate_.suspend_announced());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  dark_resume_.set_in_dark_resume(false);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());

  // Shutting down the system will not trigger a transition.
  dbus_sender_.ClearSentSignals();
  dark_resume_.set_in_dark_resume(true);
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());
  dbus_sender_.ClearSentSignals();
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());

  suspender_.HandleShutdown();
  EXPECT_EQ(0, dbus_sender_.num_sent_signals());
  EXPECT_TRUE(delegate_.suspend_announced());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
  dark_resume_.set_in_dark_resume(false);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kNoActions, delegate_.GetActions());
}

TEST_F(SuspenderTest, ReportInitialSuspendAttempts) {
  Init();
  suspender_.RequestSuspend();
  EXPECT_EQ(kPrepare, delegate_.GetActions());

  // Suspend successfully once and do a dark resume.
  dark_resume_.set_action(system::DarkResumeInterface::SUSPEND);
  dark_resume_.set_in_dark_resume(true);
  dark_resume_.set_suspend_duration(base::TimeDelta::FromSeconds(10));
  suspender_.OnReadyForSuspend(test_api_.suspend_id());
  EXPECT_EQ(kSuspend, delegate_.GetActions());

  // Report failure for the first attempt to resuspend from dark resume; then
  // report success for the second attempt.
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_FAILED);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(kSuspend, delegate_.GetActions());
  dark_resume_.set_in_dark_resume(false);
  delegate_.set_suspend_result(Suspender::Delegate::SUSPEND_SUCCESSFUL);
  EXPECT_TRUE(test_api_.TriggerResuspendTimeout());
  EXPECT_EQ(JoinActions(kSuspend, kUnprepare, NULL), delegate_.GetActions());

  // Check that the single initial suspend attempt is reported rather than the
  // two attempts that occurred while in dark resume.
  EXPECT_FALSE(test_api_.TriggerResuspendTimeout());
  EXPECT_TRUE(delegate_.suspend_was_successful());
  EXPECT_EQ(1, delegate_.num_suspend_attempts());
  EXPECT_FALSE(delegate_.suspend_canceled_while_in_dark_resume());
}

}  // namespace policy
}  // namespace power_manager
