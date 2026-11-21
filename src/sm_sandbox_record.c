#include "sm_platform.h"

#include <pthread.h>

#include "sm_log.h"
#include "sm_sandbox_record.h"
#include "sm_takeover.h"
#include "sm_unimount.h"

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  sm_record_fakelib_t fakelib;
  sm_record_redirect_set_t sets[SM_SESSION_MAX_REDIRECT_SETS];
  size_t set_count;
  uint64_t generation;
  bool running;
  bool stop;
  pthread_t thread;
} g_record = {.mutex = PTHREAD_MUTEX_INITIALIZER,
              .cond = PTHREAD_COND_INITIALIZER};

typedef struct {
  sm_record_fakelib_t fakelib;
  sm_record_redirect_set_t sets[SM_SESSION_MAX_REDIRECT_SETS];
  size_t set_count;
  sm_session_record_t record;
  bool file_present;  // last action was a write, not a removal
  bool error_logged;
} record_writer_t;

void sm_session_record_publish_fakelib(const sm_record_fakelib_t *snapshot) {
  if (!snapshot)
    return;
  pthread_mutex_lock(&g_record.mutex);
  if (memcmp(&g_record.fakelib, snapshot, sizeof(*snapshot)) != 0) {
    memcpy(&g_record.fakelib, snapshot, sizeof(*snapshot));
    ++g_record.generation;
    pthread_cond_signal(&g_record.cond);
  }
  pthread_mutex_unlock(&g_record.mutex);
}

void sm_session_record_publish_redirects(const sm_record_redirect_set_t *sets,
                                         size_t count) {
  if (count > SM_SESSION_MAX_REDIRECT_SETS || (count != 0 && !sets))
    return;
  pthread_mutex_lock(&g_record.mutex);
  if (count != g_record.set_count ||
      (count != 0 && memcmp(g_record.sets, sets, count * sizeof(*sets)) != 0)) {
    if (count != 0)
      memcpy(g_record.sets, sets, count * sizeof(*sets));
    g_record.set_count = count;
    ++g_record.generation;
    pthread_cond_signal(&g_record.cond);
  }
  pthread_mutex_unlock(&g_record.mutex);
}

static void log_record_error(record_writer_t *writer, const char *action) {
  if (writer->error_logged)
    return;
  writer->error_logged = true;
  log_debug("[ADOPT] cannot %s %s: %s", action, SANDBOX_SESSION_FILE,
            strerror(errno));
}

static void remove_record(record_writer_t *writer) {
  if (unlink(SANDBOX_SESSION_FILE) == 0) {
    if (writer->file_present)
      log_debug("[ADOPT] session record removed");
  } else if (errno != ENOENT) {
    log_record_error(writer, "remove");
    return;
  }
  writer->file_present = false;
}

static bool write_record_file(const char *text) {
  char temporary[] = SANDBOX_SESSION_FILE ".XXXXXX";
  int fd = mkstemp(temporary);
  if (fd < 0)
    return false;
  size_t length = strlen(text);
  bool written = write(fd, text, length) == (ssize_t)length && fsync(fd) == 0;
  int saved_errno = errno;
  written = close(fd) == 0 && written;
  if (!written || rename(temporary, SANDBOX_SESSION_FILE) != 0) {
    if (written)
      saved_errno = errno;
    (void)unlink(temporary);
    errno = saved_errno;
    return false;
  }
  return true;
}

// Owner identity comes from the fakelib session; redirect sets of the same
// title belong to it. Without fakelib layers, the first redirect set does.
static void sync_record(record_writer_t *writer) {
  sm_session_record_t *record = &writer->record;
  const sm_record_fakelib_t *fakelib = &writer->fakelib;
  memset(record, 0, sizeof(*record));
  pid_t pid = 0;
  bool complete = true;
  if (fakelib->layer_count > 0) {
    pid = fakelib->pid;
    complete = fakelib->complete;
    (void)strlcpy(record->title_id, fakelib->title_id, sizeof(record->title_id));
    (void)strlcpy(record->sandbox_app0, fakelib->sandbox_app0,
                  sizeof(record->sandbox_app0));
    (void)strlcpy(record->lib_mount, fakelib->lib_mount,
                  sizeof(record->lib_mount));
    record->layer_count = fakelib->layer_count;
    memcpy(record->layers, fakelib->layers,
           fakelib->layer_count * sizeof(record->layers[0]));
  } else if (writer->set_count > 0) {
    pid = writer->sets[0].pid;
    (void)strlcpy(record->title_id, writer->sets[0].title_id,
                  sizeof(record->title_id));
    (void)strlcpy(record->sandbox_app0, writer->sets[0].redirects.target,
                  sizeof(record->sandbox_app0));
  }
  for (size_t i = 0; record->title_id[0] && i < writer->set_count; ++i) {
    const sm_record_redirect_set_t *set = &writer->sets[i];
    if (strcmp(set->title_id, record->title_id) != 0)
      continue;
    complete = complete && set->complete;
    record->redirects[record->redirect_count++] = set->redirects;
  }
  if (sm_session_record_empty(record)) {
    remove_record(writer);
    return;
  }

  record->complete = complete;
  record->boot_time = sm_ownership_boot_time();
  record->owner_pid = (int64_t)getpid();
  record->game_pid = pid > 0 ? (int64_t)pid : 0;
  app_info_t info;
  memset(&info, 0, sizeof(info));
  if (pid > 0 && sceKernelGetAppInfo(pid, &info) == 0)
    record->app_id = info.app_id;

  char *text = NULL;
  if (!sm_session_record_format(record, UNIMOUNT_VERSION, &text)) {
    errno = ENOMEM;
    log_record_error(writer, "format");
    return;
  }
  bool written = write_record_file(text);
  free(text);
  if (!written) {
    log_record_error(writer, "write");
    return;
  }
  writer->file_present = true;
  log_debug("[ADOPT] session record written: %s pid=%ld app_id=0x%08X "
            "layers=%zu redirect_sets=%zu",
            record->title_id, (long)pid, record->app_id, record->layer_count,
            record->redirect_count);
}

static void *record_thread_main(void *arg) {
  record_writer_t *writer = arg;
  uint64_t written = 0;
  pthread_mutex_lock(&g_record.mutex);
  for (;;) {
    while (!g_record.stop && g_record.generation == written)
      pthread_cond_wait(&g_record.cond, &g_record.mutex);
    if (g_record.generation == written)
      break;  // stopping, nothing left to write
    uint64_t generation = g_record.generation;
    writer->fakelib = g_record.fakelib;
    writer->set_count = g_record.set_count;
    memcpy(writer->sets, g_record.sets,
           g_record.set_count * sizeof(writer->sets[0]));
    pthread_mutex_unlock(&g_record.mutex);
    sync_record(writer);
    written = generation;
    pthread_mutex_lock(&g_record.mutex);
  }
  pthread_mutex_unlock(&g_record.mutex);
  free(writer);
  return NULL;
}

bool sm_session_record_start(void) {
  pthread_mutex_lock(&g_record.mutex);
  if (g_record.running) {
    pthread_mutex_unlock(&g_record.mutex);
    return true;
  }
  record_writer_t *writer = calloc(1, sizeof(*writer));
  if (!writer) {
    pthread_mutex_unlock(&g_record.mutex);
    log_debug("[ADOPT] session record writer unavailable: %s", strerror(ENOMEM));
    return false;
  }
  // A record left by an earlier instance is replaced (or removed) at once.
  writer->file_present = true;
  g_record.stop = false;
  ++g_record.generation;
  int rc = pthread_create(&g_record.thread, NULL, record_thread_main, writer);
  g_record.running = rc == 0;
  pthread_mutex_unlock(&g_record.mutex);
  if (rc != 0) {
    free(writer);
    log_debug("[ADOPT] session record writer unavailable: %s", strerror(rc));
    return false;
  }
  return true;
}

void sm_session_record_stop(void) {
  pthread_mutex_lock(&g_record.mutex);
  bool running = g_record.running;
  g_record.running = false;
  g_record.stop = true;
  pthread_cond_signal(&g_record.cond);
  pthread_mutex_unlock(&g_record.mutex);
  if (running)
    (void)pthread_join(g_record.thread, NULL);
}
