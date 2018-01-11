// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_POLICY_SUSPENDER_H_
#define POWER_MANAGER_POWERD_POLICY_SUSPENDER_H_

#include <stdint.h>

#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <base/compiler_specific.h>
#include <base/macros.h>
#include <base/time/time.h>
#include <base/timer/timer.h>
#include <dbus/exported_object.h>
#include <dbus/message.h>

#include "power_manager/powerd/policy/suspend_delay_observer.h"
#include "power_manager/proto_bindings/suspend.pb.h"

namespace power_manager {

class Clock;
class PrefsInterface;

namespace system {
class DarkResumeInterface;
class DBusWrapperInterface;
class InputWatcher;
}  // namespace system

namespace policy {

class SuspendDelayController;

// Suspender is responsible for suspending the system.
//
// First, some terminology:
//
// - A "suspend request" refers to a request (either generated within powerd or
//   originating from another process) for powerd to suspend the system. The
//   request is complete after the system resumes successfully or the request is
//   canceled (e.g. by user activity).
//
// - A "suspend attempt" refers to a single attempt by powerd to suspend the
//   system by writing to /sys/power/state.
//
// A suspend request may result in multiple attempts: An attempt may fail and be
// retried after a brief delay, or the system may do a "dark resume" (i.e. wake
// without turning the display on), check the battery level, and then resuspend
// immediately.
//
// The typical flow in the simple case is as follows:
//
// - RequestSuspend() is called when suspending is desired.
// - StartRequest() does pre-suspend preparation and emits a SuspendImminent
//   signal to announce the new suspend request to processes that have
//   previously registered suspend delays via RegisterSuspendDelay().
// - OnReadyForSuspend() is called to announce that all processes have announced
//   readiness via HandleSuspendReadiness().
// - Suspend() runs the powerd_suspend script to perform a suspend attempt.
// - After powerd_suspend returns successfully, FinishRequest() undoes the
//   pre-suspend preparation and emits a SuspendDone signal.
// - If powerd_suspend reported failure, a timer is started to retry the suspend
//   attempt.
//
// At any point before Suspend() has been called, user activity can cancel the
// current suspend attempt.
class Suspender : public SuspendDelayObserver {
 public:
  // Information about dark resumes used for histograms.
  // First value is wake reason; second is wake duration.
  using DarkResumeInfo = std::pair<std::string, base::TimeDelta>;

  // Interface for classes responsible for performing actions on behalf of
  // Suspender.  The general sequence when suspending is:
  //
  // - Suspender::StartRequest() calls PrepareToSuspend() and then notifies
  //   other processes that the system is about to suspend.
  // - Suspender::Suspend() calls DoSuspend() to actually suspend the system.
  //   This may occur multiple times if the attempt fails and is retried or if
  //   the system wakes for dark resume and then resuspends.
  // - After the suspend request is complete, Suspender::FinishRequest()
  //   calls UndoPrepareToSuspend().
  class Delegate {
   public:
    // Outcomes for a suspend attempt.
    enum class SuspendResult {
      // The system successfully suspended and resumed.
      SUCCESS = 0,
      // The kernel reported a (possibly transient) error while suspending.
      FAILURE,
      // The suspend attempt was canceled as a result of a wakeup event.
      CANCELED,
    };

    virtual ~Delegate() {}

    // Returns a initial value for suspend-related IDs that's likely (but not
    // guaranteed) to yield successive IDs that are unique across all of the
    // powerd runs during the current boot session.
    virtual int GetInitialSuspendId() = 0;

    // Returns an initial value for the dark suspend IDs that's likely (but not
    // guaranteed) to yield successive IDs that are unique across all of the
    // powerd runs during the current boot session.  Additionally, successive
    // IDs generated from this value should not collide with successive IDs
    // generated from the value returned by GetInitialSuspendId().
    virtual int GetInitialDarkSuspendId() = 0;

    // Is the lid currently closed?  Returns false if the query fails or if
    // the system doesn't have a lid.
    virtual bool IsLidClosedForSuspend() = 0;

    // Reads the current wakeup count from sysfs and stores it in
    // |wakeup_count|. Returns true on success.
    virtual bool ReadSuspendWakeupCount(uint64_t* wakeup_count) = 0;

    // Sets state that persists across powerd restarts but not across system
    // reboots to track whether a suspend requests's commencement was announced
    // (the SuspendImminent signal was emitted) but its completion wasn't (the
    // SuspendDone signal wasn't emitted).
    virtual void SetSuspendAnnounced(bool announced) = 0;

    // Gets the state previously set via SetSuspendAnnounced().
    virtual bool GetSuspendAnnounced() = 0;

    // Performs any work that needs to happen before other processes are
    // informed that the system is about to suspend, including turning off the
    // backlight and muting audio. Called by StartRequest().
    virtual void PrepareToSuspend() = 0;

    // Synchronously runs the powerd_suspend script to suspend the system for
    // |duration|. If |wakeup_count_valid| is true, passes |wakeup_count| to the
    // script so it can avoid suspending if additional wakeup events occur.
    // Called by Suspend().
    virtual SuspendResult DoSuspend(uint64_t wakeup_count,
                                    bool wakeup_count_valid,
                                    base::TimeDelta duration) = 0;

    // Undoes the preparations performed by PrepareToSuspend(). Called by
    // FinishRequest().
    virtual void UndoPrepareToSuspend(bool success,
                                      int num_suspend_attempts,
                                      bool canceled_while_in_dark_resume) = 0;

    // Generates and reports metrics for wakeups in dark resume.
    virtual void GenerateDarkResumeMetrics(
        const std::vector<DarkResumeInfo>& dark_resume_wake_durations,
        base::TimeDelta suspend_duration_) = 0;

    // Shuts the system down in response to repeated failed suspend attempts.
    virtual void ShutDownForFailedSuspend() = 0;

    // Shuts the system down in response to the DarkResumePolicy determining the
    // system should shut down.
    virtual void ShutDownForDarkResume() = 0;
  };

  // Helper class providing functionality needed by tests.
  class TestApi {
   public:
    explicit TestApi(Suspender* suspender);

    int suspend_id() const { return suspender_->suspend_request_id_; }
    int dark_suspend_id() const { return suspender_->dark_suspend_id_; }

    Clock* clock() const { return suspender_->clock_.get(); }
    SuspendDelayController* suspend_delay_controller() const {
      return suspender_->suspend_delay_controller_.get();
    }
    SuspendDelayController* dark_suspend_delay_controller() const {
      return suspender_->dark_suspend_delay_controller_.get();
    }

    // Runs Suspender::HandleEvent(EVENT_READY_TO_RESUSPEND) if
    // |resuspend_timer_| is running. Returns false otherwise.
    bool TriggerResuspendTimeout();

    void set_last_dark_resume_wake_reason(const std::string& wake_reason) {
      suspender_->last_dark_resume_wake_reason_ = wake_reason;
    }

    std::string GetDefaultWakeReason() const;

   private:
    Suspender* suspender_;  // weak

    DISALLOW_COPY_AND_ASSIGN(TestApi);
  };

  Suspender();
  ~Suspender() override;

  void Init(Delegate* delegate,
            system::DBusWrapperInterface* dbus_wrapper,
            system::DarkResumeInterface* dark_resume,
            PrefsInterface* prefs);

  // Starts the suspend process. Note that suspending happens
  // asynchronously.
  void RequestSuspend(SuspendImminent::Reason reason);

  // Like RequestSuspend(), but aborts the suspend attempt immediately if
  // the current wakeup count reported by the kernel exceeds
  // |wakeup_count|. Autotests can pass an external wakeup count to ensure
  // that machines in the test cluster don't sleep indefinitely (see
  // http://crbug.com/218175).
  // TODO(derat): Delete this and add a base::Optional<uint64_t> arg to
  // RequestSuspend once base::Optional is available.
  void RequestSuspendWithExternalWakeupCount(SuspendImminent::Reason reason,
                                             uint64_t wakeup_count);

  // Handlers for D-Bus messages.
  void RegisterSuspendDelay(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void UnregisterSuspendDelay(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void HandleSuspendReadiness(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void RegisterDarkSuspendDelay(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void UnregisterDarkSuspendDelay(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void HandleDarkSuspendReadiness(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void RecordDarkResumeWakeReason(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);

  // Handles the lid being opened, user activity, or the system shutting down,
  // any of which may abort an in-progress suspend attempt.
  void HandleLidOpened();
  void HandleUserActivity();
  void HandleShutdown();

  // Handles the D-Bus name |name| becoming owned by |new_owner| instead of
  // |old_owner|.
  void HandleDBusNameOwnerChanged(const std::string& name,
                                  const std::string& old_owner,
                                  const std::string& new_owner);

  // SuspendDelayObserver override:
  void OnReadyForSuspend(SuspendDelayController* controller,
                         int suspend_id) override;

 private:
  // States that Suspender can be in while the event loop is running.
  enum class State {
    // Nothing suspend-related is going on.
    IDLE = 0,
    // powerd has announced a new suspend request to other processes and is
    // waiting for clients that have registered suspend delays to report
    // readiness.
    WAITING_FOR_SUSPEND_DELAYS,
    // powerd is waiting to resuspend after a failed suspend attempt or after
    // waking into dark resume.
    WAITING_TO_RESUSPEND,
    // The system is shutting down. Suspend requests are ignored.
    SHUTTING_DOWN,
  };

  enum class Event {
    // A suspend request was received.
    SUSPEND_REQUESTED = 0,
    // Clients that have registered suspend delays have all reported readiness
    // (or timed out).
    SUSPEND_DELAYS_READY,
    // User activity was reported.
    USER_ACTIVITY,
    // The system is ready to resuspend (after either a failed suspend attempt
    // or a dark resume).
    READY_TO_RESUSPEND,
    // The system is shutting down.
    SHUTDOWN_STARTED,
  };

  // Internal functions that actually process the received D-Bus messages.
  void RegisterSuspendDelayInternal(
      SuspendDelayController* controller,
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void UnregisterSuspendDelayInternal(
      SuspendDelayController* controller,
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);
  void HandleSuspendReadinessInternal(
      SuspendDelayController* controller,
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);

  // Performs actions and updates |state_| in response to |event|.
  void HandleEvent(Event event);

  // Starts a new suspend request, notifying clients that have registered delays
  // that the system is about to suspend.
  void StartRequest();

  // Completes the current suspend request, undoing any work performed by
  // StartRequest().
  void FinishRequest(bool success);

  // Actually performs a suspend attempt and waits for the system to resume,
  // returning a new value for |state_|.
  State Suspend();

  // Helper methods called by Suspend() to handle various suspend results.
  State HandleNormalResume(Delegate::SuspendResult result);
  State HandleDarkResume(Delegate::SuspendResult result);

  // Helper method called by HandleNormalResume() and HandleDarkResume() in
  // response to a failed or canceled suspend attempt.
  State HandleUnsuccessfulSuspend(Delegate::SuspendResult result);

  // Starts |resuspend_timer_| to send EVENT_READY_TO_RESUSPEND after |delay|.
  void ScheduleResuspend(const base::TimeDelta& delay);

  // Emits D-Bus signal announcing the end of a suspend request.
  void EmitSuspendDoneSignal(int suspend_request_id,
                             const base::TimeDelta& suspend_duration);

  // Emits a D-Bus signal announcing that the system will soon resuspend from
  // dark resume. |dark_resume_id_| is used as the request ID.
  void EmitDarkSuspendImminentSignal();

  Delegate* delegate_ = nullptr;                          // weak
  system::DBusWrapperInterface* dbus_wrapper_ = nullptr;  // weak
  system::DarkResumeInterface* dark_resume_ = nullptr;    // weak

  std::unique_ptr<Clock> clock_;
  std::unique_ptr<SuspendDelayController> suspend_delay_controller_;
  std::unique_ptr<SuspendDelayController> dark_suspend_delay_controller_;

  // Current state of the object, updated just before returning control to the
  // event loop.
  State state_ = State::IDLE;

  // True if HandleEvent() is currently handling an event.
  bool handling_event_ = false;

  // True if HandleEvent() is currently processing |queued_events_|.
  bool processing_queued_events_ = false;

  // Unhandled events that were received while |handling_event_| was true.
  std::queue<Event> queued_events_;

  // Unique ID associated with the current suspend request.
  int suspend_request_id_ = 0;

  // Unique ID associated with the current dark suspend request.
  int dark_suspend_id_ = 0;

  // The reason that was supplied for the current suspend request.
  SuspendImminent::Reason suspend_request_reason_ =
      SuspendImminent_Reason_OTHER;

  // An optional wakeup count supplied via
  // RequestSuspendWithExternalWakeupCount().
  bool suspend_request_supplied_wakeup_count_ = false;
  uint64_t suspend_request_wakeup_count_ = 0;

  // Number of wakeup events received at the start of the current suspend
  // attempt. Passed to the kernel to cancel an attempt if user activity is
  // received while powerd's event loop isn't running.
  uint64_t wakeup_count_ = 0;
  bool wakeup_count_valid_ = false;

  // Wall time at which the suspend request started.
  base::Time suspend_request_start_time_;

  // Time to wait before retrying a failed suspend attempt.
  base::TimeDelta retry_delay_;

  // Maximum number of times to retry after a failed suspend attempt before
  // giving up and shutting down the system.
  int64_t max_retries_ = 0;

  // Number of suspend attempts made in the current series. Up to |max_retries_|
  // additional attempts are made after a failure, but this counter is reset
  // after waking into dark resume.
  int current_num_attempts_ = 0;

  // Number of suspend attempts made in the first series after the
  // RequestSuspend() call. |current_num_attempts_| is copied here when doing a
  // dark resume.
  int initial_num_attempts_ = 0;

  // The time at which the system entered dark resume. Set by HandleDarkResume()
  // when it sees a sucessful dark resume.
  base::Time dark_resume_start_time_;

  // Information about each wake that occurred during dark resume. This vector
  // is cleared by StartRequest() and reported by FinishRequest().
  //
  // HandleDarkResume() pushes a new entry when it sees a successful dark
  // resume, but the entry's wake reason and duration is updated by Suspend()
  // when it commences the next dark suspend cycle.
  std::vector<DarkResumeInfo> dark_resume_wake_durations_;

  // The wake reason for the last dark resume.
  std::string last_dark_resume_wake_reason_;

  // Runs HandleEvent(EVENT_READY_TO_RESUSPEND).
  base::OneShotTimer resuspend_timer_;

  DISALLOW_COPY_AND_ASSIGN(Suspender);
};

}  // namespace policy
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_POLICY_SUSPENDER_H_
