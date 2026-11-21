// Launch diagnostics: pure logic shared by the daemon and the host tests.
//
// Everything here works on caller-supplied data. Stages and outcomes only
// change on observed events (ShellCore bridge requests, lifecycle process
// events, sm_mdbg findings, kernel-log lines, console error-history files);
// a successful mount or registration never implies that a game ran.

#include "sm_diag_core.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <json-c/json.h>

static void copy_text(char *dst, size_t size, const char *src) {
  if (size == 0)
    return;
  if (!src)
    src = "";
  size_t length = strlen(src);
  if (length >= size)
    length = size - 1u;
  memcpy(dst, src, length);
  dst[length] = '\0';
}

static bool contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return false;
  size_t needle_len = strlen(needle);
  if (needle_len == 0)
    return true;
  for (const char *p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, needle_len) == 0)
      return true;
  }
  return false;
}

// --- Names ---

static const char *const k_state_names[] = {"not_observed", "pending", "ok",
                                            "failed"};
static const char *const k_stage_names[SM_DIAG_STAGE_COUNT] = {
    "mounted",       "registered",        "launch_requested", "sandbox_ready",
    "process_started", "running_confirmed", "exited"};
static const char *const k_outcome_names[] = {
    "launch_in_progress", "running",      "exited_normally", "mount_failed",
    "spawn_failed",       "exited_early", "crashed",         "launch_not_observed",
    "observation_interrupted"};

const char *sm_diag_state_name(sm_diag_state_t state) {
  return (unsigned)state < sizeof(k_state_names) / sizeof(k_state_names[0])
             ? k_state_names[state]
             : "not_observed";
}

const char *sm_diag_stage_name(sm_diag_stage_id_t stage) {
  return (unsigned)stage < SM_DIAG_STAGE_COUNT ? k_stage_names[stage] : "";
}

const char *sm_diag_outcome_name(sm_diag_outcome_t outcome) {
  return (unsigned)outcome < sizeof(k_outcome_names) / sizeof(k_outcome_names[0])
             ? k_outcome_names[outcome]
             : "launch_not_observed";
}

bool sm_diag_outcome_from_name(const char *name, sm_diag_outcome_t *out) {
  for (size_t i = 0; name && i < sizeof(k_outcome_names) / sizeof(k_outcome_names[0]); ++i) {
    if (strcmp(name, k_outcome_names[i]) == 0) {
      *out = (sm_diag_outcome_t)i;
      return true;
    }
  }
  return false;
}

static bool state_from_name(const char *name, sm_diag_state_t *out) {
  for (size_t i = 0; name && i < sizeof(k_state_names) / sizeof(k_state_names[0]); ++i) {
    if (strcmp(name, k_state_names[i]) == 0) {
      *out = (sm_diag_state_t)i;
      return true;
    }
  }
  return false;
}

sm_diag_policy_t sm_diag_default_policy(void) {
  return (sm_diag_policy_t){
      .running_confirm_seconds = SM_DIAG_DEFAULT_RUNNING_CONFIRM_SECONDS,
      .spawn_timeout_seconds = SM_DIAG_SPAWN_TIMEOUT_SECONDS,
      .launch_retry_grace_seconds = SM_DIAG_LAUNCH_RETRY_GRACE_SECONDS,
      .launch_retry_window_seconds = SM_DIAG_LAUNCH_RETRY_WINDOW_SECONDS,
  };
}

// --- Record state machine ---

void sm_diag_record_init(sm_diag_record_t *rec, uint64_t id,
                         const char *title_id, int64_t now_ms,
                         uint64_t now_mono_us) {
  memset(rec, 0, sizeof(*rec));
  rec->id = id;
  copy_text(rec->title_id, sizeof(rec->title_id), title_id);
  rec->managed = -1;
  rec->created_ms = now_ms;
  rec->created_mono_us = now_mono_us;
  rec->kstuff_loaded = -1;
  rec->hooks_installed = -1;
  rec->outcome = SM_DIAG_OUTCOME_LAUNCH_NOT_OBSERVED;
}

void sm_diag_record_add_event(sm_diag_record_t *rec, int64_t at_ms,
                              uint64_t mono_us, const char *event,
                              const char *fmt, ...) {
  if (rec->timeline_count >= SM_DIAG_MAX_TIMELINE) {
    // Keep the first entries (they explain the launch) and the newest one.
    memmove(&rec->timeline[SM_DIAG_MAX_TIMELINE / 2u],
            &rec->timeline[SM_DIAG_MAX_TIMELINE / 2u + 1u],
            (SM_DIAG_MAX_TIMELINE / 2u - 1u) * sizeof(rec->timeline[0]));
    rec->timeline_count = SM_DIAG_MAX_TIMELINE - 1u;
  }
  sm_diag_event_t *entry = &rec->timeline[rec->timeline_count++];
  memset(entry, 0, sizeof(*entry));
  entry->at_ms = at_ms;
  entry->mono_us = mono_us;
  copy_text(entry->event, sizeof(entry->event), event);
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->detail, sizeof(entry->detail), fmt, args);
    va_end(args);
  }
}

void sm_diag_record_set_stage(sm_diag_record_t *rec, sm_diag_stage_id_t stage,
                              sm_diag_state_t state, int64_t at_ms,
                              uint64_t mono_us, const char *fmt, ...) {
  if ((unsigned)stage >= SM_DIAG_STAGE_COUNT)
    return;
  sm_diag_stage_t *entry = &rec->stages[stage];
  entry->state = state;
  entry->at_ms = at_ms;
  entry->mono_us = mono_us;
  entry->detail[0] = '\0';
  if (fmt) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry->detail, sizeof(entry->detail), fmt, args);
    va_end(args);
  }
}

static bool has_fatal_finding(const sm_diag_record_t *rec) {
  for (size_t i = 0; i < rec->finding_count; ++i) {
    if (rec->findings[i].kind == SM_DIAG_FINDING_FATAL ||
        rec->findings[i].kind == SM_DIAG_FINDING_CRASH_FLAG)
      return true;
  }
  return false;
}

static const sm_diag_finding_t *first_fatal_finding(const sm_diag_record_t *rec) {
  for (size_t i = 0; i < rec->finding_count; ++i) {
    if (rec->findings[i].kind == SM_DIAG_FINDING_FATAL ||
        rec->findings[i].kind == SM_DIAG_FINDING_CRASH_FLAG)
      return &rec->findings[i];
  }
  return NULL;
}

static int64_t wall_at(const sm_diag_record_t *rec, uint64_t mono_us,
                       int64_t now_ms, uint64_t now_mono_us) {
  (void)rec;
  if (mono_us == 0 || now_mono_us == 0 || now_ms == 0)
    return now_ms;
  if (mono_us >= now_mono_us)
    return now_ms + (int64_t)((mono_us - now_mono_us) / 1000u);
  return now_ms - (int64_t)((now_mono_us - mono_us) / 1000u);
}

static sm_diag_outcome_t derive_outcome(const sm_diag_record_t *rec) {
  const sm_diag_stage_t *stages = rec->stages;
  if (stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_FAILED)
    return SM_DIAG_OUTCOME_MOUNT_FAILED;
  if (has_fatal_finding(rec))
    return SM_DIAG_OUTCOME_CRASHED;
  if (stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK) {
    return stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_OK
               ? SM_DIAG_OUTCOME_EXITED_NORMALLY
               : SM_DIAG_OUTCOME_EXITED_EARLY;
  }
  if (stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_FAILED)
    return SM_DIAG_OUTCOME_SPAWN_FAILED;
  if (stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK) {
    return stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_OK
               ? SM_DIAG_OUTCOME_RUNNING
               : SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS;
  }
  if (stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].state == SM_DIAG_OK ||
      stages[SM_DIAG_STAGE_SANDBOX_READY].state != SM_DIAG_NOT_OBSERVED ||
      rec->launch_failed)
    return SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS;
  return SM_DIAG_OUTCOME_LAUNCH_NOT_OBSERVED;
}

bool sm_diag_outcome_is_failure(sm_diag_outcome_t outcome) {
  return outcome == SM_DIAG_OUTCOME_MOUNT_FAILED ||
         outcome == SM_DIAG_OUTCOME_SPAWN_FAILED ||
         outcome == SM_DIAG_OUTCOME_EXITED_EARLY ||
         outcome == SM_DIAG_OUTCOME_CRASHED;
}

bool sm_diag_record_evaluate(sm_diag_record_t *rec,
                             const sm_diag_policy_t *policy, int64_t now_ms,
                             uint64_t now_mono_us) {
  if (rec->interrupted || rec->restored) {
    // Nothing more can be observed about a launch from a previous run.
    return false;
  }
  sm_diag_stage_t before[SM_DIAG_STAGE_COUNT];
  memcpy(before, rec->stages, sizeof(before));
  sm_diag_outcome_t old_outcome = rec->outcome;
  bool old_has_diagnosis = rec->has_diagnosis;
  char old_category[sizeof(rec->diagnosis.category)];
  memcpy(old_category, rec->diagnosis.category, sizeof(old_category));
  size_t old_evidence = rec->diagnosis.evidence_count;

  sm_diag_stage_t *stages = rec->stages;
  const uint64_t confirm_us = (uint64_t)policy->running_confirm_seconds * 1000000u;
  const uint64_t spawn_us = (uint64_t)policy->spawn_timeout_seconds * 1000000u;
  const uint64_t grace_us = (uint64_t)policy->launch_retry_grace_seconds * 1000000u;
  bool mount_failed = stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_FAILED;
  bool started = stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK;

  if (!started && stages[SM_DIAG_STAGE_PROCESS_STARTED].state != SM_DIAG_FAILED &&
      !mount_failed) {
    uint64_t reference = 0;
    if (stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].state == SM_DIAG_OK)
      reference = stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].mono_us;
    if (stages[SM_DIAG_STAGE_SANDBOX_READY].state != SM_DIAG_NOT_OBSERVED &&
        stages[SM_DIAG_STAGE_SANDBOX_READY].mono_us > reference)
      reference = stages[SM_DIAG_STAGE_SANDBOX_READY].mono_us;
    if (rec->launch_failed && now_mono_us >= rec->launch_failed_mono_us &&
        now_mono_us - rec->launch_failed_mono_us >= grace_us) {
      // ShellCore retries launchApp during a game switch; only a failure that
      // is not followed by another LAUNCH within the grace is final.
      sm_diag_record_set_stage(
          rec, SM_DIAG_STAGE_PROCESS_STARTED, SM_DIAG_FAILED,
          wall_at(rec, rec->launch_failed_mono_us, now_ms, now_mono_us),
          rec->launch_failed_mono_us,
          "ShellCore reported LAUNCH_FAILED and no game process was observed");
    } else if (reference != 0 && now_mono_us >= reference &&
               now_mono_us - reference >= spawn_us && !rec->launch_failed) {
      sm_diag_record_set_stage(
          rec, SM_DIAG_STAGE_PROCESS_STARTED, SM_DIAG_FAILED,
          wall_at(rec, reference + spawn_us, now_ms, now_mono_us),
          reference + spawn_us,
          "no game process within %u s of the launch request",
          policy->spawn_timeout_seconds);
    } else if (reference != 0 &&
               stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_NOT_OBSERVED) {
      stages[SM_DIAG_STAGE_PROCESS_STARTED].state = SM_DIAG_PENDING;
    }
    if (stages[SM_DIAG_STAGE_SANDBOX_READY].state == SM_DIAG_NOT_OBSERVED &&
        stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].state == SM_DIAG_OK &&
        stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_PENDING)
      stages[SM_DIAG_STAGE_SANDBOX_READY].state = SM_DIAG_PENDING;
  }
  if (stages[SM_DIAG_STAGE_PROCESS_STARTED].state != SM_DIAG_PENDING &&
      stages[SM_DIAG_STAGE_SANDBOX_READY].state == SM_DIAG_PENDING)
    stages[SM_DIAG_STAGE_SANDBOX_READY].state = SM_DIAG_NOT_OBSERVED;

  if (started) {
    sm_diag_stage_t *confirm = &stages[SM_DIAG_STAGE_RUNNING_CONFIRMED];
    uint64_t start = rec->process_start_mono_us;
    const sm_diag_finding_t *fatal = first_fatal_finding(rec);
    bool exited = stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK;
    if (confirm->state != SM_DIAG_OK && confirm->state != SM_DIAG_FAILED) {
      if (fatal) {
        sm_diag_record_set_stage(rec, SM_DIAG_STAGE_RUNNING_CONFIRMED,
                                 SM_DIAG_FAILED, fatal->at_ms, 0,
                                 "fatal finding before confirmation: %.100s",
                                 fatal->text);
      } else if (exited && rec->exit_mono_us >= start &&
                 rec->exit_mono_us - start < confirm_us) {
        sm_diag_record_set_stage(
            rec, SM_DIAG_STAGE_RUNNING_CONFIRMED, SM_DIAG_FAILED, rec->exit_ms,
            rec->exit_mono_us, "process exited after %.1f s (needs %u s)",
            (double)(rec->exit_mono_us - start) / 1000000.0,
            policy->running_confirm_seconds);
      } else if (now_mono_us >= start && now_mono_us - start >= confirm_us &&
                 !(rec->exit_pending && rec->exit_mono_us >= start &&
                   rec->exit_mono_us - start < confirm_us) &&
                 (!policy->require_alive_probe ||
                  rec->last_alive_mono_us >= start + confirm_us)) {
        sm_diag_record_set_stage(
            rec, SM_DIAG_STAGE_RUNNING_CONFIRMED, SM_DIAG_OK,
            wall_at(rec, start + confirm_us, now_ms, now_mono_us),
            start + confirm_us,
            "process alive for %u s without a fatal finding",
            policy->running_confirm_seconds);
      } else {
        confirm->state = SM_DIAG_PENDING;
      }
    }
    if (stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_NOT_OBSERVED)
      stages[SM_DIAG_STAGE_EXITED].state = SM_DIAG_PENDING;
  }

  rec->outcome = derive_outcome(rec);
  sm_diag_diagnosis_t diagnosis;
  rec->has_diagnosis = sm_diag_classify(rec, &diagnosis);
  if (rec->has_diagnosis)
    rec->diagnosis = diagnosis;
  else
    memset(&rec->diagnosis, 0, sizeof(rec->diagnosis));

  return memcmp(before, rec->stages, sizeof(before)) != 0 ||
         old_outcome != rec->outcome || old_has_diagnosis != rec->has_diagnosis ||
         strcmp(old_category, rec->diagnosis.category) != 0 ||
         old_evidence != rec->diagnosis.evidence_count;
}

static bool has_observed_evidence(const sm_diag_record_t *rec) {
  return rec->native_error_count > 0 || rec->finding_count > 0 ||
         rec->kernel_line_count > 0;
}

bool sm_diag_record_unfinished(const sm_diag_record_t *rec) {
  if (rec->outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED)
    return false;
  if (rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS ||
      rec->outcome == SM_DIAG_OUTCOME_RUNNING)
    return true;
  for (size_t i = 0; i < SM_DIAG_STAGE_COUNT; ++i) {
    if (rec->stages[i].state == SM_DIAG_PENDING)
      return true;
  }
  // sm_diag_record_interrupt() leaves no pending stage; an interrupted record
  // that kept an observed verdict (crashed) is final. Records written by
  // older builds (interrupted but still in progress or pending) were caught
  // above and are resolved once.
  if (rec->interrupted)
    return false;
  return rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK &&
         rec->stages[SM_DIAG_STAGE_EXITED].state != SM_DIAG_OK;
}

static void refresh_diagnosis(sm_diag_record_t *rec) {
  sm_diag_diagnosis_t diagnosis;
  rec->has_diagnosis = sm_diag_classify(rec, &diagnosis);
  if (rec->has_diagnosis)
    rec->diagnosis = diagnosis;
  else
    memset(&rec->diagnosis, 0, sizeof(rec->diagnosis));
}

void sm_diag_record_interrupt(sm_diag_record_t *rec, int64_t now_ms,
                              const char *reason) {
  bool started = rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK;
  for (size_t i = 0; i < SM_DIAG_STAGE_COUNT; ++i) {
    sm_diag_stage_t *stage = &rec->stages[i];
    bool after_start = i == SM_DIAG_STAGE_RUNNING_CONFIRMED || i == SM_DIAG_STAGE_EXITED;
    if (stage->state == SM_DIAG_PENDING ||
        (after_start && started && stage->state == SM_DIAG_NOT_OBSERVED))
      sm_diag_record_set_stage(rec, (sm_diag_stage_id_t)i, SM_DIAG_NOT_OBSERVED,
                               0, 0, "%s", reason);
  }
  rec->exit_pending = false;
  rec->interrupted = true;
  // An observed verdict (crashed, mount_failed, ...) stays; only a launch
  // without one becomes observation_interrupted.
  if (rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS ||
      rec->outcome == SM_DIAG_OUTCOME_RUNNING ||
      rec->outcome == SM_DIAG_OUTCOME_LAUNCH_NOT_OBSERVED)
    rec->outcome = SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED;
  refresh_diagnosis(rec);
  sm_diag_record_add_event(rec, now_ms, 0, "observation_interrupted", "%s",
                           reason);
}

// Map a wall-clock time of an earlier daemon run onto this run's monotonic
// clock; monotonic values of the earlier run are not comparable.
static uint64_t rebase_mono(int64_t at_ms, int64_t now_ms, uint64_t now_mono_us) {
  if (at_ms <= 0 || now_ms <= 0 || at_ms > now_ms)
    return now_mono_us;
  uint64_t age_us = (uint64_t)(now_ms - at_ms) * 1000u;
  return age_us < now_mono_us ? now_mono_us - age_us : 1u;
}

bool sm_diag_record_resolve_restored(sm_diag_record_t *rec,
                                     sm_diag_process_state_t state,
                                     int64_t now_ms, uint64_t now_mono_us) {
  if (!sm_diag_record_unfinished(rec))
    return false;
  // A process started before this boot cannot be the one now holding the PID
  // (the monotonic clock counts from boot), whatever its title and app id.
  int64_t start_ms = rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].at_ms;
  bool this_boot = start_ms > 0 && now_ms >= start_ms &&
                   (uint64_t)(now_ms - start_ms) * 1000u < now_mono_us;
  // UNKNOWN: the PID exists but its identity could not be read; keep it for
  // the runtime reconciliation to settle (bounded there).
  bool live = rec->pid > 0 && this_boot &&
              (state == SM_DIAG_PROC_SAME || state == SM_DIAG_PROC_UNKNOWN) &&
              rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK &&
              rec->stages[SM_DIAG_STAGE_EXITED].state != SM_DIAG_OK;
  if (!live) {
    sm_diag_record_interrupt(
        rec, now_ms,
        rec->pid > 0 ? "UniMount restarted before this was observed; the game "
                       "process is gone or was replaced"
                     : "UniMount restarted before this was observed");
    return false;
  }
  // The same application is still running: keep observing it.
  for (size_t i = 0; i < SM_DIAG_STAGE_COUNT; ++i) {
    if (rec->stages[i].at_ms > 0)
      rec->stages[i].mono_us = rebase_mono(rec->stages[i].at_ms, now_ms, now_mono_us);
  }
  for (size_t i = 0; i < rec->timeline_count; ++i)
    rec->timeline[i].mono_us = rebase_mono(rec->timeline[i].at_ms, now_ms, now_mono_us);
  rec->created_mono_us = rebase_mono(rec->created_ms, now_ms, now_mono_us);
  rec->process_start_mono_us = rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].mono_us;
  rec->exit_pending = false;
  rec->exit_mono_us = 0;
  rec->exit_ms = 0;
  rec->launch_failed = false;
  rec->launch_failed_mono_us = 0;
  rec->restored = false;
  rec->interrupted = false;
  // Only a probe that proved the identity counts as seen alive.
  rec->last_alive_mono_us = state == SM_DIAG_PROC_SAME ? now_mono_us : 0;
  rec->next_liveness_mono_us = 0;
  rec->gone_probes = 0;
  rec->unknown_probes = 0;
  rec->reattached = true;
  sm_diag_record_add_event(rec, now_ms, now_mono_us, "observation_resumed",
                           "pid=%ld app_id=0x%08X %s after restart",
                           (long)rec->pid, rec->app_id,
                           state == SM_DIAG_PROC_SAME
                               ? "still running"
                               : "still exists (identity not readable yet)");
  return true;
}

bool sm_diag_record_liveness_due(const sm_diag_record_t *rec,
                                 uint64_t now_mono_us) {
  return !rec->restored && !rec->interrupted && rec->pid > 0 &&
         rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK &&
         rec->stages[SM_DIAG_STAGE_EXITED].state != SM_DIAG_OK &&
         now_mono_us >= rec->next_liveness_mono_us;
}

static void conclude_exit(sm_diag_record_t *rec, uint64_t exit_mono,
                          int64_t exit_ms, const char *detail) {
  rec->exit_pending = false;
  rec->exit_mono_us = exit_mono;
  rec->exit_ms = exit_ms;
  sm_diag_record_set_stage(rec, SM_DIAG_STAGE_EXITED, SM_DIAG_OK, exit_ms,
                           exit_mono, "%s", detail);
  sm_diag_record_add_event(rec, exit_ms, exit_mono, "exit_reconciled", "%s",
                           detail);
}

static void conclude_vanished(sm_diag_record_t *rec,
                              const sm_diag_policy_t *policy, int64_t now_ms,
                              uint64_t now_mono_us);

bool sm_diag_record_note_liveness(sm_diag_record_t *rec,
                                  const sm_diag_policy_t *policy,
                                  sm_diag_process_state_t state, int64_t now_ms,
                                  uint64_t now_mono_us) {
  if (!sm_diag_record_liveness_due(rec, UINT64_MAX))
    return false;
  bool confirmed = rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_OK;
  // Probe about once a second until confirmed, then every five seconds.
  rec->next_liveness_mono_us = now_mono_us + (confirmed ? 5000000u : 1000000u);
  if (state == SM_DIAG_PROC_SAME || state == SM_DIAG_PROC_UNVERIFIED) {
    rec->last_alive_mono_us = now_mono_us;
    rec->gone_probes = 0;
    rec->unknown_probes = 0;
    return sm_diag_record_evaluate(rec, policy, now_ms, now_mono_us);
  }
  if (state == SM_DIAG_PROC_UNKNOWN) {
    // The PID exists but its identity could not be read: neither proof of
    // life (no confirmation) nor of an exit. Bounded: if it never becomes
    // readable, the launch cannot be followed and is interrupted.
    rec->gone_probes = 0;
    if (++rec->unknown_probes < SM_DIAG_UNKNOWN_PROBES)
      return false;
    sm_diag_record_interrupt(rec, now_ms,
                             "the game process could not be identified any more");
    return true;
  }
  rec->unknown_probes = 0;
  char detail[SM_DIAG_DETAIL_SIZE];
  if (rec->exit_pending) {
    // NOTE_EXIT was observed; the lifecycle's final exit normally follows after
    // its same-app handoff grace. Conclude only if it never does.
    uint64_t grace = (uint64_t)SM_DIAG_EXIT_FINAL_GRACE_SECONDS * 1000000u;
    if (now_mono_us < rec->exit_mono_us || now_mono_us - rec->exit_mono_us < grace)
      return false;
    snprintf(detail, sizeof(detail),
             "pid=%ld exited (NOTE_EXIT observed); final exit not reported",
             (long)rec->pid);
    conclude_exit(rec, rec->exit_mono_us, rec->exit_ms, detail);
    (void)sm_diag_record_evaluate(rec, policy, now_ms, now_mono_us);
    return true;
  }
  // Wait for a second "gone" probe: a same-app handoff announces itself first.
  if (++rec->gone_probes < SM_DIAG_GONE_PROBES)
    return false;
  conclude_vanished(rec, policy, now_ms, now_mono_us);
  return true;
}

static void conclude_vanished(sm_diag_record_t *rec,
                              const sm_diag_policy_t *policy, int64_t now_ms,
                              uint64_t now_mono_us) {
  const uint64_t confirm_us = (uint64_t)policy->running_confirm_seconds * 1000000u;
  const uint64_t start = rec->process_start_mono_us;
  sm_diag_state_t confirm_state = rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state;
  // ok or failed (fatal finding / early exit) is a decided verdict: keep it.
  bool decided = confirm_state == SM_DIAG_OK || confirm_state == SM_DIAG_FAILED;
  char detail[SM_DIAG_DETAIL_SIZE];
  // The process vanished without an observed exit: alive at last_alive,
  // gone now.
  uint64_t last_alive = rec->last_alive_mono_us > start ? rec->last_alive_mono_us : start;
  double alive_s = (double)(last_alive - start) / 1e6;
  double gone_s = now_mono_us > start ? (double)(now_mono_us - start) / 1e6 : 0.0;
  if (!decided && last_alive >= start + confirm_us) {
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_RUNNING_CONFIRMED, SM_DIAG_OK,
                             now_ms, start + confirm_us,
                             "process seen alive %.1f s after start", alive_s);
    decided = true;
  }
  if (decided || now_mono_us < start + confirm_us) {
    // Confirmed earlier: exited_normally (crashed when a fatal finding
    // decided it). Gone before the confirmation time: exited_early.
    snprintf(detail, sizeof(detail),
             "exit not observed directly: pid=%ld gone %.1f s after start "
             "(last seen alive at %.1f s)",
             (long)rec->pid, gone_s, alive_s);
    conclude_exit(rec, now_mono_us, now_ms, detail);
    (void)sm_diag_record_evaluate(rec, policy, now_ms, now_mono_us);
    return;
  }
  // Gone after the confirmation time without a probe proving it lived that
  // long: whether it ran long enough is unknown.
  snprintf(detail, sizeof(detail),
           "exit not observed directly: pid=%ld gone %.1f s after start "
           "(last seen alive at %.1f s)",
           (long)rec->pid, gone_s, alive_s);
  conclude_exit(rec, now_mono_us, now_ms, detail);
  sm_diag_record_interrupt(rec, now_ms,
                           "process gone before running could be confirmed or "
                           "refuted");
}

bool sm_diag_record_finalise_for_shutdown(sm_diag_record_t *rec,
                                          const sm_diag_policy_t *policy,
                                          sm_diag_process_state_t state,
                                          int64_t now_ms, uint64_t now_mono_us) {
  if (rec->restored || rec->pid <= 0 || !sm_diag_record_unfinished(rec))
    return false;
  // A process that still exists is saved in progress; the next start decides.
  if (state == SM_DIAG_PROC_SAME || state == SM_DIAG_PROC_UNVERIFIED ||
      state == SM_DIAG_PROC_UNKNOWN)
    return false;
  if (rec->exit_pending) {
    // NOTE_EXIT was observed while the lifecycle's handoff grace was running:
    // the exit is known, only its final confirmation was not.
    char detail[SM_DIAG_DETAIL_SIZE];
    snprintf(detail, sizeof(detail),
             "pid=%ld exited (NOTE_EXIT observed); UniMount stopped before the "
             "final exit",
             (long)rec->pid);
    conclude_exit(rec, rec->exit_mono_us, rec->exit_ms, detail);
    (void)sm_diag_record_evaluate(rec, policy, now_ms, now_mono_us);
    return true;
  }
  sm_diag_record_interrupt(rec, now_ms,
                           "UniMount stopped before this was observed; the "
                           "game process is gone");
  return true;
}

bool sm_diag_record_is_active(const sm_diag_record_t *rec) {
  if (rec->interrupted)
    return false;
  if (rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS ||
      rec->outcome == SM_DIAG_OUTCOME_RUNNING)
    return true;
  // A crashed process may still be alive until its exit is reported.
  return rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK &&
         rec->stages[SM_DIAG_STAGE_EXITED].state != SM_DIAG_OK;
}

// --- Store ---

bool sm_diag_store_init(sm_diag_store_t *store) {
  memset(store, 0, sizeof(*store));
  store->records = calloc(SM_DIAG_MAX_RECORDS, sizeof(*store->records));
  store->next_id = 1;
  return store->records != NULL;
}

void sm_diag_store_free(sm_diag_store_t *store) {
  free(store->records);
  memset(store, 0, sizeof(*store));
}

size_t sm_diag_store_count(const sm_diag_store_t *store) { return store->count; }

sm_diag_record_t *sm_diag_store_at(sm_diag_store_t *store, size_t index) {
  if (index >= store->count)
    return NULL;
  return &store->records[(store->start + index) % SM_DIAG_MAX_RECORDS];
}

sm_diag_record_t *sm_diag_store_find_id(sm_diag_store_t *store, uint64_t id) {
  for (size_t i = 0; i < store->count; ++i) {
    sm_diag_record_t *rec = sm_diag_store_at(store, i);
    if (rec->id == id)
      return rec;
  }
  return NULL;
}

sm_diag_record_t *sm_diag_store_latest_for_title(sm_diag_store_t *store,
                                                 const char *title_id) {
  for (size_t i = store->count; i > 0; --i) {
    sm_diag_record_t *rec = sm_diag_store_at(store, i - 1u);
    if (strcmp(rec->title_id, title_id) == 0)
      return rec;
  }
  return NULL;
}

sm_diag_record_t *sm_diag_store_append(sm_diag_store_t *store,
                                       const char *title_id, int64_t now_ms,
                                       uint64_t now_mono_us) {
  sm_diag_record_t *rec;
  if (store->count < SM_DIAG_MAX_RECORDS) {
    rec = &store->records[(store->start + store->count) % SM_DIAG_MAX_RECORDS];
    store->count++;
  } else {
    rec = &store->records[store->start];
    store->start = (store->start + 1u) % SM_DIAG_MAX_RECORDS;
  }
  sm_diag_record_init(rec, store->next_id++, title_id, now_ms, now_mono_us);
  return rec;
}

static uint64_t input_event_mono(const sm_diag_input_t *input) {
  return input->event_mono_us != 0 ? input->event_mono_us : input->mono_us;
}

// The newest record of title that is still waiting for its process.
static sm_diag_record_t *find_waiting_launch(sm_diag_store_t *store,
                                             const sm_diag_policy_t *policy,
                                             const sm_diag_input_t *input,
                                             bool allow_failed_retry) {
  for (size_t i = store->count; i > 0; --i) {
    sm_diag_record_t *rec = sm_diag_store_at(store, i - 1u);
    if (strcmp(rec->title_id, input->title_id) != 0)
      continue;
    if (rec->restored)
      return NULL;
    (void)sm_diag_record_evaluate(rec, policy, input->wall_ms, input->mono_us);
    if (rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK ||
        rec->pid > 0)
      return NULL;
    uint64_t window = (uint64_t)policy->launch_retry_window_seconds * 1000000u;
    bool recent = input->mono_us >= rec->created_mono_us &&
                  input->mono_us - rec->created_mono_us < window;
    if (rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS && recent)
      return rec;
    if (allow_failed_retry && rec->launch_failed && recent &&
        rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS)
      return rec;
    return NULL;
  }
  return NULL;
}

// The newest record of title that never had a process, whatever its verdict
// (a LAUNCH_FAILED normally follows a refused launch mount).
static sm_diag_record_t *find_unbound_launch(sm_diag_store_t *store,
                                             const sm_diag_policy_t *policy,
                                             const sm_diag_input_t *input) {
  sm_diag_record_t *rec = sm_diag_store_latest_for_title(store, input->title_id);
  uint64_t window = (uint64_t)policy->launch_retry_window_seconds * 1000000u;
  if (!rec || rec->restored || rec->pid > 0 || input->mono_us < rec->created_mono_us ||
      input->mono_us - rec->created_mono_us >= window)
    return NULL;
  return rec;
}

static sm_diag_record_t *find_by_pid(sm_diag_store_t *store, pid_t pid) {
  if (pid <= 0)
    return NULL;
  for (size_t i = store->count; i > 0; --i) {
    sm_diag_record_t *rec = sm_diag_store_at(store, i - 1u);
    if (rec->pid == pid && !rec->restored)
      return rec;
  }
  return NULL;
}

static void apply_mount_result(sm_diag_record_t *rec,
                               const sm_diag_input_t *input) {
  rec->mount_reported = true;
  rec->mount_status = input->status;
  rec->mount_errno = input->native_errno;
  copy_text(rec->mount_layer, sizeof(rec->mount_layer), input->layer);
  int64_t at = input->wall_ms;
  uint64_t mono = input->mono_us;
  // Source: sm_shellcore_service.c mount_managed_title_runtime(): only a
  // missing mount link (layer "mount_link", ENOENT) or an unsupported title
  // id (no layer, EINVAL) means "not managed here" (stock title). ENOENT from
  // any other layer, e.g. an offline Kraken .pkg, is a managed mount failure.
  bool unmanaged =
      (input->status == ENOENT && strcmp(input->layer, "mount_link") == 0) ||
      (input->status == EINVAL && input->layer[0] == '\0');
  if (unmanaged) {
    rec->managed = 0;
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_MOUNTED, SM_DIAG_NOT_OBSERVED,
                             0, 0,
                             "title is not managed by UniMount (status=%d)",
                             input->status);
  } else if (input->status == 0) {
    rec->managed = 1;
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_MOUNTED, SM_DIAG_OK, at, mono,
                             "launch mount ready");
  } else {
    rec->managed = 1;
    sm_diag_record_set_stage(
        rec, SM_DIAG_STAGE_MOUNTED, SM_DIAG_FAILED, at, mono,
        "launch mount failed: status=%d layer=%s native_errno=%d (%s)",
        input->status, input->layer[0] ? input->layer : "unknown",
        input->native_errno, strerror(input->native_errno ? input->native_errno
                                                          : input->status));
  }
}

sm_diag_record_t *sm_diag_store_apply(sm_diag_store_t *store,
                                      const sm_diag_policy_t *policy,
                                      const sm_diag_input_t *input) {
  sm_diag_record_t *rec = NULL;
  const int64_t at = input->wall_ms;
  const uint64_t mono = input->mono_us;
  switch (input->type) {
  case SM_DIAG_EV_LAUNCH:
    rec = find_waiting_launch(store, policy, input, true);
    if (rec) {
      rec->launch_attempts++;
      rec->launch_failed = false;
      rec->launch_failed_mono_us = 0;
      if (rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_PENDING)
        rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state = SM_DIAG_NOT_OBSERVED;
    } else {
      rec = sm_diag_store_append(store, input->title_id, at, mono);
      rec->launch_attempts = 1;
    }
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_LAUNCH_REQUESTED, SM_DIAG_OK, at,
                             mono, "ShellCore launchApp request #%u",
                             rec->launch_attempts);
    apply_mount_result(rec, input);
    sm_diag_record_add_event(rec, at, mono, "launch_requested",
                             "status=%d layer=%s errno=%d", input->status,
                             input->layer[0] ? input->layer : "-",
                             input->native_errno);
    break;
  case SM_DIAG_EV_LAUNCH_FAILED:
    rec = find_unbound_launch(store, policy, input);
    if (!rec)
      rec = sm_diag_store_append(store, input->title_id, at, mono);
    rec->launch_failed = true;
    rec->launch_failed_mono_us = mono;
    sm_diag_record_add_event(rec, at, mono, "launch_failed",
                             "ShellCore LAUNCH_FAILED (response status=%d)",
                             input->status);
    break;
  case SM_DIAG_EV_SANDBOX_READY:
    rec = find_waiting_launch(store, policy, input, true);
    if (!rec)
      rec = sm_diag_store_append(store, input->title_id, at, mono);
    if (input->status == 0) {
      sm_diag_record_set_stage(rec, SM_DIAG_STAGE_SANDBOX_READY, SM_DIAG_OK, at,
                               mono, "sandbox ready handled");
    } else {
      // The service still lets ShellCore continue (fail-open); record the
      // overlay failure but keep waiting for the process.
      sm_diag_record_set_stage(rec, SM_DIAG_STAGE_SANDBOX_READY, SM_DIAG_FAILED,
                               at, mono,
                               "sandbox overlay preparation failed: status=%d (%s)",
                               input->status, strerror(input->status));
    }
    sm_diag_record_add_event(rec, at, mono, "sandbox_ready", "status=%d",
                             input->status);
    break;
  case SM_DIAG_EV_PROCESS_STARTED: {
    rec = find_by_pid(store, input->pid);
    if (rec && (rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK ||
                rec->interrupted))
      rec = NULL;
    // Only a record re-attached after a restart may be re-reported for the
    // process it already owns; within one run the lifecycle watcher reports
    // each process once, so the same PID again is a new process.
    if (rec && rec->reattached && strcmp(rec->title_id, input->title_id) == 0 &&
        (rec->app_id == 0 || input->app_id == 0 || rec->app_id == input->app_id)) {
      if (rec->app_id == 0)
        rec->app_id = input->app_id;
      sm_diag_record_add_event(rec, at, mono, "process_seen_again",
                               "pid=%ld app_id=0x%08X", (long)input->pid,
                               input->app_id);
      break;
    }
    if (rec) {
      // A new process now holds the PID: the old one is gone and its exit was
      // missed.
      conclude_vanished(rec, policy, at, mono);
      sm_diag_record_add_event(rec, at, mono, "pid_reused",
                               "pid=%ld started again (new process)",
                               (long)input->pid);
    }
    rec = find_waiting_launch(store, policy, input, true);
    if (!rec) {
      // A spawn-timeout or LAUNCH_FAILED verdict can be overtaken by the
      // process itself; bind it to that record instead of hiding it.
      sm_diag_record_t *latest = sm_diag_store_latest_for_title(store, input->title_id);
      uint64_t window = (uint64_t)policy->launch_retry_window_seconds * 1000000u;
      // A record restored from launches.json belongs to an earlier process
      // and its monotonic times are not comparable.
      if (latest && !latest->restored && latest->pid <= 0 &&
          !latest->interrupted &&
          latest->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED &&
          mono >= latest->created_mono_us &&
          mono - latest->created_mono_us < 2u * window) {
        rec = latest;
        sm_diag_record_add_event(rec, at, mono, "verdict_revised",
                                 "process appeared after spawn_failed verdict");
      }
    }
    if (!rec)
      rec = sm_diag_store_append(store, input->title_id, at, mono);
    uint64_t start = input_event_mono(input);
    rec->pid = input->pid;
    rec->app_id = input->app_id;
    rec->process_start_mono_us = start;
    rec->launch_failed = false;
    rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state = SM_DIAG_NOT_OBSERVED;
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_PROCESS_STARTED, SM_DIAG_OK,
                             wall_at(rec, start, at, mono), start,
                             "pid=%ld app_id=0x%08X", (long)input->pid,
                             input->app_id);
    sm_diag_record_add_event(rec, wall_at(rec, start, at, mono), start,
                             "process_started", "pid=%ld app_id=0x%08X",
                             (long)input->pid, input->app_id);
    break;
  }
  case SM_DIAG_EV_PROCESS_EXIT:
    rec = find_by_pid(store, input->pid);
    if (!rec || rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK)
      return NULL;
    rec->exit_pending = true;
    rec->exit_mono_us = input_event_mono(input);
    rec->exit_ms = wall_at(rec, rec->exit_mono_us, at, mono);
    sm_diag_record_add_event(rec, rec->exit_ms, rec->exit_mono_us,
                             "process_exit", "pid=%ld (awaiting same-app handoff)",
                             (long)input->pid);
    break;
  case SM_DIAG_EV_PROCESS_HANDOFF:
    rec = find_by_pid(store, input->pid);
    // An exit already concluded (by reconciliation) is final.
    if (!rec || rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK)
      return NULL;
    rec->pid = input->new_pid;
    rec->exit_pending = false;
    rec->exit_mono_us = 0;
    rec->exit_ms = 0;
    // Probe the new PID from scratch; a stale gone count must not carry over.
    rec->gone_probes = 0;
    rec->unknown_probes = 0;
    rec->next_liveness_mono_us = 0;
    sm_diag_record_add_event(rec, at, mono, "process_handoff",
                             "pid=%ld -> pid=%ld", (long)input->pid,
                             (long)input->new_pid);
    break;
  case SM_DIAG_EV_PROCESS_FINAL: {
    rec = find_by_pid(store, input->pid);
    if (!rec || rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK)
      return NULL;
    uint64_t exit_mono = rec->exit_pending ? rec->exit_mono_us
                                           : input_event_mono(input);
    rec->exit_pending = false;
    rec->exit_mono_us = exit_mono;
    rec->exit_ms = wall_at(rec, exit_mono, at, mono);
    double alive = exit_mono >= rec->process_start_mono_us
                       ? (double)(exit_mono - rec->process_start_mono_us) / 1000000.0
                       : 0.0;
    sm_diag_record_set_stage(rec, SM_DIAG_STAGE_EXITED, SM_DIAG_OK, rec->exit_ms,
                             exit_mono, "pid=%ld exited after %.1f s",
                             (long)input->pid, alive);
    sm_diag_record_add_event(rec, rec->exit_ms, exit_mono, "process_finalized",
                             "pid=%ld alive=%.1f s", (long)input->pid, alive);
    break;
  }
  case SM_DIAG_EV_FINDING: {
    rec = find_by_pid(store, input->pid);
    if (!rec)
      return NULL;
    if (rec->finding_count < SM_DIAG_MAX_FINDINGS) {
      sm_diag_finding_t *finding = &rec->findings[rec->finding_count++];
      finding->kind = input->finding_kind;
      finding->at_ms = at;
      copy_text(finding->text, sizeof(finding->text), input->text);
    }
    sm_diag_record_add_event(rec, at, mono, "game_error", "%.140s", input->text);
    break;
  }
  default:
    return NULL;
  }
  (void)sm_diag_record_evaluate(rec, policy, at, mono);
  return rec;
}

// --- Classification ---

typedef struct {
  uint32_t code;
  const char *short_error;
  const char *category;
} native_code_rule_t;

static const native_code_rule_t k_native_codes[] = {
    // Source: upstream ShadowMountPlus PR #226 and issue #219 (and the #189
    // kernel log: "processSpawn() error: 0x80aa001a", errno=13): eboot.bin or
    // a module without execute permission.
    {0x80AA001Au, "CE-107750-0", "permission"},
    // Source: upstream ShadowMountPlus issues #196 and #216: 0x80940033 is the
    // launch refusal seen while the title's mount was busy ("launch mount
    // unavailable ... status=16 (Device busy)").
    {0x80940033u, "CE-105773-3", "mount_busy"},
    // Source: the console's own message for CE-105779-9 / -2137784263:
    // "Can't continue using this game or app. The data is corrupted."
    {0x80940039u, "CE-105779-9", "corrupt_data"},
};

const char *sm_diag_native_code_category(uint32_t code) {
  for (size_t i = 0; i < sizeof(k_native_codes) / sizeof(k_native_codes[0]); ++i) {
    if (k_native_codes[i].code == code)
      return k_native_codes[i].category;
  }
  return NULL;
}

static void add_evidence(sm_diag_diagnosis_t *out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void add_evidence(sm_diag_diagnosis_t *out, const char *fmt, ...) {
  if (out->evidence_count >= SM_DIAG_MAX_EVIDENCE)
    return;
  char text[SM_DIAG_TEXT_SIZE];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  for (size_t i = 0; i < out->evidence_count; ++i) {
    if (strcmp(out->evidence[i], text) == 0)
      return;
  }
  copy_text(out->evidence[out->evidence_count++], SM_DIAG_TEXT_SIZE, text);
}

static void set_advice(sm_diag_diagnosis_t *out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void set_advice(sm_diag_diagnosis_t *out, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(out->advice, sizeof(out->advice), fmt, args);
  va_end(args);
}

static void native_error_evidence(sm_diag_diagnosis_t *out,
                                  const sm_diag_native_error_t *error) {
  char hex[11] = "none";
  if (error->has_internal_code)
    sm_diag_format_code_hex(error->internal_code, hex);
  add_evidence(out, "%s: %s internalErrorCode=%" PRId32 " (%s) titleId=%s attribution=%s",
               error->file, error->short_error[0] ? error->short_error : "?",
               error->internal_code, hex, error->title_id, error->attribution);
}

// Strongest native error with a documented category; title attribution wins.
static const sm_diag_native_error_t *
find_native_error(const sm_diag_record_t *rec, const char *category) {
  const sm_diag_native_error_t *found = NULL;
  for (size_t i = 0; i < rec->native_error_count; ++i) {
    const sm_diag_native_error_t *error = &rec->native_errors[i];
    if (!error->has_internal_code)
      continue;
    const char *code_category =
        sm_diag_native_code_category((uint32_t)error->internal_code);
    if (!code_category || strcmp(code_category, category) != 0)
      continue;
    if (!found || (strcmp(error->attribution, "title_id") == 0 &&
                   strcmp(found->attribution, "title_id") != 0))
      found = error;
  }
  return found;
}

static const char *native_confidence(const sm_diag_native_error_t *error) {
  return strcmp(error->attribution, "title_id") == 0 ? "observed" : "inferred";
}

static bool is_rtld_text(const char *text) {
  return text && strstr(text, "[rtld]") != NULL;
}

typedef bool (*rtld_visit_fn)(const char *text, void *ctx);

// Visit every rtld text tied to this launch: mdbg findings and PID-attributed
// kernel-log lines.
static void visit_rtld_lines(const sm_diag_record_t *rec, rtld_visit_fn fn,
                             void *ctx) {
  for (size_t i = 0; i < rec->finding_count; ++i) {
    if (rec->findings[i].kind == SM_DIAG_FINDING_RTLD &&
        fn(rec->findings[i].text, ctx))
      return;
  }
  for (size_t i = 0; i < rec->kernel_line_count; ++i) {
    if (rec->kernel_lines[i].kind == SM_DIAG_KLINE_RTLD &&
        fn(rec->kernel_lines[i].text, ctx))
      return;
  }
}

static bool rule_mount_failed(const sm_diag_record_t *rec,
                              sm_diag_diagnosis_t *out) {
  // Source: this repo, sm_shellcore_service.c mount_managed_title_runtime().
  if (rec->stages[SM_DIAG_STAGE_MOUNTED].state != SM_DIAG_FAILED)
    return false;
  copy_text(out->confidence, sizeof(out->confidence), "observed");
  copy_text(out->rule, sizeof(out->rule), "daemon_launch_mount_failed");
  int error = rec->mount_errno ? rec->mount_errno : rec->mount_status;
  add_evidence(out, "launch mount status=%d native_errno=%d (%s) layer=%s",
               rec->mount_status, rec->mount_errno, strerror(error),
               rec->mount_layer[0] ? rec->mount_layer : "unknown");
  set_advice(out,
             "UniMount could not prepare the %s layer for this title (%s). "
             "Check that the source is reachable and not in use, then launch "
             "again; the debug log has the [SHELLCORE]/[IMG]/[LINK] lines.",
             rec->mount_layer[0] ? rec->mount_layer : "launch", strerror(error));
  return true;
}

typedef struct {
  sm_diag_diagnosis_t *out;
  char file[SM_DIAG_FILE_NAME_SIZE];
  bool found;
} permission_ctx_t;

static bool visit_permission(const char *text, void *opaque) {
  permission_ctx_t *ctx = opaque;
  // Source: upstream ShadowMountPlus PR #226: "self_load_shared_object: mount
  // flag / attribute error (path=...)" when a module is not executable.
  if (!strstr(text, "mount flag / attribute error"))
    return false;
  ctx->found = true;
  add_evidence(ctx->out, "%s", text);
  if (!ctx->file[0])
    (void)sm_diag_rtld_module(text, ctx->file);
  return false;
}

static bool rule_permission(const sm_diag_record_t *rec,
                            sm_diag_diagnosis_t *out) {
  permission_ctx_t ctx = {.out = out};
  bool observed = false;
  const sm_diag_native_error_t *error = find_native_error(rec, "permission");
  if (error) {
    native_error_evidence(out, error);
    ctx.found = true;
    observed = strcmp(error->attribution, "title_id") == 0;
  }
  for (size_t i = 0; i < rec->kernel_line_count; ++i) {
    const sm_diag_kline_t *line = &rec->kernel_lines[i];
    // Source: upstream ShadowMountPlus issue #189/#219 kernel log:
    // "[Syscore App] processSpawn() error: 0x80aa001a" (errno=13).
    if (line->kind == SM_DIAG_KLINE_SPAWN_ERROR &&
        contains_ci(line->text, "0x80aa001a")) {
      add_evidence(out, "%s", line->text);
      ctx.found = true;
      observed = observed || line->attributed;
    }
  }
  visit_rtld_lines(rec, visit_permission, &ctx);
  if (!ctx.found)
    return false;
  if (ctx.file[0])
    observed = true;
  copy_text(out->confidence, sizeof(out->confidence),
            observed ? "observed" : "inferred");
  copy_text(out->rule, sizeof(out->rule), "exec_permission_0x80AA001A");
  if (ctx.file[0]) {
    set_advice(out,
               "%s lacks execute permission. Give eboot.bin and every "
               ".prx/.sprx module mode 0755 (chmod 755). UniMount adds missing "
               "execute bits to folder games on writable storage at the next "
               "scan; images and read-only sources must be fixed at the source.",
               ctx.file);
  } else {
    set_advice(out,
               "The game executable or one of its modules lacks execute "
               "permission. Give eboot.bin and every .prx/.sprx module mode "
               "0755 (chmod 755). UniMount adds missing execute bits to folder "
               "games on writable storage at the next scan; images and "
               "read-only sources must be fixed at the source.");
  }
  return true;
}

typedef struct {
  const sm_diag_record_t *rec;
  sm_diag_diagnosis_t *out;
  char module[SM_DIAG_FILE_NAME_SIZE];
  bool found;
} fakelib_ctx_t;

static bool module_in_fakelib(const sm_diag_record_t *rec, const char *module) {
  for (size_t i = 0; i < rec->fakelib_file_count; ++i) {
    if (strcasecmp(rec->fakelib_files[i], module) == 0)
      return true;
  }
  return false;
}

static bool visit_fakelib(const char *text, void *opaque) {
  fakelib_ctx_t *ctx = opaque;
  char module[SM_DIAG_FILE_NAME_SIZE];
  if (!sm_diag_rtld_module(text, module) || !module_in_fakelib(ctx->rec, module))
    return false;
  add_evidence(ctx->out, "%s", text);
  if (!ctx->found)
    copy_text(ctx->module, sizeof(ctx->module), module);
  ctx->found = true;
  return false;
}

static bool rule_fakelib_conflict(const sm_diag_record_t *rec,
                                  sm_diag_diagnosis_t *out) {
  // Source: upstream ShadowMountPlus issue #227: the module from the fakelib
  // overlay is refused ("sceSblAuthMgrAuthHeader returned unexpected error 46",
  // "[rtld] ... B: error 46 (libSceAmpr.sprx)", 0xa0020104).
  if (rec->fakelib_file_count == 0)
    return false;
  fakelib_ctx_t ctx = {.rec = rec, .out = out};
  visit_rtld_lines(rec, visit_fakelib, &ctx);
  if (!ctx.found)
    return false;
  for (size_t i = 0; i < rec->kernel_line_count; ++i) {
    if (rec->kernel_lines[i].kind == SM_DIAG_KLINE_AUTH ||
        (rec->kernel_lines[i].kind == SM_DIAG_KLINE_EXCEPTION &&
         contains_ci(rec->kernel_lines[i].text, "0xa0020104")))
      add_evidence(out, "%s", rec->kernel_lines[i].text);
  }
  add_evidence(out, "%s came from the fakelib overlay %s", ctx.module,
               rec->fakelib_source[0] ? rec->fakelib_source : "(source unknown)");
  copy_text(out->confidence, sizeof(out->confidence), "observed");
  copy_text(out->rule, sizeof(out->rule), "failing_module_from_fakelib");
  set_advice(out,
             "%s was supplied by the fakelib overlay mounted for this launch "
             "and the loader refused it. Change this title's fakelib mode "
             "(for example emulators-only or disabled) or replace %s in the "
             "fakelib source.",
             ctx.module, ctx.module);
  return true;
}

typedef struct {
  sm_diag_diagnosis_t *out;
  bool found;
} text_match_ctx_t;

static bool text_names_unresolved(const char *text) {
  // Source: upstream ShadowMountPlus issue #197: "0xA0020101
  // PRX_NOT_RESOLVED_FUNCTION" for a title requiring newer firmware.
  return contains_ci(text, "PRX_NOT_RESOLVED_FUNCTION") ||
         contains_ci(text, "0xa0020101");
}

static bool visit_unresolved(const char *text, void *opaque) {
  text_match_ctx_t *ctx = opaque;
  if (text_names_unresolved(text)) {
    add_evidence(ctx->out, "%s", text);
    ctx->found = true;
  }
  return false;
}

static bool rule_firmware(const sm_diag_record_t *rec,
                          sm_diag_diagnosis_t *out) {
  text_match_ctx_t ctx = {.out = out};
  visit_rtld_lines(rec, visit_unresolved, &ctx);
  for (size_t i = 0; i < rec->finding_count; ++i) {
    if (rec->findings[i].kind != SM_DIAG_FINDING_RTLD &&
        text_names_unresolved(rec->findings[i].text)) {
      add_evidence(out, "%s", rec->findings[i].text);
      ctx.found = true;
    }
  }
  for (size_t i = 0; i < rec->kernel_line_count; ++i) {
    if (rec->kernel_lines[i].kind == SM_DIAG_KLINE_EXCEPTION &&
        text_names_unresolved(rec->kernel_lines[i].text)) {
      add_evidence(out, "%s", rec->kernel_lines[i].text);
      ctx.found = true;
    }
  }
  bool requirement_higher =
      rec->has_required_fw && rec->console_fw_raw != 0 &&
      sm_diag_compare_fw(rec->required_fw_bcd, rec->console_fw_raw) > 0;
  if (!ctx.found && !requirement_higher)
    return false;
  char required[8] = "";
  char console[8] = "";
  if (rec->has_required_fw)
    sm_diag_format_fw_bcd(rec->required_fw_bcd, required);
  if (rec->console_fw_raw != 0)
    sm_diag_format_fw_bcd((rec->console_fw_raw >> 16) & 0xffffu, console);
  if (requirement_higher)
    add_evidence(out,
                 "sce_sys/param.json requiredSystemSoftwareVersion=%s (%s) > "
                 "console firmware %s",
                 rec->required_fw_raw, required, console);
  copy_text(out->confidence, sizeof(out->confidence),
            ctx.found ? "observed" : "inferred");
  copy_text(out->rule, sizeof(out->rule),
            ctx.found ? "prx_not_resolved_function" : "param_json_firmware");
  set_advice(out,
             "This game build needs newer system software%s%s%s. Use a build "
             "made for firmware %s or older, or a backport placed in "
             "/data/homebrew/backports/%s/.",
             requirement_higher ? " (" : "", requirement_higher ? required : "",
             requirement_higher ? ")" : "", console[0] ? console : "this",
             rec->title_id);
  return true;
}

typedef struct {
  sm_diag_diagnosis_t *out;
  char modules[4][SM_DIAG_FILE_NAME_SIZE];
  size_t module_count;
} missing_ctx_t;

static bool visit_missing(const char *text, void *opaque) {
  missing_ctx_t *ctx = opaque;
  if (!is_rtld_text(text) || strstr(text, "mount flag / attribute error"))
    return false;
  char module[SM_DIAG_FILE_NAME_SIZE];
  if (!sm_diag_rtld_module(text, module) ||
      (!contains_ci(module, ".prx") && !contains_ci(module, ".sprx")))
    return false;
  add_evidence(ctx->out, "%s", text);
  for (size_t i = 0; i < ctx->module_count; ++i) {
    if (strcasecmp(ctx->modules[i], module) == 0)
      return false;
  }
  if (ctx->module_count < 4u)
    copy_text(ctx->modules[ctx->module_count++], SM_DIAG_FILE_NAME_SIZE, module);
  return false;
}

static bool rule_missing_library(const sm_diag_record_t *rec,
                                 sm_diag_diagnosis_t *out) {
  // Source: upstream ShadowMountPlus issue #227 rtld format:
  // "[rtld] <pid> ERROR do_load_object:4355: C: Load error (libSceAmpr.sprx)".
  missing_ctx_t ctx = {.out = out};
  visit_rtld_lines(rec, visit_missing, &ctx);
  if (ctx.module_count == 0)
    return false;
  char list[4u * SM_DIAG_FILE_NAME_SIZE] = "";
  for (size_t i = 0; i < ctx.module_count; ++i) {
    if (i)
      strncat(list, ", ", sizeof(list) - strlen(list) - 1u);
    strncat(list, ctx.modules[i], sizeof(list) - strlen(list) - 1u);
  }
  copy_text(out->confidence, sizeof(out->confidence), "observed");
  copy_text(out->rule, sizeof(out->rule), "rtld_module_load_error");
  set_advice(out,
             "The loader could not load: %s. Check that the game folder or "
             "image is complete (sce_module/), and whether this title needs "
             "a fakelib or backport that provides it.",
             list);
  return true;
}

static bool rule_native_code(const sm_diag_record_t *rec,
                             sm_diag_diagnosis_t *out, const char *category,
                             const char *rule, const char *advice) {
  const sm_diag_native_error_t *error = find_native_error(rec, category);
  if (!error)
    return false;
  native_error_evidence(out, error);
  if (error->message[0])
    add_evidence(out, "messageBuffer: %s", error->message);
  copy_text(out->confidence, sizeof(out->confidence), native_confidence(error));
  copy_text(out->rule, sizeof(out->rule), rule);
  set_advice(out, "%s", advice);
  return true;
}

static bool rule_mount_busy(const sm_diag_record_t *rec,
                            sm_diag_diagnosis_t *out) {
  // Source: upstream ShadowMountPlus issues #196/#216 (0x80940033).
  return rule_native_code(
      rec, out, "mount_busy", "native_0x80940033",
      "The title's mount point was busy, typically because the previous app "
      "was still closing. Close the other app, wait a few seconds and launch "
      "again.");
}

static bool rule_corrupt_data(const sm_diag_record_t *rec,
                              sm_diag_diagnosis_t *out) {
  // Source: console message for CE-105779-9 (0x80940039): "The data is
  // corrupted".
  return rule_native_code(
      rec, out, "corrupt_data", "native_0x80940039",
      "The console judged the game data corrupted or unreadable. Verify or "
      "re-copy the source; for game folders check for an incomplete copy "
      "(missing or zero-sized files).");
}

static bool rule_kernel_patch(const sm_diag_record_t *rec,
                              sm_diag_diagnosis_t *out) {
  // Source: this repo: main.c waits for kstuff (KEKCALL_CHECK) before
  // starting services, and launch mounts depend on the ShellCore hooks.
  if (rec->kstuff_loaded != 0 && rec->hooks_installed != 0)
    return false;
  if (rec->kstuff_loaded == 0)
    add_evidence(out, "kstuff KEKCALL_CHECK failed when the launch was observed");
  if (rec->hooks_installed == 0)
    add_evidence(out, "ShellCore hooks were not installed when the launch was observed");
  copy_text(out->confidence, sizeof(out->confidence), "inferred");
  copy_text(out->rule, sizeof(out->rule), "kernel_patch_state");
  set_advice(out,
             "Load kstuff (for example through PS5 Payload Manager autoload) "
             "before UniMount and restart UniMount so the ShellCore hooks "
             "can be installed.");
  return true;
}

typedef bool (*classify_rule_fn)(const sm_diag_record_t *rec,
                                 sm_diag_diagnosis_t *out);

typedef struct {
  const char *category;
  classify_rule_fn match;
} classify_rule_t;

// First matching rule wins. Every rule carries its source next to its code.
static const classify_rule_t k_classify_rules[] = {
    {"mount_failed", rule_mount_failed},
    {"permission", rule_permission},
    {"fakelib_conflict", rule_fakelib_conflict},
    {"firmware_incompatible", rule_firmware},
    {"missing_library", rule_missing_library},
    {"mount_busy", rule_mount_busy},
    {"corrupt_data", rule_corrupt_data},
    {"kernel_patch", rule_kernel_patch},
};

static void collect_raw_evidence(const sm_diag_record_t *rec,
                                 sm_diag_diagnosis_t *out) {
  for (size_t i = 0; i < rec->native_error_count; ++i)
    native_error_evidence(out, &rec->native_errors[i]);
  for (size_t i = 0; i < rec->finding_count; ++i)
    add_evidence(out, "%s", rec->findings[i].text);
  for (size_t i = 0; i < rec->kernel_line_count; ++i)
    add_evidence(out, "%s", rec->kernel_lines[i].text);
  for (size_t i = 0; i < SM_DIAG_STAGE_COUNT; ++i) {
    if (rec->stages[i].state == SM_DIAG_FAILED)
      add_evidence(out, "%s: %s", sm_diag_stage_name((sm_diag_stage_id_t)i),
                   rec->stages[i].detail);
  }
}

bool sm_diag_classify(const sm_diag_record_t *rec, sm_diag_diagnosis_t *out) {
  memset(out, 0, sizeof(*out));
  // An interrupted launch keeps a diagnosis only for evidence observed before
  // the interruption (native error, finding, kernel-log line).
  if (!sm_diag_outcome_is_failure(rec->outcome) &&
      !(rec->outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED &&
        has_observed_evidence(rec)))
    return false;
  for (size_t i = 0; i < sizeof(k_classify_rules) / sizeof(k_classify_rules[0]); ++i) {
    sm_diag_diagnosis_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (k_classify_rules[i].match(rec, &candidate)) {
      *out = candidate;
      copy_text(out->category, sizeof(out->category), k_classify_rules[i].category);
      return true;
    }
  }
  copy_text(out->category, sizeof(out->category), "unknown");
  copy_text(out->confidence, sizeof(out->confidence), "inferred");
  copy_text(out->rule, sizeof(out->rule), "no_rule_matched");
  collect_raw_evidence(rec, out);
  set_advice(out,
             "No documented cause matched. Keep this record together with "
             "debug.log and a kernel log of the attempt when reporting it.");
  return true;
}

// --- Native error history ---

void sm_diag_format_code_hex(int32_t code, char out[11]) {
  snprintf(out, 11, "0x%08" PRIX32, (uint32_t)code);
}

static void json_copy_string(struct json_object *object, const char *key,
                             char *out, size_t size) {
  struct json_object *value = NULL;
  if (json_object_object_get_ex(object, key, &value) &&
      json_object_is_type(value, json_type_string))
    copy_text(out, size, json_object_get_string(value));
}

static bool json_get_int64(struct json_object *object, const char *key,
                           int64_t *out) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value))
    return false;
  if (json_object_is_type(value, json_type_int)) {
    *out = json_object_get_int64(value);
    return true;
  }
  return false;
}

static struct json_object *parse_json_text(const char *data, size_t size) {
  if (!data)
    return NULL;
  // UTF-8 byte order mark written by the console.
  if (size >= 3u && (unsigned char)data[0] == 0xEF &&
      (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
    data += 3;
    size -= 3u;
  }
  if (size == 0 || size > (size_t)INT32_MAX)
    return NULL;
  struct json_tokener *tokener = json_tokener_new();
  if (!tokener)
    return NULL;
  struct json_object *object = json_tokener_parse_ex(tokener, data, (int)size);
  if (!object && json_tokener_get_error(tokener) == json_tokener_continue)
    object = NULL;
  json_tokener_free(tokener);
  if (object && !json_object_is_type(object, json_type_object)) {
    json_object_put(object);
    return NULL;
  }
  return object;
}

int sm_diag_history_compare(const void *a, const void *b) {
  const sm_diag_history_entry_t *left = a;
  const sm_diag_history_entry_t *right = b;
  if (left->mtime != right->mtime)
    return left->mtime < right->mtime ? 1 : -1;
  return -strcmp(left->name, right->name);
}

void sm_diag_history_keep_newest(sm_diag_history_entry_t *entries,
                                 size_t *count, size_t capacity,
                                 const sm_diag_history_entry_t *candidate) {
  if (capacity == 0)
    return;
  if (*count < capacity) {
    entries[(*count)++] = *candidate;
    return;
  }
  size_t oldest = 0;
  for (size_t i = 1; i < *count; ++i)
    if (sm_diag_history_compare(&entries[i], &entries[oldest]) > 0)
      oldest = i;
  if (sm_diag_history_compare(candidate, &entries[oldest]) < 0)
    entries[oldest] = *candidate;
}

bool sm_diag_parse_native_error(const char *data, size_t size,
                                sm_diag_native_error_t *out) {
  char file[SM_DIAG_FILE_NAME_SIZE];
  int64_t mtime = out->mtime;
  copy_text(file, sizeof(file), out->file);
  memset(out, 0, sizeof(*out));
  copy_text(out->file, sizeof(out->file), file);
  out->mtime = mtime;
  struct json_object *object = parse_json_text(data, size);
  if (!object)
    return false;
  out->parsed = true;
  json_copy_string(object, "shortError", out->short_error, sizeof(out->short_error));
  json_copy_string(object, "messageBuffer", out->message, sizeof(out->message));
  json_copy_string(object, "titleId", out->title_id, sizeof(out->title_id));
  json_copy_string(object, "displayVersion", out->display_version,
                   sizeof(out->display_version));
  int64_t value = 0;
  if (json_get_int64(object, "internalErrorCode", &value) &&
      value >= INT32_MIN && value <= (int64_t)UINT32_MAX) {
    out->has_internal_code = true;
    out->internal_code = (int32_t)(uint32_t)(value < 0 ? (uint32_t)(int32_t)value
                                                       : (uint32_t)value);
  }
  struct json_object *tick = NULL;
  if (json_object_object_get_ex(object, "tick", &tick) &&
      json_object_is_type(tick, json_type_int)) {
    out->has_tick = true;
    out->tick = json_object_get_uint64(tick);
  }
  if (json_get_int64(object, "__appId", &value)) {
    out->has_app_id = true;
    out->app_id = value;
  }
  json_object_put(object);
  return true;
}

struct json_object *sm_diag_native_error_to_json(
    const sm_diag_native_error_t *error) {
  struct json_object *object = json_object_new_object();
  if (!object)
    return NULL;
  char hex[11];
  char mtime[32];
  sm_diag_format_time(error->mtime * 1000, mtime);
  json_object_object_add(object, "file", json_object_new_string(error->file));
  json_object_object_add(object, "file_mtime",
                         error->mtime ? json_object_new_string(mtime) : NULL);
  json_object_object_add(object, "file_mtime_unix", json_object_new_int64(error->mtime));
  json_object_object_add(object, "parsed", json_object_new_boolean(error->parsed));
  if (error->parsed) {
    json_object_object_add(object, "short_error",
                           json_object_new_string(error->short_error));
    if (error->has_internal_code) {
      sm_diag_format_code_hex(error->internal_code, hex);
      json_object_object_add(object, "internal_error_code",
                             json_object_new_int64(error->internal_code));
      json_object_object_add(object, "internal_error_code_hex",
                             json_object_new_string(hex));
    } else {
      json_object_object_add(object, "internal_error_code", NULL);
      json_object_object_add(object, "internal_error_code_hex", NULL);
    }
    json_object_object_add(object, "message", json_object_new_string(error->message));
    json_object_object_add(object, "title_id", json_object_new_string(error->title_id));
    json_object_object_add(object, "display_version",
                           json_object_new_string(error->display_version));
    json_object_object_add(object, "tick",
                           error->has_tick ? json_object_new_uint64(error->tick) : NULL);
    json_object_object_add(object, "app_id",
                           error->has_app_id ? json_object_new_int64(error->app_id) : NULL);
    const char *category = error->has_internal_code
                               ? sm_diag_native_code_category((uint32_t)error->internal_code)
                               : NULL;
    json_object_object_add(object, "category",
                           category ? json_object_new_string(category) : NULL);
  }
  if (error->attribution[0])
    json_object_object_add(object, "attribution",
                           json_object_new_string(error->attribution));
  return object;
}

// --- param.json firmware requirement ---

bool sm_diag_parse_required_fw(const char *data, size_t size,
                               uint32_t *bcd_out, char raw_out[24]) {
  struct json_object *object = parse_json_text(data, size);
  if (!object)
    return false;
  struct json_object *value = NULL;
  bool ok = false;
  uint64_t number = 0;
  if (json_object_object_get_ex(object, "requiredSystemSoftwareVersion", &value)) {
    if (json_object_is_type(value, json_type_string)) {
      const char *text = json_object_get_string(value);
      char *end = NULL;
      if (text && text[0]) {
        number = strtoull(text, &end, 16);
        ok = end && end != text && *end == '\0';
        copy_text(raw_out, 24, text);
      }
    } else if (json_object_is_type(value, json_type_int)) {
      number = json_object_get_uint64(value);
      snprintf(raw_out, 24, "0x%016" PRIX64, number);
      ok = true;
    }
  }
  json_object_put(object);
  if (!ok)
    return false;
  uint32_t bcd = (uint32_t)(number >> 48) & 0xffffu;
  // Every nibble of a BCD version is a decimal digit.
  for (unsigned shift = 0; shift < 16u; shift += 4u) {
    if (((bcd >> shift) & 0xfu) > 9u)
      return false;
  }
  *bcd_out = bcd;
  return true;
}

int sm_diag_compare_fw(uint32_t required_bcd, uint32_t console_raw) {
  uint32_t console_bcd = (console_raw >> 16) & 0xffffu;
  // Packed BCD keeps decimal order when compared as an integer.
  if (required_bcd > console_bcd)
    return 1;
  return required_bcd < console_bcd ? -1 : 0;
}

void sm_diag_format_fw_bcd(uint32_t bcd, char out[8]) {
  snprintf(out, 8, "%x.%02x", (bcd >> 8) & 0xffu, bcd & 0xffu);
}

// --- Kernel log ---

size_t sm_diag_log_offset_after_anchor(const char *text, size_t size,
                                       const char *anchor, size_t anchor_size) {
  if (!text || !anchor || anchor_size == 0 || anchor_size > size)
    return 0;
  for (size_t i = 0; i + anchor_size <= size; ++i) {
    if (text[i] == anchor[0] && memcmp(text + i, anchor, anchor_size) == 0)
      return i + anchor_size;
  }
  return 0;
}

static bool read_title_token(const char *start, char out[SM_DIAG_TITLE_ID_SIZE]) {
  while (*start == ' ' || *start == '=')
    ++start;
  size_t length = 0;
  while (start[length] && isalnum((unsigned char)start[length]) &&
         length + 1u < SM_DIAG_TITLE_ID_SIZE)
    ++length;
  if (length == 0)
    return false;
  memcpy(out, start, length);
  out[length] = '\0';
  return true;
}

static void add_kline(sm_diag_kline_t *out, size_t max, size_t *count, int kind,
                      bool attributed, const char *line) {
  for (size_t i = 0; i < *count; ++i) {
    if (strcmp(out[i].text, line) == 0) {
      out[i].attributed = out[i].attributed || attributed;
      return;
    }
  }
  if (*count >= max)
    return;
  out[*count].kind = kind;
  out[*count].attributed = attributed;
  copy_text(out[*count].text, sizeof(out[*count].text), line);
  (*count)++;
}

size_t sm_diag_scan_kernel_log(const char *text, size_t size,
                               const char *title_id, pid_t pid,
                               sm_diag_kline_t *out, size_t max) {
  size_t count = 0;
  char current_title[SM_DIAG_TITLE_ID_SIZE] = "";
  char pending_exception[SM_DIAG_TEXT_SIZE] = "";
  char line[512];
  size_t pos = 0;
  while (text && pos < size) {
    size_t end = pos;
    while (end < size && text[end] != '\n' && text[end] != '\0')
      ++end;
    size_t length = end - pos;
    if (length >= sizeof(line))
      length = sizeof(line) - 1u;
    memcpy(line, text + pos, length);
    line[length] = '\0';
    while (length > 0 && (line[length - 1u] == '\r' || line[length - 1u] == ' '))
      line[--length] = '\0';
    pos = end + 1u;
    if (length == 0)
      continue;

    // Source: kernel log in upstream ShadowMountPlus issue #227:
    // "[Syscore App] createApp PPSA15035" and "appParam: ... titleId = ...".
    const char *marker = strstr(line, "createApp ");
    if (marker)
      (void)read_title_token(marker + strlen("createApp "), current_title);
    marker = strstr(line, "titleId = ");
    if (marker)
      (void)read_title_token(marker + strlen("titleId = "), current_title);

    // Source: upstream ShadowMountPlus issue #189 (kernel log): "[Syscore
    // App] An error occured while creating new process. error=0x80aa001a
    // errno=13" and "processSpawn() error: 0x80aa001a".
    if (strstr(line, "processSpawn() error") ||
        strstr(line, "error occured while creating new process")) {
      if (current_title[0] && strcmp(current_title, title_id) != 0)
        continue; // Another application's spawn.
      add_kline(out, max, &count, SM_DIAG_KLINE_SPAWN_ERROR,
                current_title[0] != '\0', line);
      continue;
    }

    // Source: sm_mdbg.c rtld prefix "[rtld] <pid> ERROR" and issue #227.
    const char *rtld = strstr(line, "[rtld]");
    if (rtld && strstr(rtld, "ERROR")) {
      if (pid <= 0)
        continue; // No process of this launch exists to load modules.
      const char *open = strchr(rtld, '<');
      if (open && open < strstr(rtld, "ERROR")) {
        long line_pid = strtol(open + 1, NULL, 10);
        if (line_pid != (long)pid)
          continue;
        add_kline(out, max, &count, SM_DIAG_KLINE_RTLD, true, line);
      } else {
        add_kline(out, max, &count, SM_DIAG_KLINE_RTLD, false, line);
      }
      continue;
    }

    // Source: upstream ShadowMountPlus issue #227: "[KERNEL]
    // self_pager_activate:857: sceSblAuthMgrAuthHeader returned unexpected
    // error 46" (the line carries no PID).
    if (strstr(line, "sceSblAuthMgrAuthHeader")) {
      if (pid > 0)
        add_kline(out, max, &count, SM_DIAG_KLINE_AUTH, false, line);
      continue;
    }

    // Source: sm_mdbg.c fatal report format ("# exception: ...", then
    // "# proc ID: <pid>").
    if (strncmp(line, "# exception: ", 13) == 0) {
      copy_text(pending_exception, sizeof(pending_exception), line);
      continue;
    }
    if (pending_exception[0] && strncmp(line, "# proc ID: ", 11) == 0) {
      long line_pid = strtol(line + 11, NULL, 10);
      if (pid > 0 && line_pid == (long)pid)
        add_kline(out, max, &count, SM_DIAG_KLINE_EXCEPTION, true,
                  pending_exception);
      pending_exception[0] = '\0';
      continue;
    }
    if (line[0] != '#')
      pending_exception[0] = '\0';
  }
  return count;
}

bool sm_diag_rtld_module(const char *line, char out[SM_DIAG_FILE_NAME_SIZE]) {
  out[0] = '\0';
  if (!line)
    return false;
  const char *open = strrchr(line, '(');
  if (!open)
    return false;
  const char *close = strchr(open, ')');
  if (!close)
    return false;
  const char *start = open + 1;
  if (strncmp(start, "path=", 5) == 0)
    start += 5;
  const char *slash = NULL;
  for (const char *p = start; p < close; ++p) {
    if (*p == '/')
      slash = p;
  }
  if (slash)
    start = slash + 1;
  size_t length = (size_t)(close - start);
  if (length == 0 || length >= SM_DIAG_FILE_NAME_SIZE)
    return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char ch = (unsigned char)start[i];
    if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-')
      return false;
  }
  memcpy(out, start, length);
  out[length] = '\0';
  return strchr(out, '.') != NULL;
}

// --- Sandbox mounts ---

bool sm_diag_sandbox_layer_label(const char *title_id, const char *mount_on,
                                 char label[16]) {
  static const char prefix[] = "/mnt/sandbox/";
  if (!title_id || !title_id[0] || !mount_on ||
      strncmp(mount_on, prefix, sizeof(prefix) - 1u) != 0)
    return false;
  const char *name = mount_on + sizeof(prefix) - 1u;
  size_t title_len = strlen(title_id);
  if (strncmp(name, title_id, title_len) != 0 || name[title_len] != '_' ||
      !isdigit((unsigned char)name[title_len + 1u]))
    return false;
  const char *rest = name + title_len + 1u;
  while (isdigit((unsigned char)*rest))
    ++rest;
  if (*rest != '/' && *rest != '\0')
    return false;
  const char *result = "other";
  size_t rest_len = strlen(rest);
  if (*rest == '\0') {
    result = "sandbox";
  } else if (strcmp(rest, "/app0") == 0) {
    result = "app0";
  } else if (strncmp(rest, "/app0/kraken", 12) == 0) {
    result = "kraken_pkg";
  } else if (rest_len >= 11u && strcmp(rest + rest_len - 11u, "/common/lib") == 0) {
    result = "fakelib";
  } else if (strcmp(rest, "/data") == 0) {
    result = "data";
  } else if (strncmp(rest, "/mnt/usb", 8) == 0) {
    result = "usb";
  } else if (strncmp(rest, "/mnt/ext", 8) == 0) {
    result = "ext";
  } else if (strncmp(rest, "/mnt/disc", 9) == 0) {
    result = "disc";
  } else if (strncmp(rest, "/preinst/common/font", 20) == 0) {
    result = "fonts";
  }
  copy_text(label, 16, result);
  return true;
}

// --- PLDMGR ---

const char *sm_diag_kstuff_implementation(const char *text) {
  if (contains_ci(text, "kstuff-lite") || contains_ci(text, "kstuff_lite"))
    return "kstuff-lite";
  if (contains_ci(text, "kstuff"))
    return "kstuff";
  return "unknown";
}

bool sm_diag_pldmgr_slot_key_valid(const char *key) {
  if (!key)
    return false;
  size_t prefix = 0;
  if (strncmp(key, "repo/", 5) == 0)
    prefix = 5;
  else if (strncmp(key, "local/", 6) == 0)
    prefix = 6;
  else
    return false;
  if (!key[prefix] || strstr(key, "..") || strlen(key) >= 160u)
    return false;
  for (const char *p = key + prefix; *p; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (!isalnum(ch) && ch != '.' && ch != '_' && ch != '-' && ch != '@')
      return false;
  }
  return true;
}

bool sm_diag_pldmgr_find_kstuff(const char *text, size_t size,
                                sm_diag_pldmgr_kstuff_t *out) {
  memset(out, 0, sizeof(*out));
  copy_text(out->implementation, sizeof(out->implementation), "unknown");
  size_t pos = 0;
  while (text && pos < size) {
    size_t end = pos;
    while (end < size && text[end] != '\n')
      ++end;
    // Source: PLDMGR src/autoload.c read_sequence(): one entry per line,
    // trimmed; "!<ms>" lines are delays; entries are slot keys
    // ("repo/..."/"local/..."), absolute paths or bare file names.
    const char *line = text + pos;
    size_t length = end - pos;
    pos = end + 1u;
    while (length > 0 && isspace((unsigned char)*line)) {
      ++line;
      --length;
    }
    while (length > 0 && isspace((unsigned char)line[length - 1u]))
      --length;
    if (length == 0 || line[0] == '!' || length >= sizeof(out->entry))
      continue;
    char entry[sizeof(out->entry)];
    memcpy(entry, line, length);
    entry[length] = '\0';
    if (!contains_ci(entry, "kstuff"))
      continue;
    copy_text(out->entry, sizeof(out->entry), entry);
    if (sm_diag_pldmgr_slot_key_valid(entry))
      copy_text(out->slot_key, sizeof(out->slot_key), entry);
    const char *base = strrchr(entry, '/');
    copy_text(out->filename, sizeof(out->filename),
              base && entry[0] == '/' ? base + 1 : "");
    copy_text(out->implementation, sizeof(out->implementation),
              sm_diag_kstuff_implementation(entry));
    return true;
  }
  return false;
}

bool sm_diag_pldmgr_parse_meta(const char *data, size_t size,
                               sm_diag_pldmgr_kstuff_t *out) {
  struct json_object *object = parse_json_text(data, size);
  if (!object)
    return false;
  // Source: PLDMGR src/slot.c slot_read_meta_unlocked(): "version", "name",
  // "id" and "filename" keys of <slot>/meta.json.
  char id[128] = "";
  json_copy_string(object, "version", out->version, sizeof(out->version));
  json_copy_string(object, "name", out->name, sizeof(out->name));
  json_copy_string(object, "filename", out->filename, sizeof(out->filename));
  json_copy_string(object, "id", id, sizeof(id));
  json_object_put(object);
  const char *implementation = "unknown";
  const char *candidates[] = {id, out->name, out->filename, out->entry};
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    implementation = sm_diag_kstuff_implementation(candidates[i]);
    if (strcmp(implementation, "unknown") != 0)
      break;
  }
  copy_text(out->implementation, sizeof(out->implementation), implementation);
  return true;
}

// --- AMPR runtime ---

int sm_diag_fs_availability(const char *name, int registered, size_t mounted,
                            bool seen_ok, char *reason, size_t reason_size) {
  if (registered > 0) {
    snprintf(reason, reason_size, "%s registered in the kernel; %zu mount(s) present",
             name, mounted);
    return 1;
  }
  if (mounted > 0) {
    snprintf(reason, reason_size, "%zu %s mount(s) present", mounted, name);
    return 1;
  }
  if (seen_ok) {
    snprintf(reason, reason_size, "mounted successfully this session");
    return 1;
  }
  if (registered == 0) {
    snprintf(reason, reason_size, "%s is not registered in the kernel", name);
    return 0;
  }
  snprintf(reason, reason_size, "not yet observed on this boot");
  return -1;
}

bool sm_diag_ampr_version(const unsigned char *data, size_t size,
                          char out[32]) {
  // Source: vendor/kraken/vendor/ampr-emu-ng/libSceAmpr.sprx embeds the
  // NUL-terminated string "ampr-emu-ng 0.4.1", matching its VERSION file.
  static const char marker[] = "ampr-emu-ng ";
  const size_t marker_len = sizeof(marker) - 1u;
  out[0] = '\0';
  for (size_t i = 0; data && i + marker_len < size; ++i) {
    if (data[i] != 'a' || memcmp(data + i, marker, marker_len) != 0)
      continue;
    size_t start = i + marker_len;
    size_t length = 0;
    while (start + length < size && length < 31u &&
           (isdigit(data[start + length]) || data[start + length] == '.'))
      ++length;
    if (length == 0 || !isdigit(data[start]) ||
        (start + length < size && data[start + length] != '\0'))
      continue;
    memcpy(out, data + start, length);
    out[length] = '\0';
    return true;
  }
  return false;
}

// --- JSON ---

void sm_diag_format_time(int64_t at_ms, char out[32]) {
  out[0] = '\0';
  if (at_ms <= 0)
    return;
  time_t seconds = (time_t)(at_ms / 1000);
  struct tm tm_utc;
  if (!gmtime_r(&seconds, &tm_utc))
    return;
  char text[96];
  snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
           tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
           tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, (int)(at_ms % 1000));
  copy_text(out, 32, text);
}

static struct json_object *time_json(int64_t at_ms) {
  if (at_ms <= 0)
    return NULL;
  char text[32];
  sm_diag_format_time(at_ms, text);
  return json_object_new_string(text);
}

static struct json_object *tristate_json(int value) {
  return value < 0 ? NULL : json_object_new_boolean(value != 0);
}

static struct json_object *finding_kind_json(int kind) {
  const char *name = kind == SM_DIAG_FINDING_RTLD    ? "rtld"
                     : kind == SM_DIAG_FINDING_FATAL ? "fatal_signal"
                     : kind == SM_DIAG_FINDING_CRASH_FLAG ? "crash_flag"
                                                          : "unknown";
  return json_object_new_string(name);
}

static const char *kline_kind_name(int kind) {
  return kind == SM_DIAG_KLINE_SPAWN_ERROR ? "spawn_error"
         : kind == SM_DIAG_KLINE_RTLD      ? "rtld"
         : kind == SM_DIAG_KLINE_AUTH      ? "self_auth"
         : kind == SM_DIAG_KLINE_EXCEPTION ? "exception"
                                           : "unknown";
}

static int kline_kind_from_name(const char *name) {
  for (int kind = SM_DIAG_KLINE_SPAWN_ERROR; kind <= SM_DIAG_KLINE_EXCEPTION; ++kind) {
    if (name && strcmp(name, kline_kind_name(kind)) == 0)
      return kind;
  }
  return 0;
}

static const char *first_short_error(const sm_diag_record_t *rec) {
  for (size_t i = 0; i < rec->native_error_count; ++i) {
    if (rec->native_errors[i].short_error[0] &&
        strcmp(rec->native_errors[i].attribution, "title_id") == 0)
      return rec->native_errors[i].short_error;
  }
  for (size_t i = 0; i < rec->native_error_count; ++i) {
    if (rec->native_errors[i].short_error[0])
      return rec->native_errors[i].short_error;
  }
  return NULL;
}

struct json_object *sm_diag_record_summary_json(const sm_diag_record_t *rec) {
  struct json_object *object = json_object_new_object();
  if (!object)
    return NULL;
  const char *short_error = first_short_error(rec);
  json_object_object_add(object, "id", json_object_new_int64((int64_t)rec->id));
  json_object_object_add(object, "outcome",
                         json_object_new_string(sm_diag_outcome_name(rec->outcome)));
  json_object_object_add(object, "at", time_json(rec->created_ms));
  json_object_object_add(object, "short_error",
                         short_error ? json_object_new_string(short_error) : NULL);
  json_object_object_add(object, "category",
                         rec->has_diagnosis
                             ? json_object_new_string(rec->diagnosis.category)
                             : NULL);
  json_object_object_add(object, "observation_interrupted",
                         json_object_new_boolean(rec->interrupted));
  return object;
}

struct json_object *sm_diag_record_to_json(const sm_diag_record_t *rec) {
  struct json_object *object = json_object_new_object();
  if (!object)
    return NULL;
  const char *short_error = first_short_error(rec);
  json_object_object_add(object, "id", json_object_new_int64((int64_t)rec->id));
  json_object_object_add(object, "title_id", json_object_new_string(rec->title_id));
  json_object_object_add(object, "managed", tristate_json(rec->managed));
  json_object_object_add(object, "at", time_json(rec->created_ms));
  json_object_object_add(object, "at_unix_ms", json_object_new_int64(rec->created_ms));
  json_object_object_add(object, "mono_us",
                         json_object_new_int64((int64_t)rec->created_mono_us));
  json_object_object_add(object, "outcome",
                         json_object_new_string(sm_diag_outcome_name(rec->outcome)));
  json_object_object_add(object, "observation_interrupted",
                         json_object_new_boolean(rec->interrupted));
  json_object_object_add(object, "short_error",
                         short_error ? json_object_new_string(short_error) : NULL);
  json_object_object_add(object, "launch_attempts",
                         json_object_new_int64(rec->launch_attempts));

  struct json_object *stages = json_object_new_object();
  for (size_t i = 0; stages && i < SM_DIAG_STAGE_COUNT; ++i) {
    const sm_diag_stage_t *stage = &rec->stages[i];
    struct json_object *entry = json_object_new_object();
    if (!entry)
      continue;
    json_object_object_add(entry, "state",
                           json_object_new_string(sm_diag_state_name(stage->state)));
    json_object_object_add(entry, "at", time_json(stage->at_ms));
    json_object_object_add(entry, "at_unix_ms",
                           stage->at_ms > 0 ? json_object_new_int64(stage->at_ms) : NULL);
    json_object_object_add(entry, "mono_us",
                           stage->mono_us ? json_object_new_int64((int64_t)stage->mono_us)
                                          : NULL);
    json_object_object_add(entry, "detail",
                           stage->detail[0] ? json_object_new_string(stage->detail) : NULL);
    json_object_object_add(stages, sm_diag_stage_name((sm_diag_stage_id_t)i), entry);
  }
  json_object_object_add(object, "stages", stages);

  if (rec->mount_reported) {
    struct json_object *mount = json_object_new_object();
    if (mount) {
      int error = rec->mount_errno ? rec->mount_errno : rec->mount_status;
      json_object_object_add(mount, "status", json_object_new_int64(rec->mount_status));
      json_object_object_add(mount, "errno", json_object_new_int64(rec->mount_errno));
      json_object_object_add(mount, "error",
                             error ? json_object_new_string(strerror(error)) : NULL);
      json_object_object_add(mount, "layer",
                             rec->mount_layer[0] ? json_object_new_string(rec->mount_layer)
                                                 : NULL);
    }
    json_object_object_add(object, "mount", mount);
  } else {
    json_object_object_add(object, "mount", NULL);
  }

  if (rec->pid > 0) {
    struct json_object *process = json_object_new_object();
    if (process) {
      char app_id[11];
      snprintf(app_id, sizeof(app_id), "0x%08X", rec->app_id);
      json_object_object_add(process, "pid", json_object_new_int64(rec->pid));
      json_object_object_add(process, "app_id", json_object_new_string(app_id));
      json_object_object_add(process, "started_at",
                             time_json(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].at_ms));
      bool exited = rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK;
      json_object_object_add(process, "exited_at", exited ? time_json(rec->exit_ms) : NULL);
      json_object_object_add(
          process, "alive_seconds",
          exited && rec->exit_mono_us >= rec->process_start_mono_us
              ? json_object_new_double(
                    (double)(rec->exit_mono_us - rec->process_start_mono_us) / 1e6)
              : NULL);
    }
    json_object_object_add(object, "process", process);
  } else {
    json_object_object_add(object, "process", NULL);
  }

  struct json_object *environment = json_object_new_object();
  if (environment) {
    char fw[8] = "";
    json_object_object_add(environment, "kstuff_loaded", tristate_json(rec->kstuff_loaded));
    json_object_object_add(environment, "shellcore_hooks_installed",
                           tristate_json(rec->hooks_installed));
    if (rec->console_fw_raw) {
      sm_diag_format_fw_bcd((rec->console_fw_raw >> 16) & 0xffffu, fw);
      json_object_object_add(environment, "console_firmware", json_object_new_string(fw));
      json_object_object_add(environment, "console_firmware_raw",
                             json_object_new_int64(rec->console_fw_raw));
    } else {
      json_object_object_add(environment, "console_firmware", NULL);
      json_object_object_add(environment, "console_firmware_raw", NULL);
    }
    if (rec->has_required_fw) {
      sm_diag_format_fw_bcd(rec->required_fw_bcd, fw);
      json_object_object_add(environment, "required_firmware", json_object_new_string(fw));
      json_object_object_add(environment, "required_firmware_raw",
                             json_object_new_string(rec->required_fw_raw));
    } else {
      json_object_object_add(environment, "required_firmware", NULL);
      json_object_object_add(environment, "required_firmware_raw", NULL);
    }
  }
  json_object_object_add(object, "environment", environment);

  struct json_object *overlay = json_object_new_object();
  if (overlay) {
    struct json_object *layers = json_object_new_array();
    for (size_t i = 0; layers && i < rec->layer_count; ++i) {
      struct json_object *layer = json_object_new_object();
      if (!layer)
        continue;
      json_object_object_add(layer, "label", json_object_new_string(rec->layers[i].label));
      json_object_object_add(layer, "fstype", json_object_new_string(rec->layers[i].fstype));
      json_object_object_add(layer, "from", json_object_new_string(rec->layers[i].from));
      json_object_object_add(layer, "on", json_object_new_string(rec->layers[i].on));
      json_object_array_add(layers, layer);
    }
    json_object_object_add(overlay, "captured", json_object_new_boolean(rec->layers_captured));
    json_object_object_add(overlay, "layers", layers);
    struct json_object *fakelib = NULL;
    if (rec->fakelib_source[0]) {
      fakelib = json_object_new_object();
      struct json_object *files = json_object_new_array();
      for (size_t i = 0; files && i < rec->fakelib_file_count; ++i)
        json_object_array_add(files, json_object_new_string(rec->fakelib_files[i]));
      if (fakelib) {
        json_object_object_add(fakelib, "source", json_object_new_string(rec->fakelib_source));
        json_object_object_add(fakelib, "files", files);
      } else {
        json_object_put(files);
      }
    }
    json_object_object_add(overlay, "fakelib", fakelib);
  }
  json_object_object_add(object, "overlay", overlay);

  struct json_object *findings = json_object_new_array();
  for (size_t i = 0; findings && i < rec->finding_count; ++i) {
    struct json_object *finding = json_object_new_object();
    if (!finding)
      continue;
    json_object_object_add(finding, "source", json_object_new_string("mdbg"));
    json_object_object_add(finding, "kind", finding_kind_json(rec->findings[i].kind));
    json_object_object_add(finding, "at", time_json(rec->findings[i].at_ms));
    json_object_object_add(finding, "at_unix_ms", json_object_new_int64(rec->findings[i].at_ms));
    json_object_object_add(finding, "text", json_object_new_string(rec->findings[i].text));
    json_object_array_add(findings, finding);
  }
  json_object_object_add(object, "game_errors", findings);

  struct json_object *klines = json_object_new_array();
  for (size_t i = 0; klines && i < rec->kernel_line_count; ++i) {
    struct json_object *line = json_object_new_object();
    if (!line)
      continue;
    json_object_object_add(line, "kind",
                           json_object_new_string(kline_kind_name(rec->kernel_lines[i].kind)));
    json_object_object_add(line, "attributed",
                           json_object_new_boolean(rec->kernel_lines[i].attributed));
    json_object_object_add(line, "text", json_object_new_string(rec->kernel_lines[i].text));
    json_object_array_add(klines, line);
  }
  json_object_object_add(object, "kernel_log", klines);

  struct json_object *errors = json_object_new_array();
  for (size_t i = 0; errors && i < rec->native_error_count; ++i) {
    struct json_object *error = sm_diag_native_error_to_json(&rec->native_errors[i]);
    if (error)
      json_object_array_add(errors, error);
  }
  json_object_object_add(object, "native_errors", errors);

  if (rec->has_diagnosis) {
    struct json_object *diagnosis = json_object_new_object();
    if (diagnosis) {
      struct json_object *evidence = json_object_new_array();
      for (size_t i = 0; evidence && i < rec->diagnosis.evidence_count; ++i)
        json_object_array_add(evidence, json_object_new_string(rec->diagnosis.evidence[i]));
      json_object_object_add(diagnosis, "category",
                             json_object_new_string(rec->diagnosis.category));
      json_object_object_add(diagnosis, "confidence",
                             json_object_new_string(rec->diagnosis.confidence));
      json_object_object_add(diagnosis, "rule", json_object_new_string(rec->diagnosis.rule));
      json_object_object_add(diagnosis, "evidence", evidence);
      json_object_object_add(diagnosis, "advice", json_object_new_string(rec->diagnosis.advice));
    }
    json_object_object_add(object, "diagnosis", diagnosis);
  } else {
    json_object_object_add(object, "diagnosis", NULL);
  }

  struct json_object *timeline = json_object_new_array();
  for (size_t i = 0; timeline && i < rec->timeline_count; ++i) {
    const sm_diag_event_t *event = &rec->timeline[i];
    struct json_object *entry = json_object_new_object();
    if (!entry)
      continue;
    json_object_object_add(entry, "at", time_json(event->at_ms));
    json_object_object_add(entry, "at_unix_ms", json_object_new_int64(event->at_ms));
    json_object_object_add(entry, "mono_us", json_object_new_int64((int64_t)event->mono_us));
    json_object_object_add(entry, "event", json_object_new_string(event->event));
    json_object_object_add(entry, "detail", json_object_new_string(event->detail));
    json_object_array_add(timeline, entry);
  }
  json_object_object_add(object, "timeline", timeline);
  return object;
}

static struct json_object *get_member(struct json_object *object,
                                      const char *key) {
  struct json_object *value = NULL;
  if (!object || !json_object_object_get_ex(object, key, &value))
    return NULL;
  return value;
}

static int64_t get_int(struct json_object *object, const char *key,
                       int64_t fallback) {
  struct json_object *value = get_member(object, key);
  return value && (json_object_is_type(value, json_type_int) ||
                   json_object_is_type(value, json_type_double))
             ? json_object_get_int64(value)
             : fallback;
}

static int get_tristate(struct json_object *object, const char *key) {
  struct json_object *value = get_member(object, key);
  if (!value || !json_object_is_type(value, json_type_boolean))
    return -1;
  return json_object_get_boolean(value) ? 1 : 0;
}

static void get_string(struct json_object *object, const char *key, char *out,
                       size_t size) {
  json_copy_string(object, key, out, size);
}

bool sm_diag_record_from_json(struct json_object *object,
                              sm_diag_record_t *rec) {
  if (!object || !json_object_is_type(object, json_type_object))
    return false;
  char title[SM_DIAG_TITLE_ID_SIZE] = "";
  get_string(object, "title_id", title, sizeof(title));
  int64_t id = get_int(object, "id", 0);
  if (!title[0] || id <= 0)
    return false;
  sm_diag_record_init(rec, (uint64_t)id, title, get_int(object, "at_unix_ms", 0),
                      (uint64_t)get_int(object, "mono_us", 0));
  rec->managed = get_tristate(object, "managed");
  rec->interrupted = false;
  struct json_object *interrupted = get_member(object, "observation_interrupted");
  if (interrupted && json_object_is_type(interrupted, json_type_boolean))
    rec->interrupted = json_object_get_boolean(interrupted);
  rec->launch_attempts = (unsigned)get_int(object, "launch_attempts", 0);
  char outcome[32] = "";
  get_string(object, "outcome", outcome, sizeof(outcome));
  if (!sm_diag_outcome_from_name(outcome, &rec->outcome))
    rec->outcome = SM_DIAG_OUTCOME_LAUNCH_NOT_OBSERVED;

  struct json_object *stages = get_member(object, "stages");
  for (size_t i = 0; stages && i < SM_DIAG_STAGE_COUNT; ++i) {
    struct json_object *entry = get_member(stages, sm_diag_stage_name((sm_diag_stage_id_t)i));
    if (!entry)
      continue;
    char state[24] = "";
    get_string(entry, "state", state, sizeof(state));
    sm_diag_state_t parsed = SM_DIAG_NOT_OBSERVED;
    (void)state_from_name(state, &parsed);
    rec->stages[i].state = parsed;
    rec->stages[i].at_ms = get_int(entry, "at_unix_ms", 0);
    rec->stages[i].mono_us = (uint64_t)get_int(entry, "mono_us", 0);
    get_string(entry, "detail", rec->stages[i].detail, sizeof(rec->stages[i].detail));
  }

  struct json_object *mount = get_member(object, "mount");
  if (mount && json_object_is_type(mount, json_type_object)) {
    rec->mount_reported = true;
    rec->mount_status = (int)get_int(mount, "status", 0);
    rec->mount_errno = (int)get_int(mount, "errno", 0);
    get_string(mount, "layer", rec->mount_layer, sizeof(rec->mount_layer));
  }
  struct json_object *process = get_member(object, "process");
  if (process && json_object_is_type(process, json_type_object)) {
    rec->pid = (pid_t)get_int(process, "pid", 0);
    char app_id[16] = "";
    get_string(process, "app_id", app_id, sizeof(app_id));
    rec->app_id = (uint32_t)strtoul(app_id, NULL, 16);
    struct json_object *alive = get_member(process, "alive_seconds");
    rec->process_start_mono_us = rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].mono_us;
    if (rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK) {
      rec->exit_ms = rec->stages[SM_DIAG_STAGE_EXITED].at_ms;
      rec->exit_mono_us = rec->stages[SM_DIAG_STAGE_EXITED].mono_us;
      if (alive && json_object_is_type(alive, json_type_double) &&
          rec->exit_mono_us == 0)
        rec->exit_mono_us = rec->process_start_mono_us +
                            (uint64_t)(json_object_get_double(alive) * 1e6);
    }
  }
  struct json_object *environment = get_member(object, "environment");
  if (environment) {
    rec->kstuff_loaded = get_tristate(environment, "kstuff_loaded");
    rec->hooks_installed = get_tristate(environment, "shellcore_hooks_installed");
    rec->console_fw_raw = (uint32_t)get_int(environment, "console_firmware_raw", 0);
    char required[24] = "";
    get_string(environment, "required_firmware_raw", required, sizeof(required));
    if (required[0]) {
      char *end = NULL;
      uint64_t number = strtoull(required, &end, 16);
      rec->has_required_fw = end && *end == '\0';
      rec->required_fw_bcd = (uint32_t)(number >> 48) & 0xffffu;
      copy_text(rec->required_fw_raw, sizeof(rec->required_fw_raw), required);
    }
  }
  struct json_object *overlay = get_member(object, "overlay");
  if (overlay) {
    struct json_object *captured = get_member(overlay, "captured");
    rec->layers_captured = captured && json_object_get_boolean(captured);
    struct json_object *layers = get_member(overlay, "layers");
    size_t count = layers && json_object_is_type(layers, json_type_array)
                       ? json_object_array_length(layers) : 0;
    for (size_t i = 0; i < count && rec->layer_count < SM_DIAG_MAX_LAYERS; ++i) {
      struct json_object *layer = json_object_array_get_idx(layers, i);
      sm_diag_layer_t *out = &rec->layers[rec->layer_count++];
      get_string(layer, "label", out->label, sizeof(out->label));
      get_string(layer, "fstype", out->fstype, sizeof(out->fstype));
      get_string(layer, "from", out->from, sizeof(out->from));
      get_string(layer, "on", out->on, sizeof(out->on));
    }
    struct json_object *fakelib = get_member(overlay, "fakelib");
    if (fakelib && json_object_is_type(fakelib, json_type_object)) {
      get_string(fakelib, "source", rec->fakelib_source, sizeof(rec->fakelib_source));
      struct json_object *files = get_member(fakelib, "files");
      size_t file_count = files && json_object_is_type(files, json_type_array)
                              ? json_object_array_length(files) : 0;
      for (size_t i = 0; i < file_count && rec->fakelib_file_count < SM_DIAG_MAX_FAKELIB_FILES; ++i)
        copy_text(rec->fakelib_files[rec->fakelib_file_count++], SM_DIAG_FILE_NAME_SIZE,
                  json_object_get_string(json_object_array_get_idx(files, i)));
    }
  }
  struct json_object *findings = get_member(object, "game_errors");
  size_t finding_count = findings && json_object_is_type(findings, json_type_array)
                             ? json_object_array_length(findings) : 0;
  for (size_t i = 0; i < finding_count && rec->finding_count < SM_DIAG_MAX_FINDINGS; ++i) {
    struct json_object *finding = json_object_array_get_idx(findings, i);
    sm_diag_finding_t *out = &rec->findings[rec->finding_count++];
    char kind[24] = "";
    get_string(finding, "kind", kind, sizeof(kind));
    out->kind = strcmp(kind, "rtld") == 0           ? SM_DIAG_FINDING_RTLD
                : strcmp(kind, "fatal_signal") == 0 ? SM_DIAG_FINDING_FATAL
                : strcmp(kind, "crash_flag") == 0   ? SM_DIAG_FINDING_CRASH_FLAG
                                                    : 0;
    out->at_ms = get_int(finding, "at_unix_ms", 0);
    get_string(finding, "text", out->text, sizeof(out->text));
  }
  struct json_object *klines = get_member(object, "kernel_log");
  size_t kline_count = klines && json_object_is_type(klines, json_type_array)
                           ? json_object_array_length(klines) : 0;
  for (size_t i = 0; i < kline_count && rec->kernel_line_count < SM_DIAG_MAX_KERNEL_LINES; ++i) {
    struct json_object *line = json_object_array_get_idx(klines, i);
    sm_diag_kline_t *out = &rec->kernel_lines[rec->kernel_line_count++];
    char kind[24] = "";
    get_string(line, "kind", kind, sizeof(kind));
    out->kind = kline_kind_from_name(kind);
    out->attributed = get_tristate(line, "attributed") == 1;
    get_string(line, "text", out->text, sizeof(out->text));
  }
  struct json_object *errors = get_member(object, "native_errors");
  size_t error_count = errors && json_object_is_type(errors, json_type_array)
                           ? json_object_array_length(errors) : 0;
  for (size_t i = 0; i < error_count && rec->native_error_count < SM_DIAG_MAX_NATIVE_ERRORS; ++i) {
    struct json_object *error = json_object_array_get_idx(errors, i);
    sm_diag_native_error_t *out = &rec->native_errors[rec->native_error_count++];
    get_string(error, "file", out->file, sizeof(out->file));
    out->mtime = get_int(error, "file_mtime_unix", 0);
    out->parsed = get_tristate(error, "parsed") == 1;
    get_string(error, "short_error", out->short_error, sizeof(out->short_error));
    struct json_object *code = get_member(error, "internal_error_code");
    if (code && json_object_is_type(code, json_type_int)) {
      out->has_internal_code = true;
      out->internal_code = (int32_t)json_object_get_int64(code);
    }
    get_string(error, "message", out->message, sizeof(out->message));
    get_string(error, "title_id", out->title_id, sizeof(out->title_id));
    get_string(error, "display_version", out->display_version, sizeof(out->display_version));
    struct json_object *tick = get_member(error, "tick");
    if (tick && json_object_is_type(tick, json_type_int)) {
      out->has_tick = true;
      out->tick = json_object_get_uint64(tick);
    }
    struct json_object *app = get_member(error, "app_id");
    if (app && json_object_is_type(app, json_type_int)) {
      out->has_app_id = true;
      out->app_id = json_object_get_int64(app);
    }
    get_string(error, "attribution", out->attribution, sizeof(out->attribution));
  }
  struct json_object *timeline = get_member(object, "timeline");
  size_t event_count = timeline && json_object_is_type(timeline, json_type_array)
                           ? json_object_array_length(timeline) : 0;
  for (size_t i = 0; i < event_count && rec->timeline_count < SM_DIAG_MAX_TIMELINE; ++i) {
    struct json_object *event = json_object_array_get_idx(timeline, i);
    sm_diag_event_t *out = &rec->timeline[rec->timeline_count++];
    out->at_ms = get_int(event, "at_unix_ms", 0);
    out->mono_us = (uint64_t)get_int(event, "mono_us", 0);
    get_string(event, "event", out->event, sizeof(out->event));
    get_string(event, "detail", out->detail, sizeof(out->detail));
  }
  struct json_object *diagnosis = get_member(object, "diagnosis");
  if (diagnosis && json_object_is_type(diagnosis, json_type_object)) {
    rec->has_diagnosis = true;
    get_string(diagnosis, "category", rec->diagnosis.category, sizeof(rec->diagnosis.category));
    get_string(diagnosis, "confidence", rec->diagnosis.confidence,
               sizeof(rec->diagnosis.confidence));
    get_string(diagnosis, "rule", rec->diagnosis.rule, sizeof(rec->diagnosis.rule));
    get_string(diagnosis, "advice", rec->diagnosis.advice, sizeof(rec->diagnosis.advice));
    struct json_object *evidence = get_member(diagnosis, "evidence");
    size_t evidence_count = evidence && json_object_is_type(evidence, json_type_array)
                                ? json_object_array_length(evidence) : 0;
    for (size_t i = 0; i < evidence_count && rec->diagnosis.evidence_count < SM_DIAG_MAX_EVIDENCE; ++i)
      copy_text(rec->diagnosis.evidence[rec->diagnosis.evidence_count++], SM_DIAG_TEXT_SIZE,
                json_object_get_string(json_object_array_get_idx(evidence, i)));
  }
  // Unfinished launches are resolved by sm_diag_record_resolve_restored().
  rec->restored = true;
  return true;
}
