#ifndef SM_ADOPT_H
#define SM_ADOPT_H

// Adoption of a game session whose owning instance is gone: a starting copy
// that finds a managed game running reads the predecessor's session record
// (sm_sandbox_record.h), verifies it against the live system and, when every
// check passes, removes the recorded layers when the game exits, exactly as
// the owner would have. See docs/unimount-ownership.md.

#include <stdbool.h>

typedef struct {
  // Deferrable stop requests so far (signals and STOP files); *by_signal is
  // true when at least one of them was a signal.
  unsigned (*stop_requests)(bool *by_signal);
  // Thread name taken while adopting, so that a later copy sees a
  // predecessor and does not adopt the same session a second time.
  const char *process_name;
} sm_adopt_hooks_t;

typedef enum {
  SM_ADOPT_NOT_TAKEN = 0,  // refused: keep waiting passively
  SM_ADOPT_RELEASED,       // the game exited and its layers were handled
  SM_ADOPT_STOPPED,        // a stop request ended adoption: exit
} sm_adopt_result_t;

// A live predecessor owns the game: log, in one line, whether its session
// could be adopted. Changes nothing.
void sm_adopt_dry_run(const char *running_title_id);

// No live predecessor: adopt the recorded session if every check passes and
// watch the game until it exits. Logs exactly one line when refused.
sm_adopt_result_t sm_adopt_session(const char *running_title_id,
                                   const sm_adopt_hooks_t *hooks);

#endif
