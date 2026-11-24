#include "host_platform.h"
#include "sm_diag_core.h"

#include "../src/sm_diag_core.c"

#define TITLE "PPSA03355"
#define SECOND 1000000ull

static const sm_diag_policy_t *policy(void) {
  static sm_diag_policy_t value;
  value = sm_diag_default_policy();
  return &value;
}

// All tests share one clock origin; "t" is seconds since the launch.
static const uint64_t k_mono0 = 500ull * SECOND;
static const int64_t k_wall0 = 1795201919000ll;

static sm_diag_input_t event(sm_diag_event_type_t type, double t) {
  sm_diag_input_t input;
  memset(&input, 0, sizeof(input));
  input.type = type;
  strlcpy(input.title_id, TITLE, sizeof(input.title_id));
  input.mono_us = k_mono0 + (uint64_t)(t * 1e6);
  input.wall_ms = k_wall0 + (int64_t)(t * 1000.0);
  return input;
}

static sm_diag_record_t *apply(sm_diag_store_t *store, sm_diag_input_t input) {
  return sm_diag_store_apply(store, policy(), &input);
}

static void evaluate(sm_diag_record_t *rec, double t) {
  (void)sm_diag_record_evaluate(rec, policy(), k_wall0 + (int64_t)(t * 1000.0),
                                k_mono0 + (uint64_t)(t * 1e6));
}

static sm_diag_record_t *launch(sm_diag_store_t *store, int status, double t) {
  sm_diag_input_t input = event(SM_DIAG_EV_LAUNCH, t);
  input.status = status;
  return apply(store, input);
}

static sm_diag_record_t *start_process(sm_diag_store_t *store, pid_t pid,
                                       double t) {
  sm_diag_input_t input = event(SM_DIAG_EV_PROCESS_STARTED, t);
  input.pid = pid;
  input.app_id = 0x2019;
  input.event_mono_us = input.mono_us;
  return apply(store, input);
}

static sm_diag_record_t *pid_event(sm_diag_store_t *store,
                                   sm_diag_event_type_t type, pid_t pid,
                                   double t) {
  sm_diag_input_t input = event(type, t);
  input.title_id[0] = '\0';
  input.pid = pid;
  return apply(store, input);
}

static void set_registered(sm_diag_record_t *rec) {
  sm_diag_record_set_stage(rec, SM_DIAG_STAGE_REGISTERED, SM_DIAG_OK, k_wall0,
                           k_mono0, "title present in app.db at launch time");
}

// --- Timeline / outcome state machine ---

static void test_mounted_registered_but_no_process_is_not_running(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  // A mount or registration alone never creates a launch record.
  assert(sm_diag_store_count(&store) == 0);

  sm_diag_record_t *rec = launch(&store, 0, 0);
  set_registered(rec);
  assert(rec->stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_OK);
  assert(rec->stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].state == SM_DIAG_OK);
  for (double t = 1; t <= 29; t += 4) {
    evaluate(rec, t);
    assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
    assert(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_PENDING);
    assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state != SM_DIAG_OK);
  }
  evaluate(rec, 31);
  assert(rec->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED);
  assert(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_FAILED);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_NOT_OBSERVED);
  assert(rec->stages[SM_DIAG_STAGE_SANDBOX_READY].state == SM_DIAG_NOT_OBSERVED);
  assert(rec->has_diagnosis && !strcmp(rec->diagnosis.category, "unknown"));
  sm_diag_store_free(&store);
}

static void test_process_dies_after_six_seconds_is_exited_early(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  sm_diag_input_t sandbox = event(SM_DIAG_EV_SANDBOX_READY, 0.5);
  assert(apply(&store, sandbox) == rec);
  assert(start_process(&store, 99, 1) == rec);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  assert(rec->stages[SM_DIAG_STAGE_SANDBOX_READY].state == SM_DIAG_OK);
  assert(pid_event(&store, SM_DIAG_EV_PROCESS_EXIT, 99, 7) == rec);
  // NOTE_EXIT is not final yet (same-app handoff window): no verdict, and
  // in particular no running confirmation, even after 20 s.
  evaluate(rec, 22);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_PENDING);
  assert(pid_event(&store, SM_DIAG_EV_PROCESS_FINAL, 99, 23) == rec);
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_EARLY);
  assert(rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_FAILED);
  assert(rec->exit_mono_us - rec->process_start_mono_us == 6 * SECOND);
  assert(strstr(rec->stages[SM_DIAG_STAGE_EXITED].detail, "6.0 s"));
  assert(rec->has_diagnosis);
  sm_diag_store_free(&store);
}

static void test_launch_failed_is_spawn_failed(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  sm_diag_input_t failed = event(SM_DIAG_EV_LAUNCH_FAILED, 1);
  assert(apply(&store, failed) == rec);
  // ShellCore retries launchApp during a game switch: a retry within the
  // grace stays the same launch and is still in progress.
  evaluate(rec, 5);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  assert(launch(&store, 0, 6) == rec);
  assert(rec->launch_attempts == 2 && !rec->launch_failed);
  assert(apply(&store, event(SM_DIAG_EV_LAUNCH_FAILED, 7)) == rec);
  evaluate(rec, 16);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  evaluate(rec, 17.5);
  assert(rec->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED);
  assert(strstr(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].detail, "LAUNCH_FAILED"));
  // A later launch is a new record.
  sm_diag_record_t *next = launch(&store, 0, 40);
  assert(next != rec && next->id == rec->id + 1u);
  sm_diag_store_free(&store);

  // A spawn_failed record restored from launches.json (an earlier process)
  // never absorbs a new process, even with overlapping monotonic times.
  assert(sm_diag_store_init(&store));
  rec = launch(&store, 0, 0);
  assert(apply(&store, event(SM_DIAG_EV_LAUNCH_FAILED, 1)) == rec);
  evaluate(rec, 17.5);
  assert(rec->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED);
  rec->restored = true;
  sm_diag_record_t *process = start_process(&store, 4242, 18);
  assert(process != rec && rec->pid <= 0 && process->pid == 4242);
  assert(rec->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED);
  // Without the restored flag the same process revises the verdict.
  sm_diag_store_free(&store);
  assert(sm_diag_store_init(&store));
  rec = launch(&store, 0, 0);
  assert(apply(&store, event(SM_DIAG_EV_LAUNCH_FAILED, 1)) == rec);
  evaluate(rec, 17.5);
  assert(start_process(&store, 4242, 18) == rec);
  sm_diag_store_free(&store);
}

static void test_running_then_exited_normally(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 120, 1);
  evaluate(rec, 20.9);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  evaluate(rec, 21);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_OK);
  assert(!rec->has_diagnosis);
  pid_event(&store, SM_DIAG_EV_PROCESS_EXIT, 120, 600);
  pid_event(&store, SM_DIAG_EV_PROCESS_FINAL, 120, 602);
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY);
  assert(!rec->has_diagnosis);
  sm_diag_store_free(&store);
}

static void test_handoff_keeps_process_alive(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 130, 1);
  pid_event(&store, SM_DIAG_EV_PROCESS_EXIT, 130, 3);
  sm_diag_input_t handoff = event(SM_DIAG_EV_PROCESS_HANDOFF, 4);
  handoff.pid = 130;
  handoff.new_pid = 131;
  assert(apply(&store, handoff) == rec);
  assert(rec->pid == 131 && !rec->exit_pending);
  // The old PID's final exit no longer belongs to this launch.
  assert(pid_event(&store, SM_DIAG_EV_PROCESS_FINAL, 130, 5) == NULL);
  evaluate(rec, 22);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  sm_diag_store_free(&store);
}

static void test_fatal_signal_is_crashed(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 253, 1);
  sm_diag_input_t finding = event(SM_DIAG_EV_FINDING, 2);
  finding.pid = 253;
  finding.finding_kind = SM_DIAG_FINDING_FATAL;
  strlcpy(finding.text, "signal: 12 (SIGSYS)", sizeof(finding.text));
  assert(apply(&store, finding) == rec);
  assert(rec->outcome == SM_DIAG_OUTCOME_CRASHED);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_FAILED);
  assert(sm_diag_record_is_active(rec)); // still waiting for the exit
  pid_event(&store, SM_DIAG_EV_PROCESS_FINAL, 253, 3);
  assert(rec->outcome == SM_DIAG_OUTCOME_CRASHED && !sm_diag_record_is_active(rec));
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  sm_diag_store_free(&store);
}

static void test_mount_failure_and_unmanaged_titles(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_input_t input = event(SM_DIAG_EV_LAUNCH, 0);
  input.status = EBUSY;
  input.native_errno = EBUSY;
  strlcpy(input.layer, "claim", sizeof(input.layer));
  sm_diag_record_t *rec = apply(&store, input);
  assert(rec->outcome == SM_DIAG_OUTCOME_MOUNT_FAILED);
  assert(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_NOT_OBSERVED);
  assert(!strcmp(rec->diagnosis.category, "mount_failed"));
  assert(!strcmp(rec->diagnosis.confidence, "observed"));
  assert(strstr(rec->diagnosis.evidence[0], "layer=claim"));
  // ShellCore's LAUNCH_FAILED for the refused launch joins the same record.
  assert(apply(&store, event(SM_DIAG_EV_LAUNCH_FAILED, 0.2)) == rec);
  assert(sm_diag_store_count(&store) == 1);

  sm_diag_input_t stock_input = event(SM_DIAG_EV_LAUNCH, 100);
  stock_input.status = ENOENT;
  strlcpy(stock_input.layer, "mount_link", sizeof(stock_input.layer));
  sm_diag_record_t *stock = apply(&store, stock_input);
  assert(stock != rec && stock->managed == 0);
  assert(stock->stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_NOT_OBSERVED);
  assert(stock->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  // An unsupported title id (no layer, EINVAL) is not managed either.
  sm_diag_record_t *unsupported = launch(&store, EINVAL, 200);
  assert(unsupported->managed == 0);
  assert(unsupported->stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_NOT_OBSERVED);
  // A managed Kraken title whose .pkg is offline fails in "kraken_source"
  // with ENOENT: a managed mount failure, not "not managed".
  sm_diag_input_t offline = event(SM_DIAG_EV_LAUNCH, 300);
  strlcpy(offline.title_id, "PPSA05678", sizeof(offline.title_id));
  offline.status = ENOENT;
  offline.native_errno = ENOENT;
  strlcpy(offline.layer, "kraken_source", sizeof(offline.layer));
  sm_diag_record_t *kraken = apply(&store, offline);
  assert(kraken->managed == 1);
  assert(kraken->stages[SM_DIAG_STAGE_MOUNTED].state == SM_DIAG_FAILED);
  assert(kraken->outcome == SM_DIAG_OUTCOME_MOUNT_FAILED);
  assert(!strcmp(kraken->diagnosis.category, "mount_failed"));
  assert(strstr(kraken->diagnosis.evidence[0], "layer=kraken_source"));
  sm_diag_store_free(&store);
}

static void test_process_without_launch_request(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = start_process(&store, 77, 0);
  assert(rec->stages[SM_DIAG_STAGE_LAUNCH_REQUESTED].state == SM_DIAG_NOT_OBSERVED);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  evaluate(rec, 20);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  sm_diag_store_free(&store);
}

static void test_store_keeps_last_64(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  for (int i = 0; i < 70; ++i)
    (void)launch(&store, EIO, (double)i * 100.0);
  assert(sm_diag_store_count(&store) == SM_DIAG_MAX_RECORDS);
  assert(sm_diag_store_at(&store, 0)->id == 7);
  assert(sm_diag_store_at(&store, SM_DIAG_MAX_RECORDS - 1u)->id == 70);
  assert(sm_diag_store_find_id(&store, 6) == NULL);
  assert(sm_diag_store_latest_for_title(&store, TITLE)->id == 70);
  sm_diag_store_free(&store);
}

static void test_json_round_trip(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *done = launch(&store, 0, 0);
  start_process(&store, 99, 1);
  pid_event(&store, SM_DIAG_EV_PROCESS_FINAL, 99, 7);
  assert(done->outcome == SM_DIAG_OUTCOME_EXITED_EARLY);
  sm_diag_record_t *live = launch(&store, 0, 100);
  start_process(&store, 100, 101);
  evaluate(live, 125);
  assert(live->outcome == SM_DIAG_OUTCOME_RUNNING);

  struct json_object *json = sm_diag_record_to_json(done);
  sm_diag_record_t restored;
  assert(sm_diag_record_from_json(json, &restored));
  json_object_put(json);
  assert(restored.id == done->id && restored.outcome == SM_DIAG_OUTCOME_EXITED_EARLY);
  assert(!restored.interrupted && restored.restored);
  assert(restored.stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK);
  assert(restored.has_diagnosis &&
         !strcmp(restored.diagnosis.category, done->diagnosis.category));
  assert(restored.pid == 99 && restored.app_id == 0x2019);

  json = sm_diag_record_to_json(live);
  assert(sm_diag_record_from_json(json, &restored));
  json_object_put(json);
  // Restored as stored; the daemon resolves it with a PID probe.
  assert(restored.restored && !restored.interrupted &&
         restored.outcome == SM_DIAG_OUTCOME_RUNNING);
  assert(sm_diag_record_unfinished(&restored));
  assert(!sm_diag_record_evaluate(&restored, policy(), k_wall0, k_mono0));

  struct json_object *summary = sm_diag_record_summary_json(done);
  struct json_object *value = NULL;
  assert(json_object_object_get_ex(summary, "outcome", &value) &&
         !strcmp(json_object_get_string(value), "exited_early"));
  assert(json_object_object_get_ex(summary, "short_error", &value) && value == NULL);
  json_object_put(summary);
  sm_diag_store_free(&store);
}

// --- Restart and missed-exit reconciliation ---

static const sm_diag_policy_t *probe_policy(void) {
  static sm_diag_policy_t value;
  value = sm_diag_default_policy();
  value.require_alive_probe = true;
  return &value;
}

// The hardware case: launched, process started, UniMount replaced a few
// seconds later. Persist and restore it as the next run would.
static void restore_running_launch(sm_diag_record_t *out, bool with_error) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  set_registered(rec);
  apply(&store, event(SM_DIAG_EV_SANDBOX_READY, 0.5));
  start_process(&store, 143, 1);
  evaluate(rec, 4);
  if (with_error) {
    sm_diag_native_error_t *error = &rec->native_errors[rec->native_error_count++];
    memset(error, 0, sizeof(*error));
    strlcpy(error->file, "0000001D.json", sizeof(error->file));
    error->parsed = error->has_internal_code = true;
    error->internal_code = -2137784263;
    strlcpy(error->short_error, "CE-105779-9", sizeof(error->short_error));
    strlcpy(error->title_id, TITLE, sizeof(error->title_id));
    strlcpy(error->attribution, "title_id", sizeof(error->attribution));
  }
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_PENDING);
  struct json_object *json = sm_diag_record_to_json(rec);
  assert(sm_diag_record_from_json(json, out));
  json_object_put(json);
  sm_diag_store_free(&store);
}

static void test_restore_without_process_is_interrupted(void) {
  sm_diag_record_t rec;
  restore_running_launch(&rec, false);
  assert(rec.restored && sm_diag_record_unfinished(&rec));
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                          9 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED && rec.interrupted);
  assert(!sm_diag_record_unfinished(&rec) && !sm_diag_record_is_active(&rec));
  for (int stage = SM_DIAG_STAGE_MOUNTED; stage <= SM_DIAG_STAGE_PROCESS_STARTED; ++stage)
    assert(rec.stages[stage].state == SM_DIAG_OK);
  assert(rec.stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_NOT_OBSERVED);
  assert(rec.stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_NOT_OBSERVED);
  assert(strstr(rec.stages[SM_DIAG_STAGE_EXITED].detail, "UniMount restarted"));
  assert(!rec.has_diagnosis); // nothing was observed that explains anything
  struct json_object *summary = sm_diag_record_summary_json(&rec);
  struct json_object *value = NULL;
  assert(json_object_object_get_ex(summary, "outcome", &value) &&
         !strcmp(json_object_get_string(value), "observation_interrupted"));
  assert(json_object_object_get_ex(summary, "observation_interrupted", &value) &&
         json_object_get_boolean(value));
  json_object_put(summary);
  // Persisted again, it stays final.
  struct json_object *json = sm_diag_record_to_json(&rec);
  sm_diag_record_t again;
  assert(sm_diag_record_from_json(json, &again));
  json_object_put(json);
  assert(again.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED &&
         !sm_diag_record_unfinished(&again));

  // A reused PID (other application) or an unprovable identity is not the
  // same launch.
  restore_running_launch(&rec, false);
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_OTHER, k_wall0 + 60000,
                                          9 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);
  restore_running_launch(&rec, false);
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_UNVERIFIED,
                                          k_wall0 + 60000, 9 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);

  // Evidence observed before the interruption keeps its diagnosis.
  restore_running_launch(&rec, true);
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                          9 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);
  assert(rec.has_diagnosis && !strcmp(rec.diagnosis.category, "corrupt_data"));

  // Files written by the previous build: interrupted but still in progress.
  restore_running_launch(&rec, false);
  rec.interrupted = true;
  assert(sm_diag_record_unfinished(&rec));
  (void)sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                        9 * SECOND);
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);

  // A launch that never got a process is resolved as well.
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *pending = launch(&store, 0, 0);
  struct json_object *pending_json = sm_diag_record_to_json(pending);
  assert(sm_diag_record_from_json(pending_json, &rec));
  json_object_put(pending_json);
  sm_diag_store_free(&store);
  assert(sm_diag_record_unfinished(&rec));
  (void)sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                        9 * SECOND);
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);
  assert(rec.stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_NOT_OBSERVED);
}

static void test_restore_with_same_process_reattaches(void) {
  sm_diag_record_t rec;
  restore_running_launch(&rec, false);
  // New run: its monotonic clock restarted; 10 s of wall time have passed.
  const uint64_t mono = 100 * SECOND;
  const int64_t wall = k_wall0 + 10000;
  assert(sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_SAME, wall, mono));
  assert(!rec.restored && !rec.interrupted);
  assert(rec.outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  assert(sm_diag_record_unfinished(&rec));
  // Process start (wall +1 s) mapped 9 s before "now" on the new clock.
  assert(rec.process_start_mono_us == mono - 9 * SECOND);
  // Keep observing: alive past the confirmation time -> running.
  assert(sm_diag_record_liveness_due(&rec, mono));
  uint64_t t = mono;
  int64_t w = wall;
  for (int i = 0; i < 14; ++i, t += SECOND, w += 1000)
    (void)sm_diag_record_note_liveness(&rec, probe_policy(), SM_DIAG_PROC_SAME, w, t);
  assert(rec.outcome == SM_DIAG_OUTCOME_RUNNING);
  // Then it vanishes without an exit event: two gone probes.
  t += 10 * SECOND;
  assert(!sm_diag_record_note_liveness(&rec, probe_policy(), SM_DIAG_PROC_GONE, w, t));
  t += 5 * SECOND;
  assert(sm_diag_record_note_liveness(&rec, probe_policy(), SM_DIAG_PROC_GONE, w, t));
  assert(rec.outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY);
  assert(strstr(rec.stages[SM_DIAG_STAGE_EXITED].detail, "exit not observed directly"));
  assert(!sm_diag_record_liveness_due(&rec, t + 60 * SECOND));
}

static void test_missed_exit_reconciliation(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  // Without a positive probe the daemon policy never confirms running.
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 200, 1);
  (void)sm_diag_record_evaluate(rec, probe_policy(), k_wall0 + 30000,
                                k_mono0 + 30 * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);

  // Dies after 6 s, exit missed: exited_early.
  rec = launch(&store, 0, 100);
  start_process(&store, 201, 101);
  for (int s = 102; s <= 106; ++s)
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  assert(!sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                       k_wall0 + 107000, k_mono0 + 107 * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS); // one probe is not enough
  assert(sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                      k_wall0 + 108000, k_mono0 + 108 * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_EARLY);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_FAILED);
  assert(strstr(rec->stages[SM_DIAG_STAGE_EXITED].detail, "exit not observed directly"));

  // A reused PID counts as gone.
  rec = launch(&store, 0, 200);
  start_process(&store, 202, 201);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_OTHER,
                                     k_wall0 + 202000, k_mono0 + 202 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_OTHER,
                                     k_wall0 + 203000, k_mono0 + 203 * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_EARLY);

  // Gone after the confirmation time without a probe proving it lived that
  // long (probes were missed): honest answer is observation_interrupted.
  rec = launch(&store, 0, 300);
  start_process(&store, 203, 301);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                     k_wall0 + 303000, k_mono0 + 303 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 340000, k_mono0 + 340 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 341000, k_mono0 + 341 * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED && rec->interrupted);
  assert(rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_NOT_OBSERVED);

  // NOTE_EXIT seen but the final exit never reported: concluded after the
  // grace, using the observed exit time.
  rec = launch(&store, 0, 400);
  start_process(&store, 204, 401);
  for (int s = 402; s <= 425; ++s)
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  pid_event(&store, SM_DIAG_EV_PROCESS_EXIT, 204, 430);
  for (int s = 431; s < 490; s += 5) {
    rec->next_liveness_mono_us = 0;
    assert(!sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                         k_wall0 + s * 1000,
                                         k_mono0 + (uint64_t)s * SECOND));
  }
  rec->next_liveness_mono_us = 0;
  assert(sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                      k_wall0 + 491000, k_mono0 + 491 * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY);
  assert(rec->exit_mono_us == k_mono0 + 430 * SECOND);

  // The lifecycle watcher re-reporting a live re-attached PID does not open
  // a second record.
  rec = launch(&store, 0, 600);
  start_process(&store, 205, 601);
  rec->reattached = true; // as sm_diag_record_resolve_restored() leaves it
  size_t before = sm_diag_store_count(&store);
  assert(start_process(&store, 205, 640) == rec && sm_diag_store_count(&store) == before);
  sm_diag_store_free(&store);
}

// --- Review fixes ---

static size_t timeline_json_size(const sm_diag_record_t *rec, char *out,
                                 size_t size) {
  struct json_object *json = sm_diag_record_to_json(rec);
  const char *text = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
  strlcpy(out, text, size);
  json_object_put(json);
  return rec->timeline_count;
}

// 1. Resolving restored records is idempotent, including a crashed launch
// whose process was still alive when UniMount stopped.
static void test_restore_is_idempotent(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *live = launch(&store, 0, 0);
  start_process(&store, 77, 1);
  sm_diag_input_t finding = event(SM_DIAG_EV_FINDING, 2);
  finding.pid = 77;
  finding.finding_kind = SM_DIAG_FINDING_FATAL;
  strlcpy(finding.text, "signal: 11 (SIGSEGV)", sizeof(finding.text));
  apply(&store, finding);
  assert(live->outcome == SM_DIAG_OUTCOME_CRASHED && sm_diag_record_unfinished(live));
  struct json_object *json = sm_diag_record_to_json(live);
  sm_diag_record_t rec;
  assert(sm_diag_record_from_json(json, &rec));
  json_object_put(json);
  sm_diag_store_free(&store);

  static char first[65536], again[65536];
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                          100 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_CRASHED && rec.interrupted);
  assert(rec.stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_NOT_OBSERVED);
  assert(!sm_diag_record_unfinished(&rec));
  size_t events = timeline_json_size(&rec, first, sizeof(first));
  for (int restart = 0; restart < 3; ++restart) {
    // Persist, restore, resolve again: even a matching live PID must not
    // re-attach a finished record.
    json = sm_diag_record_to_json(&rec);
    assert(sm_diag_record_from_json(json, &rec));
    json_object_put(json);
    assert(!sm_diag_record_unfinished(&rec));
    assert(!sm_diag_record_resolve_restored(&rec, restart == 1 ? SM_DIAG_PROC_SAME
                                                               : SM_DIAG_PROC_GONE,
                                            k_wall0 + 60000 + restart * 1000,
                                            (100u + (unsigned)restart) * SECOND));
    assert(timeline_json_size(&rec, again, sizeof(again)) == events);
    assert(!strcmp(first, again));
    assert(!sm_diag_record_finalise_for_shutdown(&rec, policy(), SM_DIAG_PROC_GONE,
                                                 k_wall0, 100 * SECOND));
  }
  // Records of the previous build (interrupted, still in progress) are
  // resolved exactly once.
  sm_diag_record_t old;
  restore_running_launch(&old, false);
  old.interrupted = true;
  assert(sm_diag_record_unfinished(&old));
  (void)sm_diag_record_resolve_restored(&old, SM_DIAG_PROC_GONE, k_wall0 + 60000,
                                        100 * SECOND);
  size_t old_events = old.timeline_count;
  assert(!sm_diag_record_unfinished(&old));
  (void)sm_diag_record_resolve_restored(&old, SM_DIAG_PROC_GONE, k_wall0 + 61000,
                                        101 * SECOND);
  assert(old.timeline_count == old_events);
}

// 2. A failed running_confirmed (fatal finding) is never overwritten.
static void test_vanish_keeps_failed_confirmation(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 88, 1);
  sm_diag_input_t finding = event(SM_DIAG_EV_FINDING, 3);
  finding.pid = 88;
  finding.finding_kind = SM_DIAG_FINDING_FATAL;
  strlcpy(finding.text, "signal: 11 (SIGSEGV)", sizeof(finding.text));
  apply(&store, finding);
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_FAILED);
  for (int s = 4; s <= 30; ++s)
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 31000, k_mono0 + 31 * SECOND);
  assert(sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                      k_wall0 + 32000, k_mono0 + 32 * SECOND));
  assert(rec->stages[SM_DIAG_STAGE_RUNNING_CONFIRMED].state == SM_DIAG_FAILED);
  assert(rec->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK);
  assert(rec->outcome == SM_DIAG_OUTCOME_CRASHED && !rec->interrupted);
  sm_diag_store_free(&store);
}

// 3. A record from a previous boot is never re-attached.
static void test_previous_boot_is_not_reattached(void) {
  sm_diag_record_t rec;
  restore_running_launch(&rec, false);
  // Rebooted: uptime 30 s, but the process started an hour ago.
  assert(!sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_SAME,
                                          k_wall0 + 3600000, 30 * SECOND));
  assert(rec.outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED && rec.interrupted);
  // The same record while this boot has run long enough is re-attached.
  restore_running_launch(&rec, false);
  assert(sm_diag_record_resolve_restored(&rec, SM_DIAG_PROC_SAME, k_wall0 + 3600000,
                                         7200 * SECOND));
  assert(rec.process_start_mono_us > 1u);
}

// 4. A handoff cannot revive a concluded exit, and resets the gone count.
static void test_handoff_after_reconciled_exit(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 90, 1);
  for (int s = 2; s <= 22; ++s)
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 30000, k_mono0 + 30 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 35000, k_mono0 + 35 * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY);
  sm_diag_input_t handoff = event(SM_DIAG_EV_PROCESS_HANDOFF, 36);
  handoff.pid = 90;
  handoff.new_pid = 91;
  assert(apply(&store, handoff) == NULL);
  assert(rec->pid == 90 && rec->outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY);

  // One gone probe, then a handoff: the new PID needs two gone probes again.
  rec = launch(&store, 0, 100);
  start_process(&store, 92, 101);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                     k_wall0 + 102000, k_mono0 + 102 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 103000, k_mono0 + 103 * SECOND);
  handoff = event(SM_DIAG_EV_PROCESS_HANDOFF, 103.5);
  handoff.pid = 92;
  handoff.new_pid = 93;
  assert(apply(&store, handoff) == rec && rec->pid == 93);
  assert(!sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                       k_wall0 + 104500, k_mono0 + 104500000ull));
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS);
  sm_diag_store_free(&store);
}

// 5. Shutdown with an observed NOTE_EXIT concludes the exit.
static void test_shutdown_with_observed_exit(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 94, 1);
  for (int s = 2; s <= 22; ++s)
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  pid_event(&store, SM_DIAG_EV_PROCESS_EXIT, 94, 40);
  // Still alive at shutdown: kept in progress.
  assert(!sm_diag_record_finalise_for_shutdown(rec, probe_policy(), SM_DIAG_PROC_UNKNOWN,
                                               k_wall0 + 41000, k_mono0 + 41 * SECOND));
  assert(sm_diag_record_finalise_for_shutdown(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                              k_wall0 + 42000, k_mono0 + 42 * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_EXITED_NORMALLY && !rec->interrupted);
  assert(rec->exit_mono_us == k_mono0 + 40 * SECOND);
  // Without an observed exit the launch is interrupted.
  rec = launch(&store, 0, 100);
  start_process(&store, 95, 101);
  assert(sm_diag_record_finalise_for_shutdown(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                              k_wall0 + 105000, k_mono0 + 105 * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);
  sm_diag_store_free(&store);
}

// 6. An unreadable identity neither confirms nor ends a launch (bounded).
static void test_unknown_identity(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *rec = launch(&store, 0, 0);
  start_process(&store, 96, 1);
  for (unsigned i = 0; i + 1u < SM_DIAG_UNKNOWN_PROBES; ++i)
    assert(!sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_UNKNOWN,
                                         k_wall0 + 2000 + (int64_t)i * 1000,
                                         k_mono0 + (2u + i) * SECOND));
  assert(rec->outcome == SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS); // not running past 20 s
  // Unknown probes interleaved with one gone probe do not add up to "gone".
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_GONE,
                                     k_wall0 + 40000, k_mono0 + 40 * SECOND);
  (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_SAME,
                                     k_wall0 + 41000, k_mono0 + 41 * SECOND);
  assert(rec->outcome == SM_DIAG_OUTCOME_RUNNING);
  for (unsigned i = 0; i < SM_DIAG_UNKNOWN_PROBES; ++i) {
    rec->next_liveness_mono_us = 0;
    (void)sm_diag_record_note_liveness(rec, probe_policy(), SM_DIAG_PROC_UNKNOWN,
                                       k_wall0 + 50000 + (int64_t)i * 5000,
                                       k_mono0 + (50u + 5u * i) * SECOND);
  }
  assert(rec->outcome == SM_DIAG_OUTCOME_OBSERVATION_INTERRUPTED);
  sm_diag_store_free(&store);

  // At restore an unknown identity keeps the launch for reconciliation.
  sm_diag_record_t restored;
  restore_running_launch(&restored, false);
  assert(sm_diag_record_resolve_restored(&restored, SM_DIAG_PROC_UNKNOWN,
                                         k_wall0 + 10000, 100 * SECOND));
  assert(!restored.restored && sm_diag_record_unfinished(&restored));
  assert(restored.last_alive_mono_us == 0);
  (void)sm_diag_record_note_liveness(&restored, probe_policy(), SM_DIAG_PROC_SAME,
                                     k_wall0 + 30000, 120 * SECOND);
  assert(restored.outcome == SM_DIAG_OUTCOME_RUNNING);
}

// 7. A same-title relaunch on a reused PID after a missed exit opens its own
// record (bound to ShellCore's LAUNCH) and closes the old one.
static void test_pid_reuse_relaunch(void) {
  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  sm_diag_record_t *old = launch(&store, 0, 0);
  start_process(&store, 97, 1);
  for (int s = 2; s <= 22; ++s)
    (void)sm_diag_record_note_liveness(old, probe_policy(), SM_DIAG_PROC_SAME,
                                       k_wall0 + s * 1000, k_mono0 + (uint64_t)s * SECOND);
  sm_diag_record_t *next = launch(&store, 0, 25);
  assert(next != old);
  assert(start_process(&store, 97, 26) == next);
  assert(next->stages[SM_DIAG_STAGE_PROCESS_STARTED].state == SM_DIAG_OK);
  assert(old->stages[SM_DIAG_STAGE_EXITED].state == SM_DIAG_OK);
  assert(strstr(old->stages[SM_DIAG_STAGE_EXITED].detail, "exit not observed directly"));
  sm_diag_store_free(&store);
}

// --- Native error history ---

static const char k_bom[] = "\xEF\xBB\xBF";

static void parse_sample(const char *json, sm_diag_native_error_t *out) {
  char buffer[2048];
  snprintf(buffer, sizeof(buffer), "%s%s", k_bom, json);
  memset(out, 0, sizeof(*out));
  strlcpy(out->file, "0000001D.json", sizeof(out->file));
  out->mtime = 1795201919;
  assert(sm_diag_parse_native_error(buffer, strlen(buffer), out));
  assert(!strcmp(out->file, "0000001D.json") && out->mtime == 1795201919);
}

static void test_error_history_parser(void) {
  sm_diag_native_error_t error;
  char hex[11];
  parse_sample("{\"__appId\":7,\"internalErrorCode\":-2136342502,\"logReport\":\"\","
               "\"messageBuffer\":\"Can't start the game or app.\",\"shortError\":"
               "\"CE-107750-0\",\"suggestionType\":-1,\"titleId\":\"NPXS40087\","
               "\"tick\":63930798719305000,\"natType\":4,\"alreadyRead\":false,"
               "\"displayVersion\":\"26.05-13.60.00.07-00.00.00.0.1\"}",
               &error);
  assert(error.parsed && error.has_internal_code);
  assert(error.internal_code == -2136342502);
  sm_diag_format_code_hex(error.internal_code, hex);
  assert(!strcmp(hex, "0x80AA001A"));
  assert(!strcmp(error.short_error, "CE-107750-0"));
  assert(!strcmp(error.title_id, "NPXS40087"));
  assert(!strcmp(error.message, "Can't start the game or app."));
  assert(!strcmp(error.display_version, "26.05-13.60.00.07-00.00.00.0.1"));
  assert(error.has_tick && error.tick == 63930798719305000ull);
  assert(error.has_app_id && error.app_id == 7);
  assert(!strcmp(sm_diag_native_code_category(0x80AA001Au), "permission"));

  parse_sample("{\"__appId\":8199,\"internalErrorCode\":-2137784263,\"logReport\":\"\","
               "\"messageBuffer\":\"Can't continue using this game or app. The data "
               "is corrupted. Delete it from your PS5, and then download it again "
               "from your library.\",\"shortError\":\"CE-105779-9\",\"suggestionType\":-1,"
               "\"titleId\":\"PPSA03355\",\"tick\":63930729211188000,\"natType\":4,"
               "\"alreadyRead\":false,\"displayVersion\":\"OnionHEN v0.9.0 (13.200.006 )\"}",
               &error);
  sm_diag_format_code_hex(error.internal_code, hex);
  assert(!strcmp(hex, "0x80940039") && !strcmp(error.title_id, "PPSA03355"));
  assert(!strcmp(error.display_version, "OnionHEN v0.9.0 (13.200.006 )"));

  parse_sample("{\"__appId\":7,\"internalErrorCode\":-2137784269,\"logReport\":\"\","
               "\"messageBuffer\":\"Can't start the game or app.\",\"shortError\":"
               "\"CE-105773-3\",\"suggestionType\":-1,\"titleId\":\"NPXS40087\","
               "\"tick\":63930806263138000,\"natType\":4,\"alreadyRead\":false,"
               "\"displayVersion\":\"26.05-13.60.00.07-00.00.00.0.1\"}",
               &error);
  sm_diag_format_code_hex(error.internal_code, hex);
  assert(!strcmp(hex, "0x80940033"));

  parse_sample("{\"__appId\":7,\"internalErrorCode\":-2135293933,\"logReport\":\"\","
               "\"messageBuffer\":\"Something went wrong with the system software.\\n\","
               "\"shortError\":\"CE-108262-9\",\"suggestionType\":-1,\"titleId\":"
               "\"NPXS40082\",\"tick\":63930822900427000,\"natType\":1,"
               "\"alreadyRead\":false,\"displayVersion\":\"26.05-13.60.00.07-00.00.00.0.1\"}",
               &error);
  sm_diag_format_code_hex(error.internal_code, hex);
  assert(!strcmp(hex, "0x80BA0013"));
  assert(!strcmp(error.message, "Something went wrong with the system software.\n"));
  assert(sm_diag_native_code_category(0x80BA0013u) == NULL);

  // Tolerant: garbage, truncated JSON, arrays and missing fields.
  memset(&error, 0, sizeof(error));
  assert(!sm_diag_parse_native_error("\xEF\xBB\xBF{\"shortError\":", 17, &error));
  assert(!error.parsed);
  assert(!sm_diag_parse_native_error("\x01\x02garbage", 9, &error));
  assert(!sm_diag_parse_native_error("[1,2]", 5, &error));
  assert(!sm_diag_parse_native_error("", 0, &error));
  assert(sm_diag_parse_native_error("{\"internalErrorCode\":\"x\"}", 25, &error));
  assert(error.parsed && !error.has_internal_code && error.short_error[0] == '\0');
  struct json_object *json = sm_diag_native_error_to_json(&error);
  struct json_object *value = NULL;
  assert(json_object_object_get_ex(json, "internal_error_code_hex", &value) && !value);
  json_object_put(json);

  // JSON output keeps the signed value and the hex form.
  parse_sample("{\"internalErrorCode\":-2136342502,\"shortError\":\"CE-107750-0\"}", &error);
  json = sm_diag_native_error_to_json(&error);
  assert(json_object_object_get_ex(json, "internal_error_code", &value) &&
         json_object_get_int64(value) == -2136342502);
  assert(json_object_object_get_ex(json, "internal_error_code_hex", &value) &&
         !strcmp(json_object_get_string(value), "0x80AA001A"));
  assert(json_object_object_get_ex(json, "category", &value) &&
         !strcmp(json_object_get_string(value), "permission"));
  json_object_put(json);
}

// --- Classification table ---

static void failed_record(sm_diag_record_t *rec) {
  sm_diag_record_init(rec, 1, TITLE, k_wall0, k_mono0);
  rec->outcome = SM_DIAG_OUTCOME_SPAWN_FAILED;
}

static void add_native(sm_diag_record_t *rec, int32_t code, const char *short_error,
                       const char *title, const char *attribution) {
  sm_diag_native_error_t *error = &rec->native_errors[rec->native_error_count++];
  memset(error, 0, sizeof(*error));
  strlcpy(error->file, "0000001D.json", sizeof(error->file));
  error->parsed = true;
  error->has_internal_code = true;
  error->internal_code = code;
  strlcpy(error->short_error, short_error, sizeof(error->short_error));
  strlcpy(error->title_id, title, sizeof(error->title_id));
  strlcpy(error->attribution, attribution, sizeof(error->attribution));
}

static void add_rtld(sm_diag_record_t *rec, const char *text) {
  sm_diag_finding_t *finding = &rec->findings[rec->finding_count++];
  finding->kind = SM_DIAG_FINDING_RTLD;
  strlcpy(finding->text, text, sizeof(finding->text));
}

static void add_kline_text(sm_diag_record_t *rec, int kind, bool attributed,
                           const char *text) {
  sm_diag_kline_t *line = &rec->kernel_lines[rec->kernel_line_count++];
  line->kind = kind;
  line->attributed = attributed;
  strlcpy(line->text, text, sizeof(line->text));
}

static const sm_diag_diagnosis_t *classify(sm_diag_record_t *rec) {
  static sm_diag_diagnosis_t out;
  assert(sm_diag_classify(rec, &out));
  return &out;
}

static void test_fs_availability(void) {
  char reason[160];
  // vfs.conflist missing (PS5): mounts present now prove the filesystem.
  assert(sm_diag_fs_availability("nullfs", -1, 1074, false, reason, sizeof(reason)) == 1);
  assert(!strcmp(reason, "1074 nullfs mount(s) present"));
  // None present, but UniMount mounted one earlier in this process.
  assert(sm_diag_fs_availability("unionfs", -1, 0, true, reason, sizeof(reason)) == 1);
  assert(!strcmp(reason, "mounted successfully this session"));
  // No evidence either way: unknown, never "unavailable".
  assert(sm_diag_fs_availability("pfs", -1, 0, false, reason, sizeof(reason)) == -1);
  assert(!strcmp(reason, "not yet observed on this boot"));
  assert(!strstr(reason, "conflist"));
  // A readable kernel list still decides when it is available.
  assert(sm_diag_fs_availability("unionfs", 1, 0, false, reason, sizeof(reason)) == 1);
  assert(strstr(reason, "registered in the kernel"));
  assert(sm_diag_fs_availability("unionfs", 0, 0, false, reason, sizeof(reason)) == 0);
  assert(sm_diag_fs_availability("unionfs", 0, 2, false, reason, sizeof(reason)) == 1);
}

static void test_error_history_keeps_newest(void) {
  enum { CAP = 4, FILES = 11 };
  sm_diag_history_entry_t kept[CAP];
  size_t count = 0;
  // readdir order is arbitrary: newest files arrive first, last and between.
  const int64_t mtimes[FILES] = {500, 10, 20, 900, 30, 40, 700, 50, 800, 60, 600};
  for (size_t i = 0; i < FILES; ++i) {
    sm_diag_history_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.name, sizeof(entry.name), "e%02zu.json", i);
    entry.mtime = mtimes[i];
    sm_diag_history_keep_newest(kept, &count, CAP, &entry);
  }
  assert(count == CAP);
  qsort(kept, count, sizeof(kept[0]), sm_diag_history_compare);
  assert(kept[0].mtime == 900 && kept[1].mtime == 800 &&
         kept[2].mtime == 700 && kept[3].mtime == 600);
}

static void test_classification_rules(void) {
  sm_diag_record_t rec;
  const sm_diag_diagnosis_t *d;

  // permission: native code, title attribution -> observed.
  failed_record(&rec);
  add_native(&rec, -2136342502, "CE-107750-0", TITLE, "title_id");
  d = classify(&rec);
  assert(!strcmp(d->category, "permission") && !strcmp(d->confidence, "observed"));
  assert(strstr(d->evidence[0], "0x80AA001A") && strstr(d->evidence[0], "CE-107750-0"));
  // ... a system-title error inside the window is circumstantial.
  failed_record(&rec);
  add_native(&rec, -2136342502, "CE-107750-0", "NPXS40087", "time_window");
  d = classify(&rec);
  assert(!strcmp(d->category, "permission") && !strcmp(d->confidence, "inferred"));
  // ... kernel-log spawn error.
  failed_record(&rec);
  add_kline_text(&rec, SM_DIAG_KLINE_SPAWN_ERROR, true,
                 "<118>[Syscore App] processSpawn() error: 0x80aa001a");
  d = classify(&rec);
  assert(!strcmp(d->category, "permission") && !strcmp(d->confidence, "observed"));
  // ... rtld names the file.
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_EXITED_EARLY;
  add_rtld(&rec, "[rtld] <99> ERROR self_load_shared_object: mount flag / attribute "
                 "error (path=/app0/sce_module/libc.prx)");
  d = classify(&rec);
  assert(!strcmp(d->category, "permission"));
  assert(strstr(d->advice, "libc.prx"));

  // mount_busy and corrupt_data.
  failed_record(&rec);
  add_native(&rec, -2137784269, "CE-105773-3", "NPXS40087", "time_window");
  d = classify(&rec);
  assert(!strcmp(d->category, "mount_busy") && !strcmp(d->confidence, "inferred"));
  failed_record(&rec);
  add_native(&rec, -2137784263, "CE-105779-9", TITLE, "title_id");
  d = classify(&rec);
  assert(!strcmp(d->category, "corrupt_data") && !strcmp(d->confidence, "observed"));
  assert(strstr(d->advice, "re-copy"));

  // firmware_incompatible: param.json requirement above the console.
  failed_record(&rec);
  rec.has_required_fw = true;
  rec.required_fw_bcd = 0x1360;
  strlcpy(rec.required_fw_raw, "0x1360000000000000", sizeof(rec.required_fw_raw));
  rec.console_fw_raw = 0x13400000u;
  d = classify(&rec);
  assert(!strcmp(d->category, "firmware_incompatible") && !strcmp(d->confidence, "inferred"));
  assert(strstr(d->evidence[0], "13.60") && strstr(d->evidence[0], "13.40"));
  assert(strstr(d->advice, "/data/homebrew/backports/" TITLE "/"));
  // ... same firmware is not a mismatch.
  rec.console_fw_raw = 0x13600000u;
  d = classify(&rec);
  assert(!strcmp(d->category, "unknown"));
  // ... PRX_NOT_RESOLVED_FUNCTION fatal report.
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_CRASHED;
  rec.findings[0].kind = SM_DIAG_FINDING_FATAL;
  strlcpy(rec.findings[0].text, "0xa0020101 (PRX_NOT_RESOLVED_FUNCTION)\nthread name: eboot.bin",
          sizeof(rec.findings[0].text));
  rec.finding_count = 1;
  d = classify(&rec);
  assert(!strcmp(d->category, "firmware_incompatible") && !strcmp(d->confidence, "observed"));

  // missing_library lists the modules.
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_EXITED_EARLY;
  add_rtld(&rec, "[rtld] <99> ERROR do_load_object:4355: C: Load error (libSceFoo.sprx)");
  add_kline_text(&rec, SM_DIAG_KLINE_RTLD, true,
                 "[rtld] <99> ERROR do_load_object:4355: C: Load error (libBar.prx)");
  d = classify(&rec);
  assert(!strcmp(d->category, "missing_library"));
  assert(strstr(d->advice, "libSceFoo.sprx") && strstr(d->advice, "libBar.prx"));

  // fakelib_conflict: the failing module came from the overlay (issue #227).
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_EXITED_EARLY;
  strlcpy(rec.fakelib_source, "/data/shadowmount/cache/PPSA15035", sizeof(rec.fakelib_source));
  strlcpy(rec.fakelib_files[0], "libSceAmpr.sprx", SM_DIAG_FILE_NAME_SIZE);
  strlcpy(rec.fakelib_files[1], "libSceAppContent.sprx", SM_DIAG_FILE_NAME_SIZE);
  rec.fakelib_file_count = 2;
  add_kline_text(&rec, SM_DIAG_KLINE_AUTH, false,
                 "[KERNEL] self_pager_activate:857: sceSblAuthMgrAuthHeader returned "
                 "unexpected error 46");
  add_rtld(&rec, "[rtld] <99> ERROR self_load_shared_object:2758: B: error 46 (libSceAmpr.sprx)");
  add_rtld(&rec, "[rtld] <99> ERROR do_load_object:4355: C: Load error (libSceAmpr.sprx)");
  d = classify(&rec);
  assert(!strcmp(d->category, "fakelib_conflict") && !strcmp(d->confidence, "observed"));
  assert(strstr(d->advice, "libSceAmpr.sprx"));
  bool auth_quoted = false;
  for (size_t i = 0; i < d->evidence_count; ++i)
    auth_quoted = auth_quoted || strstr(d->evidence[i], "sceSblAuthMgrAuthHeader");
  assert(auth_quoted);
  // ... the same rtld lines without the overlay are a missing library.
  rec.fakelib_file_count = 0;
  d = classify(&rec);
  assert(!strcmp(d->category, "missing_library"));

  // kernel_patch.
  failed_record(&rec);
  rec.kstuff_loaded = 0;
  rec.hooks_installed = 1;
  d = classify(&rec);
  assert(!strcmp(d->category, "kernel_patch") && !strcmp(d->confidence, "inferred"));

  // mount_failed has priority.
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_MOUNT_FAILED;
  sm_diag_record_set_stage(&rec, SM_DIAG_STAGE_MOUNTED, SM_DIAG_FAILED, k_wall0, k_mono0,
                           "launch mount failed");
  rec.mount_status = EIO;
  rec.mount_errno = ENOENT;
  strlcpy(rec.mount_layer, "image", sizeof(rec.mount_layer));
  add_native(&rec, -2136342502, "CE-107750-0", TITLE, "title_id");
  d = classify(&rec);
  assert(!strcmp(d->category, "mount_failed") && strstr(d->evidence[0], "layer=image"));

  // Unknown code stays unknown and keeps raw evidence.
  failed_record(&rec);
  add_native(&rec, -2135293933, "CE-108262-9", "NPXS40082", "time_window");
  add_kline_text(&rec, SM_DIAG_KLINE_SPAWN_ERROR, false,
                 "[Syscore App] processSpawn() error: 0x80020011");
  d = classify(&rec);
  assert(!strcmp(d->category, "unknown"));
  assert(d->evidence_count >= 2 && strstr(d->evidence[0], "0x80BA0013") &&
         strstr(d->evidence[1], "0x80020011"));

  // No diagnosis for running or successfully exited launches.
  sm_diag_diagnosis_t none;
  failed_record(&rec);
  rec.outcome = SM_DIAG_OUTCOME_RUNNING;
  add_native(&rec, -2136342502, "CE-107750-0", TITLE, "title_id");
  assert(!sm_diag_classify(&rec, &none));
  rec.outcome = SM_DIAG_OUTCOME_EXITED_NORMALLY;
  assert(!sm_diag_classify(&rec, &none));
  rec.outcome = SM_DIAG_OUTCOME_LAUNCH_IN_PROGRESS;
  assert(!sm_diag_classify(&rec, &none));
}

// The hardware case on firmware 13.60: a PS5 debug .pkg a store app installed
// natively. Not managed (mount_link, ENOENT), LAUNCH_FAILED 0.2 s after the
// request, no process, CE-109297-8 filed under the title.
#define INSTALLED_TITLE "PPSA01234"

static sm_diag_record_t *installed_pkg_launch(sm_diag_store_t *store,
                                              const char *title, int pkg_debug,
                                              uint32_t console_fw_raw) {
  sm_diag_input_t input = event(SM_DIAG_EV_LAUNCH, 0);
  strlcpy(input.title_id, title, sizeof(input.title_id));
  input.status = ENOENT;
  input.native_errno = ENOENT;
  strlcpy(input.layer, "mount_link", sizeof(input.layer));
  sm_diag_record_t *rec = apply(store, input);
  rec->console_fw_raw = console_fw_raw;
  sm_diag_input_t failed = event(SM_DIAG_EV_LAUNCH_FAILED, 0.2);
  strlcpy(failed.title_id, title, sizeof(failed.title_id));
  assert(apply(store, failed) == rec);
  rec->installed_pkg_debug = pkg_debug;
  char json[512];
  snprintf(json, sizeof(json),
           "{\"__appId\":7,\"internalErrorCode\":-2132213704,\"logReport\":\"\","
           "\"messageBuffer\":\"Can't start the game or app.\",\"shortError\":"
           "\"CE-109297-8\",\"suggestionType\":-1,\"titleId\":\"%s\","
           "\"natType\":4,\"alreadyRead\":false,"
           "\"displayVersion\":\"26.05-13.60.00.07-00.00.00.0.1\"}",
           title);
  sm_diag_native_error_t *error = &rec->native_errors[rec->native_error_count++];
  parse_sample(json, error);
  strlcpy(error->attribution, "title_id", sizeof(error->attribution));
  evaluate(rec, 10.3);
  assert(rec->outcome == SM_DIAG_OUTCOME_SPAWN_FAILED && rec->managed == 0);
  assert(strstr(rec->stages[SM_DIAG_STAGE_PROCESS_STARTED].detail,
                "LAUNCH_FAILED and no game process was observed"));
  return rec;
}

static void test_installed_debug_pkg(void) {
  const unsigned char fih[] = {0x7F, 'F', 'I', 'H', 0x00};
  const unsigned char retail[] = {0x7F, 'C', 'N', 'T'};
  assert(sm_diag_debug_pkg_magic(fih, sizeof(fih)) == 1);
  assert(sm_diag_debug_pkg_magic(retail, sizeof(retail)) == 0);
  assert(sm_diag_debug_pkg_magic(fih, 3) == -1);
  assert(sm_diag_debug_pkg_magic(NULL, 0) == -1);
  assert(sm_diag_native_debug_pkg_unavailable(0x1360, NULL));
  assert(sm_diag_native_debug_pkg_unavailable(0x1360, "kstuff-lite"));
  assert(!sm_diag_native_debug_pkg_unavailable(0x1360, "kstuff"));
  assert(!sm_diag_native_debug_pkg_unavailable(0x1160, NULL));

  sm_diag_store_t store;
  assert(sm_diag_store_init(&store));
  // Header read and \x7FFIH: observed.
  sm_diag_record_t *rec = installed_pkg_launch(&store, INSTALLED_TITLE, 1, 0x13600000u);
  const sm_diag_diagnosis_t *d = &rec->diagnosis;
  assert(rec->has_diagnosis && !strcmp(d->category, "installed_debug_pkg"));
  assert(!strcmp(d->confidence, "observed"));
  assert(!strcmp(d->rule, "installed_debug_pkg_0x80E90038"));
  assert(strstr(d->evidence[0], "0x80E90038") && strstr(d->evidence[0], "CE-109297-8") &&
         strstr(d->evidence[0], "attribution=title_id"));
  bool magic_quoted = false, firmware_quoted = false;
  for (size_t i = 0; i < d->evidence_count; ++i) {
    magic_quoted = magic_quoted ||
        strstr(d->evidence[i], "/user/app/" INSTALLED_TITLE "/app.pkg starts with \\x7FFIH");
    firmware_quoted = firmware_quoted || strstr(d->evidence[i], "13.60");
  }
  assert(magic_quoted && firmware_quoted);
  assert(strstr(d->advice, "/data/homebrew") && strstr(d->advice, "11.60") &&
         strstr(d->advice, "Orbit Store"));
  // The category and the fact survive launches.json.
  struct json_object *json = sm_diag_record_to_json(rec);
  sm_diag_record_t restored;
  assert(sm_diag_record_from_json(json, &restored));
  json_object_put(json);
  assert(restored.installed_pkg_debug == 1 &&
         !strcmp(restored.diagnosis.category, "installed_debug_pkg"));

  // Header not readable (missing file): inferred.
  rec = installed_pkg_launch(&store, "PPSA01235", -1, 0x13600000u);
  assert(!strcmp(rec->diagnosis.category, "installed_debug_pkg") &&
         !strcmp(rec->diagnosis.confidence, "inferred"));
  // Another magic (retail or encrypted package): not this cause.
  rec = installed_pkg_launch(&store, "PPSA01236", 0, 0x13600000u);
  assert(!strcmp(rec->diagnosis.category, "unknown") &&
         !strcmp(rec->diagnosis.rule, "no_rule_matched"));
  // Firmware 11.60 installs debug packages natively.
  rec = installed_pkg_launch(&store, "PPSA01237", 1, 0x11600000u);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  // Firmware unknown.
  rec = installed_pkg_launch(&store, "PPSA01238", 1, 0);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  // A PS4 title id.
  rec = installed_pkg_launch(&store, "CUSA01234", 1, 0x13600000u);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  // Managed by UniMount.
  rec = installed_pkg_launch(&store, "PPSA01239", 1, 0x13600000u);
  rec->managed = 1;
  evaluate(rec, 10.4);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  // Another internal code.
  rec = installed_pkg_launch(&store, "PPSA01240", 1, 0x13600000u);
  rec->native_errors[0].internal_code = -2135293933;  // 0x80BA0013
  evaluate(rec, 10.4);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  // No console error attached.
  rec = installed_pkg_launch(&store, "PPSA01241", 1, 0x13600000u);
  rec->native_error_count = 0;
  evaluate(rec, 10.4);
  assert(!strcmp(rec->diagnosis.category, "unknown"));
  sm_diag_store_free(&store);

  // A process was observed (it exited early): not a LAUNCH_FAILED spawn.
  sm_diag_record_t early;
  failed_record(&early);
  strlcpy(early.title_id, INSTALLED_TITLE, sizeof(early.title_id));
  early.managed = 0;
  early.outcome = SM_DIAG_OUTCOME_EXITED_EARLY;
  early.pid = 99;
  early.launch_failed = true;
  early.installed_pkg_debug = 1;
  early.console_fw_raw = 0x13600000u;
  add_native(&early, -2132213704, "CE-109297-8", INSTALLED_TITLE, "title_id");
  assert(!strcmp(classify(&early)->category, "unknown"));
}

// --- param.json firmware comparison ---

static void test_param_firmware(void) {
  uint32_t bcd = 0;
  char raw[24];
  // Real value from upstream ShadowMountPlus issue #197 (PPSA24913 01.001.001).
  const char *param = "\xEF\xBB\xBF{\"contentId\":\"JP0700-PPSA24913_00-MAINGAME00000000\","
                      "\"requiredSystemSoftwareVersion\":\"0x1360000000000000\","
                      "\"sdkVersion\":\"0x1200000000000000\"}";
  assert(sm_diag_parse_required_fw(param, strlen(param), &bcd, raw));
  assert(bcd == 0x1360 && !strcmp(raw, "0x1360000000000000"));
  char text[8];
  sm_diag_format_fw_bcd(bcd, text);
  assert(!strcmp(text, "13.60"));
  // kernel_get_fw_version() carries BCD major.minor in bits 31..16.
  assert(sm_diag_compare_fw(0x1360, 0x13400000u) > 0);
  assert(sm_diag_compare_fw(0x1360, 0x13600000u) == 0);
  assert(sm_diag_compare_fw(0x1360, 0x13600123u) == 0);
  assert(sm_diag_compare_fw(0x1120, 0x13600000u) < 0);
  // Decimal order across the byte boundary: 9.60 < 10.01.
  assert(sm_diag_compare_fw(0x0960, 0x10010000u) < 0);
  assert(sm_diag_compare_fw(0x1001, 0x09600000u) > 0);
  sm_diag_format_fw_bcd(0x0920, text);
  assert(!strcmp(text, "9.20"));
  // Issue #227 value: 0x1120... is 11.20.
  const char *param227 = "{\"requiredSystemSoftwareVersion\":\"0x1120000000000000\"}";
  assert(sm_diag_parse_required_fw(param227, strlen(param227), &bcd, raw) && bcd == 0x1120);
  // Numeric form.
  const char *numeric = "{\"requiredSystemSoftwareVersion\":1396397359461564416}";
  assert(sm_diag_parse_required_fw(numeric, strlen(numeric), &bcd, raw) && bcd == 0x1361);
  // Malformed or missing.
  const char *bad[] = {"{\"requiredSystemSoftwareVersion\":\"0xZZ\"}",
                       "{\"requiredSystemSoftwareVersion\":\"0x1A60000000000000\"}",
                       "{\"sdkVersion\":\"0x1200000000000000\"}", "not json"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    assert(!sm_diag_parse_required_fw(bad[i], strlen(bad[i]), &bcd, raw));
}

// --- PLDMGR autoload / meta ---

static void test_pldmgr_kstuff(void) {
  sm_diag_pldmgr_kstuff_t info;
  const char *autoload = "!2000\n  local/etahen-lite  \n\nrepo/kstuff-lite@e8cce5d1\r\n"
                         "repo/shadowmount@12345678\n";
  assert(sm_diag_pldmgr_find_kstuff(autoload, strlen(autoload), &info));
  assert(!strcmp(info.entry, "repo/kstuff-lite@e8cce5d1"));
  assert(!strcmp(info.slot_key, "repo/kstuff-lite@e8cce5d1"));
  assert(!strcmp(info.implementation, "kstuff-lite"));
  assert(info.version[0] == '\0');
  // meta.json as written by PLDMGR slot_write_meta_unlocked().
  const char *meta = "{\n  \"schema\": 2,\n  \"kind\": \"repo\",\n  \"filename\": "
                     "\"kstuff-lite_v1.11.elf\",\n  \"installed_at\": "
                     "\"2026-11-01T10:00:00Z\",\n  \"id\": \"kstuff-lite\",\n"
                     "  \"source_url\": \"https://example.invalid/repo.json\",\n"
                     "  \"name\": \"kstuff-lite\",\n  \"version\": \"1.11\"\n}\n";
  assert(sm_diag_pldmgr_parse_meta(meta, strlen(meta), &info));
  assert(!strcmp(info.version, "1.11"));
  assert(!strcmp(info.filename, "kstuff-lite_v1.11.elf"));
  assert(!strcmp(info.implementation, "kstuff-lite"));
  // A meta without a version never gets one guessed (not even from the file
  // name).
  const char *local_meta = "{\"schema\":2,\"kind\":\"local\",\"filename\":"
                           "\"kstuff_v1.6.elf\",\"origin\":\"web_upload\"}";
  sm_diag_pldmgr_kstuff_t local;
  assert(sm_diag_pldmgr_find_kstuff("local/kstuff\n", 13, &local));
  assert(sm_diag_pldmgr_parse_meta(local_meta, strlen(local_meta), &local));
  assert(local.version[0] == '\0' && !strcmp(local.implementation, "kstuff"));
  // Absolute paths and bare names have no slot.
  assert(sm_diag_pldmgr_find_kstuff("/data/payloads/kstuff.elf\n", 26, &info));
  assert(info.slot_key[0] == '\0' && !strcmp(info.filename, "kstuff.elf"));
  assert(!strcmp(info.implementation, "kstuff"));
  assert(!sm_diag_pldmgr_find_kstuff("repo/etahen@1\n!500\n", 19, &info));
  assert(!strcmp(info.implementation, "unknown"));
  // Slot keys become path components: reject traversal and odd characters.
  assert(sm_diag_pldmgr_slot_key_valid("local/kstuff-lite-2"));
  assert(!sm_diag_pldmgr_slot_key_valid("repo/../../etc"));
  assert(!sm_diag_pldmgr_slot_key_valid("repo/a/b"));
  assert(!sm_diag_pldmgr_slot_key_valid("repo/"));
  assert(!sm_diag_pldmgr_slot_key_valid("kstuff-lite@e8cce5d1"));
  assert(sm_diag_pldmgr_find_kstuff("repo/../kstuff\n", 15, &info) &&
         info.slot_key[0] == '\0');
  assert(!strcmp(sm_diag_kstuff_implementation("kstuff-lite_v1.11.e"), "kstuff-lite"));
}

// --- Kernel log, rtld, sandbox mounts, AMPR ---

static void test_kernel_log_scan(void) {
  // Issue #189 / #227 formats.
  const char *log =
      "old line before the launch\n"
      "ANCHOR-LINE\n"
      "<118>[Syscore App] createApp PPSA99999\n"
      "<118>[Syscore App] processSpawn() error: 0x80020011\n"
      "<118>[Syscore App] createApp PPSA03355\n"
      "<118>[Syscore App] appParam: appType = 0, titleId = PPSA03355\n"
      "<118>[Syscore App] An error occured while creating new process. error=0x80aa001a errno=13\n"
      "<118>[Syscore App] processSpawn() error: 0x80aa001a\n"
      "[rtld] <98> ERROR do_load_object:4355: C: Load error (other.sprx)\n"
      "[KERNEL] self_pager_activate:857: sceSblAuthMgrAuthHeader returned unexpected error 46\n"
      "[rtld] <99> ERROR self_load_shared_object:2758: B: error 46 (libSceAmpr.sprx)\r\n"
      "#\n# A user thread receives a fatal signal\n#\n"
      "# exception: 0xa0020104 (PRX_PROCESS_STARTUP_FAILURE)\n"
      "# thread ID: 101893\n# proc ID: 99\n"
      "# exception: 0x1 (OTHER)\n# proc ID: 300\n";
  size_t offset = sm_diag_log_offset_after_anchor(log, strlen(log), "ANCHOR-LINE\n", 12);
  assert(offset > 0 && !strncmp(log + offset, "<118>[Syscore App] createApp PPSA99999", 38));
  // An anchor that rolled out of the ring means everything left is newer.
  assert(sm_diag_log_offset_after_anchor(log, strlen(log), "gone", 4) == 0);

  sm_diag_kline_t lines[SM_DIAG_MAX_KERNEL_LINES];
  size_t count = sm_diag_scan_kernel_log(log + offset, strlen(log) - offset,
                                         TITLE, 99, lines, SM_DIAG_MAX_KERNEL_LINES);
  assert(count == 5);
  assert(lines[0].kind == SM_DIAG_KLINE_SPAWN_ERROR && lines[0].attributed &&
         strstr(lines[0].text, "errno=13"));
  assert(lines[1].kind == SM_DIAG_KLINE_SPAWN_ERROR && strstr(lines[1].text, "0x80aa001a"));
  assert(lines[2].kind == SM_DIAG_KLINE_AUTH && !lines[2].attributed);
  assert(lines[3].kind == SM_DIAG_KLINE_RTLD && lines[3].attributed &&
         strstr(lines[3].text, "libSceAmpr.sprx") && !strchr(lines[3].text, '\r'));
  assert(lines[4].kind == SM_DIAG_KLINE_EXCEPTION && strstr(lines[4].text, "0xa0020104"));
  // Without a process there are no rtld/auth lines for this launch.
  count = sm_diag_scan_kernel_log(log + offset, strlen(log) - offset, TITLE, 0,
                                  lines, SM_DIAG_MAX_KERNEL_LINES);
  assert(count == 2);
  // A spawn error with no title context is kept but not attributed.
  const char *bare = "[Syscore App] processSpawn() error: 0x80aa001a\n";
  count = sm_diag_scan_kernel_log(bare, strlen(bare), TITLE, 0, lines, 4);
  assert(count == 1 && !lines[0].attributed);

  char module[SM_DIAG_FILE_NAME_SIZE];
  assert(sm_diag_rtld_module("[rtld] ERROR self_load_shared_object: mount flag / attribute "
                             "error (path=/app0/sce_module/libc.prx)", module) &&
         !strcmp(module, "libc.prx"));
  assert(sm_diag_rtld_module("C: Load error (libSceAmpr.sprx)", module) &&
         !strcmp(module, "libSceAmpr.sprx"));
  assert(!sm_diag_rtld_module("ERROR no parenthesis", module));
  assert(!sm_diag_rtld_module("error (not a module name)", module));
}

static void test_sandbox_labels(void) {
  char label[16];
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/app0", label) &&
         !strcmp(label, "app0"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/app0/kraken", label) &&
         !strcmp(label, "kraken_pkg"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_001/abcdEF/common/lib", label) &&
         !strcmp(label, "fakelib"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/data", label) &&
         !strcmp(label, "data"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/mnt/usb1", label) &&
         !strcmp(label, "usb"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/mnt/ext1", label) &&
         !strcmp(label, "ext"));
  assert(sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_000/preinst/common/font",
                                     label) && !strcmp(label, "fonts"));
  assert(!sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA033550_000/app0", label));
  assert(!sm_diag_sandbox_layer_label(TITLE, "/mnt/sandbox/PPSA03355_x/app0", label));
  assert(!sm_diag_sandbox_layer_label(TITLE, "/system_ex/app/PPSA03355", label));
}

static void test_ampr_version(const char *bundled_path, const char *version_path) {
  static const unsigned char blob[] = "\x7f" "ELF....ampr_emu\0ampr-emu-ng 9.9\0x";
  char version[32];
  assert(sm_diag_ampr_version(blob, sizeof(blob), version) && !strcmp(version, "9.9"));
  static const unsigned char partial[] = "ampr-emu-ng beta\0ampr-emu-ng 1.2.3x";
  assert(!sm_diag_ampr_version(partial, sizeof(partial) - 1u, version));
  if (!bundled_path)
    return;
  // The bundled runtime identifies itself with its VERSION file's number.
  FILE *file = fopen(bundled_path, "rb");
  assert(file);
  static unsigned char data[4u * 1024u * 1024u];
  size_t size = fread(data, 1, sizeof(data), file);
  fclose(file);
  assert(sm_diag_ampr_version(data, size, version));
  char expected[32] = "";
  file = fopen(version_path, "r");
  assert(file && fgets(expected, sizeof(expected), file));
  fclose(file);
  expected[strcspn(expected, "\r\n")] = '\0';
  assert(!strcmp(expected[0] == 'v' ? expected + 1 : expected, version));
}

int main(int argc, char **argv) {
  test_mounted_registered_but_no_process_is_not_running();
  test_process_dies_after_six_seconds_is_exited_early();
  test_launch_failed_is_spawn_failed();
  test_running_then_exited_normally();
  test_handoff_keeps_process_alive();
  test_fatal_signal_is_crashed();
  test_mount_failure_and_unmanaged_titles();
  test_error_history_keeps_newest();
  test_fs_availability();
  test_process_without_launch_request();
  test_store_keeps_last_64();
  test_json_round_trip();
  test_restore_without_process_is_interrupted();
  test_restore_with_same_process_reattaches();
  test_missed_exit_reconciliation();
  test_restore_is_idempotent();
  test_vanish_keeps_failed_confirmation();
  test_previous_boot_is_not_reattached();
  test_handoff_after_reconciled_exit();
  test_shutdown_with_observed_exit();
  test_unknown_identity();
  test_pid_reuse_relaunch();
  test_error_history_parser();
  test_classification_rules();
  test_installed_debug_pkg();
  test_param_firmware();
  test_pldmgr_kstuff();
  test_kernel_log_scan();
  test_sandbox_labels();
  test_ampr_version(argc > 2 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
  puts("Diagnostics: launch outcomes, native error history, classification, "
       "firmware requirement, PLDMGR kstuff and kernel-log selection passed");
  return 0;
}
