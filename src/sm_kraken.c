#include "sm_platform.h"

#include "sm_kraken.h"

#include "sm_filesystem.h"
#include "sm_gameinfo.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_pkg.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_shellcore_service.h"
#include "sm_storage.h"
#include "sm_unimount_logic.h"
#include <json-c/json.h>

#define KRAKEN_CACHE LOG_DIR "/kraken"
#define KRAKEN_MARKER ".smp-kraken-source"
#define KRAKEN_MANIFEST ".smp-kraken-built"
#define KRAKEN_REVISION "2"
#define KRAKEN_INITIAL_READER "a20bb8be4578f4826e253e3e6f0ed94e1476683c"
#define KRAKEN_V020_READER "73cc894b88338b617dfe8b173585e94db78cd348"
#define KRAKEN_V021_READER "029386534988d7f70ba3eb2e01d4c285336e5611"
#define KRAKEN_PATCH_REVISION "5"
#define KRAKEN_PATCH_FAST_REVISION "5-fast"
#define KRAKEN_PATCH_VOLUME ".smp-kraken-patch.pak"
#define KRAKEN_ID ".smp-kraken-id"
#define KRAKEN_AMPR_NAME "libSceAmpr.sprx"
#define KRAKEN_AMPR "fakelib2/" KRAKEN_AMPR_NAME
#define KRAKEN_AMPR_MAX_SIZE (16u * 1024u * 1024u)
#define KRAKEN_PLAYGO "fakelib2/libScePlayGo.sprx"
#define KRAKEN_PLAYGO_STATE ".smp-kraken-playgo"

typedef unsigned char package_id_t[32];

extern const unsigned char sm_kraken_runtime_start[];
extern const unsigned char sm_kraken_runtime_end[];
extern const char sm_kraken_revision[];
extern const unsigned char sm_kraken_playgo_start[];
extern const unsigned char sm_kraken_playgo_end[];

typedef ssize_t (*package_read_fn)(void *, uint64_t, unsigned char *, size_t);
typedef int (*package_write_fn)(void *, const unsigned char *, size_t, uint64_t,
                                const unsigned char *, size_t);
typedef int (*package_loose_fn)(void *, const unsigned char *, size_t, uint64_t *);
typedef void (*package_progress_fn)(void *, uint32_t, uint64_t, uint64_t);
typedef int (*package_cancelled_fn)(void *);
typedef bool (*package_load_verified_fn)(void *, unsigned char *, size_t);
typedef void (*package_save_verified_fn)(void *, const unsigned char *, size_t);

extern int sm_kraken_build_index(
    void *ctx, uint64_t size, int64_t mtime, const unsigned char *basename,
    size_t basename_len, package_read_fn read, package_write_fn write,
    package_loose_fn loose, unsigned char *error, size_t error_len);
extern bool sm_kraken_package_id(void *ctx, uint64_t size, package_read_fn read,
                                package_id_t id);
extern int sm_kraken_build_patched_index(
    void *ctx, uint64_t base_size, uint64_t patch_size, int64_t mtime,
    const unsigned char *basename, size_t basename_len, package_read_fn read,
    package_read_fn read_patch, package_write_fn write, package_loose_fn loose,
    package_progress_fn progress, package_cancelled_fn is_cancelled,
    bool fast, package_load_verified_fn load_verified, package_save_verified_fn save_verified,
    unsigned char *error, size_t error_len);

typedef struct {
  char path[MAX_PATH];
  char signature[280];
} patch_record_t;

typedef struct {
  int package_fd;
  int patch_fd;
  int patch_output_fd;
  int output_fd;
  int error;
  char output_path[MAX_PATH];
  const char *staging;
  const sm_kraken_job_t *job;
  const sm_pkg_info_t *base;
} cache_io_t;

static bool join_path(const char *root, const char *name, char out[MAX_PATH]) {
  int written = snprintf(out, MAX_PATH, "%s/%s", root, name);
  if (written < 0 || written >= MAX_PATH) {
    errno = ENAMETOOLONG;
    return false;
  }
  return true;
}

static bool local_package_root(const char *path) {
  if (path_matches_root_or_child(path, "/data") ||
      path_matches_root_or_child(path, "/user/data"))
    return !path_matches_root_or_child(path, LOG_DIR) &&
           !path_matches_root_or_child(path, "/user" LOG_DIR);
  for (int i = 0; i < 10; i++) {
    char root[16];
    snprintf(root, sizeof(root), i < 8 ? "/mnt/usb%d" : "/mnt/ext%d",
             i < 8 ? i : i - 8);
    if (path_matches_root_or_child(path, root))
      return true;
  }
  return false;
}

bool sm_kraken_is_package(const char *path) {
  if (!path || !local_package_root(path) || !strrchr(path, '/'))
    return false;
  const char *name = strrchr(path, '/') + 1;
  const char *suffix = strrchr(name, '.');
  return suffix && strcasecmp(suffix, ".pkg") == 0 && strlen(name) <= 255u &&
         !strpbrk(path, "\\\r\n") && !strstr(path, "/../") && !strstr(path, "/./");
}

bool sm_kraken_is_cache(const char *path) {
  const size_t prefix = sizeof(KRAKEN_CACHE);
  if (!path || strncmp(path, KRAKEN_CACHE "/", prefix) != 0)
    return false;
  const char *title_id = path + prefix;
  return strlen(title_id) == 9u && strncmp(title_id, "PPSA", 4u) == 0 &&
         is_supported_game_title_id(title_id);
}

static bool package_on_selected_storage(const char *path) {
  char canonical[PATH_MAX];
  if (!realpath(path, canonical))
    return false;
  if (local_package_root(canonical) && !sm_pkg_is_installed_content_path(canonical))
    return true;
  errno = EPERM;
  return false;
}

static int open_source(const sm_pkg_info_t *info, pkg_kind_t kind) {
  if (!info || !sm_kraken_is_package(info->path) ||
      info->pkg_kind != kind || strncmp(info->title_id, "PPSA", 4u) != 0 ||
      !is_supported_game_title_id(info->title_id) ||
      sm_pkg_is_installed_content_path(info->path)) {
    errno = EINVAL;
    return -1;
  }
  if (!package_on_selected_storage(info->path))
    return -1;
  int fd = open(info->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return -1;
  struct stat st;
  if (fstat(fd, &st) == 0 && sm_pkg_source_matches(info, &st))
    return fd;
  close(fd);
  errno = EAGAIN;
  return -1;
}

static uint64_t read_le64(const unsigned char bytes[8]) {
  uint64_t result = 0;
  for (size_t i = 0; i < 8u; i++)
    result |= (uint64_t)bytes[i] << (i * 8u);
  return result;
}

bool sm_kraken_can_mount(const sm_pkg_info_t *info) {
  int fd = open_source(info, PKG_KIND_BASE);
  if (fd < 0)
    return false;

  // Check the FIH debug flag and the outer superblock's plaintext marker.
  unsigned char header[0x28] = {0};
  bool supported = pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header) &&
                   memcmp(header, "\177FIH", 4) == 0 && header[5] == 0;
  uint64_t offset = read_le64(header + 0x20);
  const char expected[] = "PPRPLAIN-NOAUTH!";
  char marker[sizeof(expected) - 1u];
  const uint64_t marker_end = 0x370u + sizeof(marker);
  supported = supported && offset >= 0x10000u &&
              info->source_size >= marker_end && offset <= info->source_size - marker_end &&
              pread(fd, marker, sizeof(marker), (off_t)(offset + 0x370u)) ==
                  (ssize_t)sizeof(marker) &&
              memcmp(marker, expected, sizeof(marker)) == 0;
  close(fd);
  return supported;
}

bool sm_kraken_can_apply_patch(const sm_pkg_info_t *info) {
  if (!info || !info->is_lih)
    return false;
  int fd = open_source(info, PKG_KIND_PATCH);
  if (fd < 0)
    return false;
  unsigned char header[0x30] = {0};
  bool valid = pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header) &&
               memcmp(header, "\177LIH", 4) == 0 && header[6] == 1 && header[7] == 0;
  uint64_t fih = read_le64(header + 0x20);
  unsigned char fih_header[8];
  valid = valid && fih >= 0x10000u && fih <= INT64_MAX &&
          fih <= info->source_size - sizeof(fih_header) &&
          pread(fd, fih_header, sizeof(fih_header), (off_t)fih) == (ssize_t)sizeof(fih_header) &&
          memcmp(fih_header, "\177FIH", 4) == 0 && fih_header[5] == 0 &&
          fih_header[6] == 3 && fih_header[7] == 0;
  close(fd);
  return valid;
}

static bool read_cache_marker(const char *cache, char source[MAX_PATH],
                              char *signature, size_t signature_size,
                              patch_record_t *patch) {
  source[0] = '\0';
  char path[MAX_PATH];
  if (!sm_kraken_is_cache(cache) || !join_path(cache, KRAKEN_MARKER, path))
    return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  FILE *marker = fdopen(fd, "r");
  if (!marker) {
    close(fd);
    return false;
  }
  bool valid = fgets(source, MAX_PATH, marker) != NULL;
  char *line_end = valid ? strchr(source, '\n') : NULL;
  if (line_end)
    *line_end = '\0';
  valid = line_end && sm_kraken_is_package(source);
  if (valid && signature)
    valid = signature_size > 0 && fgets(signature, (int)signature_size, marker) &&
            strchr(signature, '\n') != NULL;
  if (valid && patch && fgets(patch->path, sizeof(patch->path), marker)) {
    line_end = strchr(patch->path, '\n');
    if (line_end)
      *line_end = '\0';
    valid = line_end && sm_kraken_is_package(patch->path) &&
        fgets(patch->signature, sizeof(patch->signature), marker) != NULL &&
        strchr(patch->signature, '\n') != NULL;
  }
  fclose(marker);
  if (!valid)
    source[0] = '\0';
  return valid;
}

bool sm_kraken_source(const char *cache, char path[MAX_PATH]) {
  return read_cache_marker(cache, path, NULL, 0, NULL);
}

bool sm_kraken_patch_source(const char *cache, char path[MAX_PATH]) {
  char base[MAX_PATH];
  char signature[280];
  patch_record_t patch = {0};
  if (!read_cache_marker(cache, base, signature, sizeof(signature), &patch) ||
      patch.path[0] == '\0')
    return false;
  strlcpy(path, patch.path, MAX_PATH);
  return true;
}

static const char *signature_reader(const char *signature, const char *format) {
  size_t prefix = strlen(format);
  if (strncmp(signature, format, prefix) || signature[prefix] != ' ')
    return NULL;
  const char *reader = signature + prefix + 1;
  const char *readers[] = {
    sm_kraken_revision, KRAKEN_V021_READER, KRAKEN_V020_READER, KRAKEN_INITIAL_READER,
  };
  for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); i++) {
    size_t length = strlen(readers[i]);
    if (!strncmp(reader, readers[i], length) && reader[length] == ' ')
      return readers[i];
  }
  return NULL;
}

static bool read_ready_source(const char *cache, char source[MAX_PATH], char saved[280],
                              patch_record_t *patch) {
  char index[MAX_PATH];
  if (!read_cache_marker(cache, source, saved, 280, patch) ||
      !join_path(cache, "ampr_assets.index", index) || !path_exists(index)) {
    errno = ENOENT;
    return false;
  }
  return package_on_selected_storage(source);
}

static bool source_stat_matches(const char *saved, const struct stat *st) {
  char expected[280];
  const char *reader = signature_reader(saved, KRAKEN_REVISION);
  if (!reader) {
    errno = ESTALE;
    return false;
  }
  int written = snprintf(expected, sizeof(expected),
                          "%s %s %llx %llx %llx %llx %lx %llx %lx ",
                          KRAKEN_REVISION, reader,
                          (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
                          (unsigned long long)st->st_size, (unsigned long long)st->st_mtime,
                          (unsigned long)st->st_mtim.tv_nsec, (unsigned long long)st->st_ctime,
                          (unsigned long)st->st_ctim.tv_nsec);
  if (!S_ISREG(st->st_mode) || written <= 0 || (size_t)written >= sizeof(expected) ||
      strncmp(saved, expected, (size_t)written) != 0) {
    errno = ESTALE;
    return false;
  }
  return true;
}

bool sm_kraken_source_ready(const char *cache) {
  char source[MAX_PATH], saved[280];
  struct stat st;
  return read_ready_source(cache, source, saved, NULL) &&
      lstat(source, &st) == 0 && source_stat_matches(saved, &st);
}

static bool cancelled(void *unused) {
  (void)unused;
  return should_stop_requested() || runtime_sleep_mode_active();
}

static bool cache_cancelled(const cache_io_t *io) {
  return cancelled(NULL) || (io->job && io->job->cancelled(io->job->ctx));
}

static int patch_cancelled(void *context) {
  cache_io_t *io = context;
  if (!cache_cancelled(io))
    return 0;
  io->error = ECANCELED;
  return 1;
}

static ssize_t cache_read(cache_io_t *io, int fd, uint64_t offset,
                          unsigned char *buffer, size_t length) {
  if (cache_cancelled(io))
    return -(io->error = ECANCELED);
  if (offset > INT64_MAX)
    return -(io->error = EOVERFLOW);
  ssize_t result;
  do {
    result = pread(fd, buffer, length, (off_t)offset);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? -(io->error = errno) : result;
}

static ssize_t package_read(void *context, uint64_t offset, unsigned char *buffer,
                            size_t length) {
  cache_io_t *io = context;
  return cache_read(io, io->package_fd, offset, buffer, length);
}

static ssize_t patch_read(void *context, uint64_t offset, unsigned char *buffer,
                          size_t length) {
  cache_io_t *io = context;
  return cache_read(io, io->patch_fd, offset, buffer, length);
}

static void patch_progress(void *context, uint32_t phase, uint64_t done, uint64_t total) {
  const cache_io_t *io = context;
  io->job->progress(io->job->ctx, phase, done, total);
}

static bool output_path(cache_io_t *io, const unsigned char *name, size_t length,
                        char path[MAX_PATH]) {
  if (length == 0 || length >= MAX_PATH || memchr(name, '\0', length)) {
    errno = EINVAL;
    return false;
  }
  char relative[MAX_PATH];
  memcpy(relative, name, length);
  relative[length] = '\0';
  return join_path(io->staging, relative, path);
}

static bool make_parents(char *path, size_t root_length) {
  for (char *slash = strchr(path + root_length + 1u, '/'); slash;
       slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    bool ready = mkdir(path, 0777) == 0 ? chmod(path, 0777) == 0 : errno == EEXIST;
    *slash = '/';
    if (!ready)
      return false;
  }
  return true;
}

static int close_output(cache_io_t *io) {
  if (io->output_fd < 0)
    return 0;
  const int status = close(io->output_fd);
  io->output_fd = -1;
  io->output_path[0] = '\0';
  return status;
}

static int stage_write(void *context, const unsigned char *name, size_t length,
                       uint64_t offset, const unsigned char *buffer, size_t count) {
  cache_io_t *io = context;
  char path[MAX_PATH];
  if (cache_cancelled(io))
    return io->error = ECANCELED;
  if (offset > INT64_MAX || count > (uint64_t)INT64_MAX - offset)
    return EOVERFLOW;
  if (!output_path(io, name, length, path))
    return errno;
  const bool patch_volume = length == sizeof(KRAKEN_PATCH_VOLUME) - 1u &&
                           memcmp(name, KRAKEN_PATCH_VOLUME, length) == 0;
  int *fd = patch_volume ? &io->patch_output_fd : &io->output_fd;
  if (*fd < 0 || (!patch_volume && strcmp(io->output_path, path) != 0)) {
    if (!patch_volume && close_output(io) != 0)
      return errno;
    if (!make_parents(path, strlen(io->staging)))
      return errno;
    *fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0777);
    if (*fd < 0)
      return io->error = errno;
    if (!patch_volume)
      strlcpy(io->output_path, path, sizeof(io->output_path));
    if (fchmod(*fd, 0777) != 0)
      return io->error = errno;
  }
  while (count > 0) {
    ssize_t written = pwrite(*fd, buffer, count, (off_t)offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return io->error = written == 0 ? EIO : errno;
    offset += (uint64_t)written;
    buffer += written;
    count -= (size_t)written;
  }
  return 0;
}

static bool buffer_contains(const unsigned char *data, size_t size,
                            const char *needle, size_t length) {
  for (size_t i = 0; i + length <= size; i++) {
    if (data[i] == (unsigned char)needle[0] && memcmp(data + i, needle, length) == 0)
      return true;
  }
  return false;
}

static bool eboot_mentions_playgo(const char *path, const cache_io_t *io) {
  const char module[] = "libScePlayGo";
  const size_t length = sizeof(module) - 1u;
  const size_t block = 64u * 1024u;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0)
    return false;
  unsigned char *buffer = malloc(block + length - 1u);
  bool found = false;
  size_t carry = 0;
  while (buffer && !found && !cache_cancelled(io)) {
    ssize_t bytes = read(fd, buffer + carry, block);
    if (bytes < 0 && errno == EINTR)
      continue;
    if (bytes <= 0)
      break;
    size_t total = carry + (size_t)bytes;
    found = buffer_contains(buffer, total, module, length);
    carry = total < length ? total : length - 1u;
    memmove(buffer, buffer + total - carry, carry);
  }
  free(buffer);
  close(fd);
  return found;
}

static bool stage_playgo(cache_io_t *io) {
  char executable[MAX_PATH];
  unsigned char state = '0';
  if (!resolve_sce_sys_file(io->staging, KRAKEN_PLAYGO, executable) &&
      errno == ENOENT) {
    if (!join_path(io->staging, "eboot.bin", executable))
      return false;
    if (eboot_mentions_playgo(executable, io))
      state = '1';
  }

  const char output[] = KRAKEN_PLAYGO;
  int status = 0;
  if (state == '1') {
    status = stage_write(io, (const unsigned char *)output, sizeof(output) - 1u,
        0, sm_kraken_playgo_start, (size_t)(sm_kraken_playgo_end - sm_kraken_playgo_start));
  }
  if (status == 0) {
    const char marker[] = KRAKEN_PLAYGO_STATE;
    status = stage_write(io, (const unsigned char *)marker, sizeof(marker) - 1u,
                         0, &state, 1);
  }
  if (status) {
    errno = status;
    return false;
  }
  return true;
}

static int staged_file_size(void *context, const unsigned char *name,
                            size_t length, uint64_t *size) {
  char path[MAX_PATH];
  if (!output_path(context, name, length, path))
    return -errno;
  struct stat st;
  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : -errno;
  if (!S_ISREG(st.st_mode))
    return -EINVAL;
  *size = (uint64_t)st.st_size;
  return 1;
}

static bool reserved_backport_name(const char *name) {
  return strcasecmp(name, "kraken") == 0 || strcasecmp(name, "kraken.image") == 0 ||
         strcasecmp(name, "ampr_emu.index") == 0 || strcasecmp(name, "ampr_assets.index") == 0 ||
         strcasecmp(name, "ampr_zstd.index") == 0 || strncasecmp(name, ".smp-kraken-", 12) == 0;
}

typedef bool (*tree_file_fn)(const char *, const char *, const struct stat *, void *);

// Keep a single path buffer for the walk. lstat prevents descending symlinks.
static bool walk_tree(char path[MAX_PATH], size_t root_length,
                      tree_file_fn visit, void *context) {
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  size_t length = strlen(path);
  int error = 0;
  for (;;) {
    errno = 0;
    const struct dirent *entry = readdir(dir);
    if (!entry) {
      error = errno;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (cancelled(NULL)) {
      error = ECANCELED;
      break;
    }
    int size = snprintf(path + length, MAX_PATH - length, "/%s", entry->d_name);
    if (size < 0 || (size_t)size >= MAX_PATH - length) {
      error = ENAMETOOLONG;
      break;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !visit(path, path + root_length + 1u, &st, context) ||
        (S_ISDIR(st.st_mode) && !walk_tree(path, root_length, visit, context))) {
      error = errno ? errno : EIO;
      break;
    }
    path[length] = '\0';
  }
  path[length] = '\0';
  if (closedir(dir) != 0 && error == 0)
    error = errno;
  errno = error;
  return error == 0;
}

static bool record_built_path(const char *path, const char *relative,
                              const struct stat *st, void *context) {
  (void)path;
  (void)st;
  if (json_object_object_add(context, relative, NULL) == 0)
    return true;
  errno = ENOMEM;
  return false;
}

static bool write_build_manifest(const char *staging) {
  char path[MAX_PATH];
  struct json_object *built = json_object_new_object();
  if (!built) {
    errno = ENOMEM;
    return false;
  }
  strlcpy(path, staging, sizeof(path));
  bool ready = walk_tree(path, strlen(staging), record_built_path, built) &&
               join_path(staging, KRAKEN_MANIFEST, path) &&
               json_object_to_file_ext(path, built, JSON_C_TO_STRING_PLAIN) == 0;
  json_object_put(built);
  return ready;
}

typedef struct {
  struct json_object *built;
  const char *staging;
} saved_file_copy_t;

static bool copy_game_written_path(const char *path, const char *relative,
                                   const struct stat *st, void *context) {
  saved_file_copy_t *copy = context;
  if (reserved_backport_name(relative) || json_object_object_get_ex(copy->built, relative, NULL))
    return true;

  char target[MAX_PATH];
  if (!join_path(copy->staging, relative, target))
    return false;
  struct stat new_st;
  if (lstat(target, &new_st) == 0)
    return true; // A current build file owns this path.
  if (errno != ENOENT || !make_parents(target, strlen(copy->staging)))
    return false;
  if (S_ISDIR(st->st_mode))
    return mkdir(target, st->st_mode & 0777) == 0;
  if (S_ISREG(st->st_mode))
    return sm_storage_copy_path_progress(path, target, NULL, cancelled, NULL) == 0;
  if (S_ISLNK(st->st_mode)) {
    char link[MAX_PATH];
    ssize_t count = readlink(path, link, sizeof(link) - 1u);
    if (count < 0)
      return false;
    if ((size_t)count == sizeof(link) - 1u) {
      errno = ENAMETOOLONG;
      return false;
    }
    link[count] = '\0';
    return symlink(link, target) == 0;
  }
  return true;
}

static bool preserve_game_written_files(const char *cache, const char *staging) {
  char path[MAX_PATH];
  if (!join_path(cache, KRAKEN_MANIFEST, path))
    return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st;
  struct json_object *built = fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
                                 ? json_object_from_fd(fd) : NULL;
  close(fd);
  if (!built || !json_object_is_type(built, json_type_object)) {
    if (built) json_object_put(built);
    errno = EINVAL;
    return false;
  }
  saved_file_copy_t copy = {.built = built, .staging = staging};
  strlcpy(path, cache, sizeof(path));
  bool ready = walk_tree(path, strlen(cache), copy_game_written_path, &copy);
  json_object_put(built);
  return ready;
}

static uint64_t hash_bytes(uint64_t value, const void *bytes, size_t length) {
  const unsigned char *data = bytes;
  for (size_t i = 0; i < length; i++) {
    value ^= data[i];
    value *= UINT64_C(1099511628211);
  }
  return value;
}

static bool fingerprint_backport_file(const char *path, const char *relative,
                                      const struct stat *st, void *context) {
  (void)path;
  const uint64_t attributes[] = {st->st_dev, st->st_ino, st->st_mode, st->st_size,
                                 st->st_mtime, st->st_mtim.tv_nsec,
                                 st->st_ctime, st->st_ctim.tv_nsec};
  uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), relative, strlen(relative));
  uint64_t *fingerprint = context;
  *fingerprint += hash_bytes(hash, attributes, sizeof(attributes));
  return true;
}

static bool stage_backport(const char *backport, const char *staging,
                           bool use_patched_metadata) {
  if (!*backport)
    return true;
  DIR *dir = opendir(backport);
  if (!dir)
    return false;
  int error = 0;
  struct dirent *entry;
  while (error == 0) {
    errno = 0;
    entry = readdir(dir);
    if (!entry) {
      error = errno;
      break;
    }
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strcasecmp(name, "fakelib") == 0 || strcasecmp(name, "fakelib2") == 0 ||
        reserved_backport_name(name))
      continue;
    if (use_patched_metadata && strcasecmp(name, "sce_sys") == 0)
      continue; // The LIH supplies a coherent version and metadata set.
    char source[MAX_PATH], target[MAX_PATH];
    if (!join_path(backport, name, source) || !join_path(staging, name, target) ||
        sm_storage_copy_path_progress(source, target, NULL, cancelled, NULL) != 0)
      error = errno ? errno : EIO;
  }
  if (closedir(dir) != 0 && error == 0)
    error = errno;
  errno = error;
  return error == 0;
}

static bool stage_backport_libraries(const char *backport, cache_io_t *io) {
  if (!*backport)
    return true;
  char path[MAX_PATH];
  if (!join_path(backport, "fakelib2", path))
    return false;
  DIR *dir = opendir(path);
  if (!dir && errno == ENOENT) {
    if (!join_path(backport, "fakelib", path))
      return false;
    dir = opendir(path);
  }
  if (!dir)
    return errno == ENOENT;

  int error = 0;
  while (error == 0) {
    errno = 0;
    const struct dirent *entry = readdir(dir);
    if (!entry) {
      error = errno;
      break;
    }
    const char *name = entry->d_name;
    if (name[0] == '.' || !strcasecmp(name, "libsceampr.sprx") ||
        !strcasecmp(name, "libkernel.sprx"))
      continue;
    char source[MAX_PATH], target[MAX_PATH], relative[MAX_PATH];
    struct stat st;
    if (!join_path(path, name, source) || lstat(source, &st) != 0) {
      error = errno;
      break;
    }
    if (!S_ISREG(st.st_mode))
      continue;
    if (!join_path("fakelib2", name, relative) ||
        !join_path(io->staging, relative, target) ||
        !make_parents(target, strlen(io->staging)) ||
        sm_storage_copy_path_progress(source, target, NULL, cancelled, NULL) != 0 ||
        chmod(target, 0777) != 0)
      error = errno ? errno : EIO;
  }
  if (closedir(dir) != 0 && error == 0)
    error = errno;
  errno = error;
  return error == 0;
}

static int package_signature_prefix(const sm_pkg_info_t *info, const char *revision,
                                     const char *reader, char *out, size_t size) {
  return snprintf(out, size, "%s %s %llx %llx %llx %llx %lx ",
                         revision, reader,
                         (unsigned long long)info->source_device, (unsigned long long)info->source_inode,
                         (unsigned long long)info->source_size, (unsigned long long)info->source_mtime,
                         (unsigned long)info->source_mtime_ns);
}

static bool write_reader_signature(const sm_pkg_info_t *info, const char *revision,
                                    const char *reader, uint64_t signature, char out[280]) {
  int prefix = package_signature_prefix(info, revision, reader, out, 280);
  if (prefix < 0 || prefix >= 280) {
    errno = EOVERFLOW;
    return false;
  }
  int written = snprintf(out + prefix, 280 - prefix, "%llx %lx %llx\n",
                         (unsigned long long)info->source_ctime,
                         (unsigned long)info->source_ctime_ns, (unsigned long long)signature);
  if (written <= 0 || written >= 280 - prefix) {
    errno = EOVERFLOW;
    return false;
  }
  return true;
}

static bool write_source_signature(const sm_pkg_info_t *info, const char *revision,
                                    uint64_t signature, char out[280]) {
  return write_reader_signature(info, revision, sm_kraken_revision, signature, out);
}

static bool matches_source_signature(const char *saved, const sm_pkg_info_t *info,
                                     const char *format) {
  const char *reader = signature_reader(saved, format);
  char current[280];
  return reader && write_reader_signature(info, format, reader, 0, current) &&
      strcmp(saved, current) == 0;
}

static bool source_signature(const sm_pkg_info_t *info, const char *backport,
                             const char *revision, char out[280]) {
  uint64_t signature = 0;
  if (*backport) {
    char path[MAX_PATH];
    strlcpy(path, backport, sizeof(path));
    signature = hash_bytes(UINT64_C(14695981039346656037), path, strlen(path));
    if (!walk_tree(path, strlen(path), fingerprint_backport_file, &signature))
      return false;
  }
  return write_source_signature(info, revision, signature, out);
}

static bool package_unchanged(const sm_pkg_info_t *info, int fd) {
  struct stat original, current;
  return fstat(fd, &original) == 0 && lstat(info->path, &current) == 0 &&
         sm_pkg_source_matches(info, &original) && sm_pkg_source_matches(info, &current);
}

static FILE *open_cache_temp(const char *target, char temporary[MAX_PATH]) {
  int len = snprintf(temporary, MAX_PATH, "%s.XXXXXX", target);
  if (len < 0 || len >= MAX_PATH) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  int fd = mkstemp(temporary);
  if (fd < 0)
    return NULL;
  FILE *file = fdopen(fd, "wb");
  if (!file) {
    int error = errno;
    close(fd);
    unlink(temporary);
    errno = error;
  }
  return file;
}

static bool finish_cache_temp(FILE *file, const char *temporary, const char *target, bool written) {
  int error = written ? 0 : (errno ? errno : EIO);
  if (error == 0 && (fflush(file) != 0 || fsync(fileno(file)) != 0))
    error = errno;
  if (fclose(file) != 0 && error == 0)
    error = errno;
  if (error == 0 && rename(temporary, target) != 0)
    error = errno;
  if (error)
    unlink(temporary);
  errno = error;
  return error == 0;
}

static bool cached_module_matches(const char *path, const unsigned char *data,
                                  size_t size) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st;
  bool current = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size == (off_t)size;
  unsigned char block[4096];
  for (size_t offset = 0; current && offset < size; offset += sizeof(block)) {
    size_t count = size - offset < sizeof(block) ? size - offset : sizeof(block);
    ssize_t length;
    do {
      length = pread(fd, block, count, (off_t)offset);
    } while (length < 0 && errno == EINTR);
    current = length == (ssize_t)count && memcmp(block, data + offset, count) == 0;
  }
  close(fd);
  return current;
}

static bool write_cache_asset(const char *path, const void *data, size_t size,
                              mode_t mode) {
  char temporary[MAX_PATH];
  FILE *file = open_cache_temp(path, temporary);
  if (!file)
    return false;
  bool written = fchmod(fileno(file), mode) == 0 && fwrite(data, 1, size, file) == size;
  return finish_cache_temp(file, temporary, path, written);
}

static unsigned char *read_shared_ampr(const char *path, size_t *size) {
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return NULL;
  struct stat before, after;
  int error = 0;
  unsigned char *data = NULL;
  size_t length = 0;
  if (fstat(fd, &before) != 0)
    error = errno;
  else if (!S_ISREG(before.st_mode) || before.st_size < 64 ||
           before.st_size > KRAKEN_AMPR_MAX_SIZE)
    error = ENOEXEC;
  else {
    length = (size_t)before.st_size;
    data = malloc(length);
    if (!data)
      error = ENOMEM;
  }
  for (size_t offset = 0; error == 0 && offset < length;) {
    ssize_t count = pread(fd, data + offset, length - offset, (off_t)offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      error = count < 0 ? errno : EIO;
    else
      offset += (size_t)count;
  }
  if (error == 0 && fstat(fd, &after) != 0)
    error = errno;
  if (error == 0 && (before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec))
    error = EAGAIN;
  close(fd);

  // Debug PKG needs AMPR-emu-NG's package reader. The legacy AMPR library
  // may also be present in this directory for the other mount formats.
  static const char pkg_path[] = "/app0/kraken";
  if (error == 0) {
    bool elf = memcmp(data, "\177ELF\2\1", 6) == 0;
    uint64_t self_size = read_le64(data + 16);
    bool self = memcmp(data, "\117\025\075\035", 4) == 0 &&
                self_size >= 64 && self_size <= length;
    if ((!elf && !self) || !buffer_contains(data, length, pkg_path, sizeof(pkg_path) - 1u))
      error = ENOEXEC;
  }
  if (error) {
    free(data);
    errno = error;
    return NULL;
  }
  *size = length;
  return data;
}

// A shared AMPR-emu-NG from <emulators>/ampr-emu-ng (PR #200 layout) wins
// over <emulators>/libSceAmpr.sprx; either must pass read_shared_ampr.
static unsigned char *load_shared_ampr(const char *emulators_path, size_t *size,
                                       char shared_path[MAX_PATH]) {
  if (!emulators_path || !*emulators_path)
    return NULL;
  char ng_path[MAX_PATH], top_path[MAX_PATH];
  unsigned char *ng = NULL, *top = NULL;
  size_t ng_size = 0, top_size = 0;
  if (join_path(emulators_path, "ampr-emu-ng/" KRAKEN_AMPR_NAME, ng_path))
    ng = read_shared_ampr(ng_path, &ng_size);
  if (!ng && errno != ENOENT)
    log_debug("  [KRAKEN] shared AMPR unavailable in %s/ampr-emu-ng: %s",
              emulators_path, strerror(errno));
  if (!ng && join_path(emulators_path, KRAKEN_AMPR_NAME, top_path))
    top = read_shared_ampr(top_path, &top_size);
  if (!ng && !top && errno != ENOENT)
    log_debug("  [KRAKEN] shared AMPR unavailable in %s: %s", emulators_path, strerror(errno));
  switch (sm_ampr_ng_select_source(ng != NULL, top != NULL)) {
  case SM_AMPR_SOURCE_SHARED_NG_DIR:
    (void)strlcpy(shared_path, ng_path, MAX_PATH);
    *size = ng_size;
    return ng;
  case SM_AMPR_SOURCE_SHARED_TOP:
    (void)strlcpy(shared_path, top_path, MAX_PATH);
    *size = top_size;
    return top;
  case SM_AMPR_SOURCE_BUNDLED:
  default:
    return NULL;
  }
}

static bool install_ampr_file(const char *path, const char *emulators_path,
                              const char *label) {
  char shared_path[MAX_PATH];
  size_t size = (size_t)(sm_kraken_runtime_end - sm_kraken_runtime_start);
  unsigned char *shared = load_shared_ampr(emulators_path, &size, shared_path);
  const unsigned char *data = shared ? shared : sm_kraken_runtime_start;
  struct stat st;
  bool current = lstat(path, &st) == 0 && (st.st_mode & 0111) != 0 &&
                 cached_module_matches(path, data, size);
  bool ready = current || write_cache_asset(path, data, size, 0777);
  if (ready && !current)
    log_debug("  [KRAKEN] AMPR for %s: %s", label, shared ? shared_path : "bundled");
  free(shared);
  return ready;
}

static bool refresh_runtime_file(const char *cache, const char *emulators_path) {
  char path[MAX_PATH];
  if (!join_path(cache, KRAKEN_AMPR, path))
    return false;
  return install_ampr_file(path, emulators_path, cache);
}

bool sm_kraken_install_ampr(const char *path, const char *emulators_path,
                            const char *label) {
  return install_ampr_file(path, emulators_path, label ? label : path);
}

static char read_playgo_state(const char *path) {
  char state = '\0';
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return '\0';
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 1) {
    ssize_t length;
    do {
      length = read(fd, &state, 1);
    } while (length < 0 && errno == EINTR);
    if (length != 1)
      state = '\0';
  }
  close(fd);
  return state;
}

bool sm_kraken_has_bundled_playgo(const char *cache) {
  char path[MAX_PATH];
  if (!join_path(cache, KRAKEN_PLAYGO_STATE, path))
    return false;
  char state = read_playgo_state(path);
  if (state == '0' || state == '1')
    return state == '1';
  return join_path(cache, KRAKEN_PLAYGO, path) &&
      cached_module_matches(path, sm_kraken_playgo_start,
                            (size_t)(sm_kraken_playgo_end - sm_kraken_playgo_start));
}

static bool refresh_playgo(const char *cache, bool enabled) {
  char marker[MAX_PATH], path[MAX_PATH];
  if (!join_path(cache, KRAKEN_PLAYGO_STATE, marker) ||
      !join_path(cache, KRAKEN_PLAYGO, path))
    return false;
  char state = read_playgo_state(marker);
  if (state == '0' || (state != '1' && enabled))
    return true;

  const size_t size = (size_t)(sm_kraken_playgo_end - sm_kraken_playgo_start);
  if (enabled) {
    char existing[MAX_PATH];
    if (resolve_sce_sys_file(cache, KRAKEN_PLAYGO, existing))
      return true;
    return errno == ENOENT && write_cache_asset(path, sm_kraken_playgo_start, size, 0777);
  }
  if (!cached_module_matches(path, sm_kraken_playgo_start, size))
    return true;
  // Older caches have no origin marker. The exact bundled stub can be
  // restored from the ELF, so record it before applying the opt-out.
  if (state != '1' && !write_cache_asset(marker, "1", 1, 0600))
    return false;
  return unlink(path) == 0 || errno == ENOENT;
}

static bool read_package_id(const char *cache, package_id_t id) {
  char path[MAX_PATH];
  if (!join_path(cache, KRAKEN_ID, path))
    return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  struct stat st;
  bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
      st.st_size == (off_t)sizeof(package_id_t) &&
      pread(fd, id, sizeof(package_id_t), 0) == (ssize_t)sizeof(package_id_t);
  close(fd);
  return valid;
}

static bool write_package_id(const char *cache, const package_id_t id) {
  char path[MAX_PATH], temporary[MAX_PATH];
  FILE *file = join_path(cache, KRAKEN_ID, path) ? open_cache_temp(path, temporary) : NULL;
  if (!file)
    return false;
  return finish_cache_temp(file, temporary, path,
      fwrite(id, 1, sizeof(package_id_t), file) == sizeof(package_id_t));
}

static bool capture_package_id(const sm_pkg_info_t *base, const sm_kraken_job_t *job,
                                 package_id_t id) {
  int fd = open_source(base, PKG_KIND_BASE);
  if (fd < 0)
    return false;
  cache_io_t io = {.package_fd = fd, .job = job};
  bool ready = sm_kraken_package_id(&io, base->source_size, package_read, id) &&
      package_unchanged(base, fd);
  close(fd);
  return ready;
}

bool sm_kraken_refresh_runtime(const char *cache, const char *emulators_path,
                               bool playgo_stub_enabled) {
  if (!sm_kraken_is_cache(cache)) {
    errno = EINVAL;
    return false;
  }
  if (!refresh_runtime_file(cache, emulators_path))
    return false;
  if (!refresh_playgo(cache, playgo_stub_enabled))
    log_debug("  [KRAKEN] PlayGo policy for %s: %s", cache, strerror(errno));

  // Old caches already contain the files and index needed for launch. Record
  // the portable PKG ID once while that cache still has its verified source.
  package_id_t id;
  char source[MAX_PATH];
  sm_pkg_info_t base;
  if (!read_package_id(cache, id) && sm_kraken_source_ready(cache) &&
      sm_kraken_source(cache, source) && sm_pkg_parse_file(source, NULL, &base) &&
      capture_package_id(&base, NULL, id) && sm_kraken_source_ready(cache))
    (void)write_package_id(cache, id);
  return true;
}

static bool verification_path(const cache_io_t *io, char path[MAX_PATH]) {
  char name[40];
  snprintf(name, sizeof(name), ".verified-%s", io->base->title_id);
  return join_path(KRAKEN_CACHE, name, path);
}

static bool load_verified(void *context, unsigned char *bytes, size_t size) {
  const cache_io_t *io = context;
  char path[MAX_PATH], saved[280];
  if (!verification_path(io, path))
    return false;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return false;
  FILE *file = fdopen(fd, "rb");
  if (!file) {
    close(fd);
    return false;
  }
  bool valid = fgets(saved, sizeof(saved), file) &&
      matches_source_signature(saved, io->base, "verified-1") &&
      fread(bytes, 1, size, file) == size && fgetc(file) == EOF && !ferror(file);
  fclose(file);
  return valid;
}

static void save_verified(void *context, const unsigned char *bytes, size_t size) {
  const cache_io_t *io = context;
  if (cache_cancelled(io) || !package_unchanged(io->base, io->package_fd))
    return;
  char path[MAX_PATH], signature[280], temporary[MAX_PATH];
  if (!verification_path(io, path) ||
      !write_source_signature(io->base, "verified-1", 0, signature))
    return;
  FILE *file = open_cache_temp(path, temporary);
  if (file)
    (void)finish_cache_temp(file, temporary, path,
        fputs(signature, file) >= 0 && fwrite(bytes, 1, size, file) == size);
}

static bool write_cache_marker(const char *cache, const char *source, const char *signature,
                                const patch_record_t *patch) {
  char path[MAX_PATH], temporary[MAX_PATH];
  FILE *file = join_path(cache, KRAKEN_MARKER, path) ? open_cache_temp(path, temporary) : NULL;
  if (!file)
    return false;
  bool written = fprintf(file, "%s\n%s", source, signature) >= 0 &&
      (!patch || fprintf(file, "%s\n%s", patch->path, patch->signature) >= 0);
  return finish_cache_temp(file, temporary, path, written);
}

static void remove_staging(const char *path) {
  if (sm_storage_delete_path(path) != 0 && errno != ENOENT)
    log_debug("  [KRAKEN] temporary cache cleanup failed: %s (%s)", path, strerror(errno));
}

static bool publish_cache(const char *title_id, const char *staging,
                          const char *cache, const char *backup) {
  // The full scan already holds the external mutation guard through registration.
  if (cancelled(NULL) || sm_shellcore_title_is_protected(title_id)) {
    errno = EBUSY;
    return false;
  }
  runtime_mount_state_lock();
  bool mounted = is_data_mounted(title_id);
  bool ready = !mounted || unmount_title_runtime_layers_quiet(title_id);
  struct stat old;
  if (ready && lstat(cache, &old) == 0) {
    if (S_ISDIR(old.st_mode))
      ready = preserve_game_written_files(cache, staging) && rename(cache, backup) == 0;
    else {
      errno = EEXIST;
      ready = false;
    }
  } else if (ready && errno != ENOENT) {
    ready = false;
  }
  if (ready && rename(staging, cache) != 0) {
    int saved = errno;
    if (rename(backup, cache) != 0 && errno != ENOENT)
      log_debug("  [KRAKEN] rollback failed; previous cache kept in %s", backup);
    errno = saved;
    ready = false;
  }
  int error = ready ? 0 : errno;
  runtime_mount_state_unlock();
  errno = error;
  return ready;
}

static bool prepare_cache(const sm_pkg_info_t *info, const sm_pkg_info_t *patch,
                          const sm_kraken_job_t *job, char cache[MAX_PATH]) {
  if (!join_path(KRAKEN_CACHE, info->title_id, cache) || !sm_kraken_is_cache(cache))
    return false;
  char existing[MAX_PATH] = "", current[280], saved[280] = "", patch_signature[280] = "";
  const char *patch_revision = job->fast ? KRAKEN_PATCH_FAST_REVISION : KRAKEN_PATCH_REVISION;
  patch_record_t old_patch = {0};
  char backport[MAX_PATH] = "", package_dir[MAX_PATH], backup[MAX_PATH];
  strlcpy(package_dir, info->path, sizeof(package_dir));
  char *separator = strrchr(package_dir, '/');
  if (!separator)
    return false;
  *separator = '\0';
  (void)resolve_backport_path_for_title(info->title_id, package_dir, backport);
  if (!source_signature(info, backport, KRAKEN_REVISION, current) ||
      (patch && !source_signature(patch, "", patch_revision, patch_signature)))
    return false;

  if (read_cache_marker(cache, existing, saved, sizeof(saved), &old_patch) &&
      strcmp(existing, info->path) == 0) {
    bool intact = sm_kraken_source_ready(cache) && directory_has_game_executable(cache);
    bool same_patch = !patch && !old_patch.path[0];
    if (patch && strcmp(old_patch.path, patch->path) == 0) {
      char full_signature[280];
      same_patch = strcmp(old_patch.signature, patch_signature) == 0 ||
          (job->fast && source_signature(patch, "", KRAKEN_PATCH_REVISION, full_signature) &&
           strcmp(old_patch.signature, full_signature) == 0);
    }
    if (intact && same_patch && strcmp(current, saved) == 0) {
      if (job->finalize(job->ctx))
        return true;
      errno = ECANCELED;
      return false;
    }
  }

  char backup_name[40];
  snprintf(backup_name, sizeof(backup_name), ".previous-%s", info->title_id);
  if (!join_path(KRAKEN_CACHE, backup_name, backup))
    return false;
  if (mkdir(KRAKEN_CACHE, 0777) != 0 && errno != EEXIST)
    return false;
  struct stat st;
  if (lstat(backup, &st) == 0) {
    if (lstat(cache, &st) != 0) {
      if (errno != ENOENT || rename(backup, cache) != 0)
        return false;
    } else if (sm_storage_delete_path(backup) != 0) {
      return false;
    }
  }
  int fd = open_source(info, PKG_KIND_BASE);
  if (fd < 0)
    return false;
  int patch_fd = patch ? open_source(patch, PKG_KIND_PATCH) : -1;
  if (patch && patch_fd < 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return false;
  }
  char staging[] = KRAKEN_CACHE "/.prepare-XXXXXX";
  if (!mkdtemp(staging)) {
    int saved_errno = errno;
    close(fd);
    if (patch_fd >= 0)
      close(patch_fd);
    errno = saved_errno;
    return false;
  }
  cache_io_t io = {.package_fd = fd, .patch_fd = patch_fd, .patch_output_fd = -1,
                   .output_fd = -1, .staging = staging, .job = job, .base = info};
  char package_mount[MAX_PATH];
  char build_error[MAX_PATH + 256] = "";
  const char *name = strrchr(info->path, '/') + 1;
  bool ready = chmod(staging, 0777) == 0 &&
      join_path(staging, SM_KRAKEN_PACKAGE_DIR, package_mount) &&
      mkdir(package_mount, 0777) == 0 && stage_backport(backport, staging, patch != NULL) &&
      stage_backport_libraries(backport, &io);
  if (ready) {
    const unsigned char *basename = (const unsigned char *)name;
    unsigned char *message = (unsigned char *)build_error;
    int result = patch ? sm_kraken_build_patched_index(
        &io, info->source_size, patch->source_size, (int64_t)patch->source_mtime,
        basename, strlen(name), package_read, patch_read, stage_write,
        staged_file_size, patch_progress, patch_cancelled, job->fast, load_verified,
        save_verified, message, sizeof(build_error))
        : sm_kraken_build_index(&io, info->source_size, (int64_t)info->source_mtime,
        basename, strlen(name), package_read, stage_write, staged_file_size,
        message, sizeof(build_error));
    ready = result == 0;
    if (!ready)
      errno = io.error ? io.error : (result == -2 ? EILSEQ : EINVAL);
  }
  if (ready) {
    const char runtime[] = KRAKEN_AMPR;
    int status = stage_write(&io, (const unsigned char *)runtime, sizeof(runtime) - 1u,
        0, sm_kraken_runtime_start, (size_t)(sm_kraken_runtime_end - sm_kraken_runtime_start));
    ready = status == 0;
    if (!ready) errno = status;
  }
  if (ready)
    ready = stage_playgo(&io);
  int error = ready ? 0 : (errno ? errno : EIO);
  if (close_output(&io) != 0 && error == 0)
    error = errno;
  if (io.patch_output_fd >= 0 && close(io.patch_output_fd) != 0 && error == 0)
    error = errno;
  package_id_t id;
  bool have_id = error == 0 && sm_kraken_package_id(&io, info->source_size, package_read, id);
  char final_signature[280];
  if (error == 0 && (!package_unchanged(info, fd) ||
      !source_signature(info, backport, KRAKEN_REVISION, final_signature) ||
      strcmp(current, final_signature) != 0))
    error = EAGAIN;
  close(fd);
  if (patch_fd >= 0) {
    if (error == 0 && !package_unchanged(patch, patch_fd))
      error = EAGAIN;
    close(patch_fd);
  }
  if (error == 0) {
    sm_pkg_info_t extracted = {0};
    if (!sm_pkg_read_directory_metadata(staging, &extracted) ||
        strcmp(extracted.title_id, info->title_id) != 0 ||
        (patch && (strcmp(extracted.content_id, patch->content_id) != 0 ||
                   (patch->app_ver[0] && strcmp(extracted.app_ver, patch->app_ver) != 0))))
      error = EINVAL;
  }
  if (error == 0 && have_id && !write_package_id(staging, id))
    error = errno ? errno : EIO;
  if (error == 0 && !write_build_manifest(staging))
    error = errno ? errno : EIO;
  patch_record_t applied = {0};
  if (patch) {
    strlcpy(applied.path, patch->path, sizeof(applied.path));
    strlcpy(applied.signature, patch_signature, sizeof(applied.signature));
  }
  if (error == 0 && !write_cache_marker(staging, info->path, current, patch ? &applied : NULL))
    error = errno ? errno : EIO;
  if (error == 0 && !job->finalize(job->ctx))
    error = ECANCELED;
  if (error == 0 && !publish_cache(info->title_id, staging, cache, backup))
    error = errno ? errno : EIO;
  if (error == 0) {
    remove_staging(backup);
    if (job->rebuilt)
      *job->rebuilt = true;
    log_debug("  [KRAKEN] cache ready: %s -> %s", info->path, cache);
    if (patch)
      log_debug("  [KRAKEN] LIH applied: %s %s: %s", patch->title_id,
                patch->app_ver, patch->path);
  } else {
    remove_staging(staging);
    log_debug("  [KRAKEN] cache failed: %s: %s", info->path,
              *build_error ? build_error : strerror(error));
  }
  errno = error;
  return error == 0;
}

bool sm_kraken_prepare_base(const sm_pkg_info_t *info, const sm_kraken_job_t *job,
                            char cache[MAX_PATH]) {
  return prepare_cache(info, NULL, job, cache);
}

bool sm_kraken_apply_patch(const sm_pkg_info_t *base, const sm_pkg_info_t *patch,
                           const sm_kraken_job_t *job) {
  if (strcmp(base->title_id, patch->title_id) != 0 || !base->content_id[0] ||
      strcmp(base->content_id, patch->content_id) != 0) {
    log_debug("  [KRAKEN] LIH identity mismatch: base=%s (%s), patch=%s (%s)",
              base->title_id, base->content_id, patch->title_id, patch->content_id);
    errno = EINVAL;
    return false;
  }
  char cache[MAX_PATH];
  return prepare_cache(base, patch, job, cache);
}

static bool relink_candidate(const char *cache, const char *existing, const sm_pkg_info_t *base) {
  struct stat st;
  if (strcmp(existing, base->path) == 0) {
    if (sm_kraken_source_ready(cache))
      return false;
  } else if (lstat(existing, &st) == 0 || (errno != ENOENT && errno != ENOTDIR)) {
    return false;
  }
  sm_pkg_info_t metadata;
  return base->pkg_kind == PKG_KIND_BASE && base->content_id[0] &&
      strcmp(strrchr(cache, '/') + 1, base->title_id) == 0 &&
      sm_pkg_read_directory_metadata(cache, &metadata) &&
      strcmp(base->content_id, metadata.content_id) == 0;
}

bool sm_kraken_can_relink(const char *cache, const sm_pkg_info_t *base) {
  char existing[MAX_PATH];
  return sm_kraken_source(cache, existing) && relink_candidate(cache, existing, base);
}

static bool same_package_after_rename(const sm_pkg_info_t *base, const char *saved) {
  const char *reader = signature_reader(saved, KRAKEN_REVISION);
  if (!reader || base->source_inode == 0)
    return false;
  char prefix[280];
  int length = package_signature_prefix(base, KRAKEN_REVISION, reader, prefix, sizeof(prefix));
  return length > 0 && (size_t)length < sizeof(prefix) &&
      strncmp(saved, prefix, (size_t)length) == 0;
}

static bool cached_backport_fingerprint(const char *saved, uint64_t *backport) {
  const char *last = strrchr(saved, ' ');
  if (!last || !signature_reader(saved, KRAKEN_REVISION))
    return false;
  unsigned long long fingerprint;
  int parsed = 0;
  if (sscanf(last + 1, "%llx%n", &fingerprint, &parsed) != 1 ||
      strcmp(last + 1 + parsed, "\n") != 0)
    return false;
  *backport = fingerprint;
  return true;
}

static void add_source_access(int fd, mode_t mode, mode_t wanted, const char *path) {
  if ((mode & wanted) != wanted && fchmod(fd, (mode & 07777) | wanted) != 0)
    log_debug("  [KRAKEN] could not add read access to %s (%s)", path, strerror(errno));
}

static bool make_launch_source_readable(const char *cache, const char *source, int fd,
                                        const char *saved, const patch_record_t *patch) {
  struct stat before;
  if (fstat(fd, &before) != 0 || !source_stat_matches(saved, &before))
    return false;
  const mode_t wanted = S_IRUSR | S_IRGRP | S_IROTH;
  if ((before.st_mode & wanted) == wanted)
    return true;

  const char *reader = signature_reader(saved, KRAKEN_REVISION);
  uint64_t backport;
  if (!cached_backport_fingerprint(saved, &backport)) {
    errno = EINVAL;
    return false;
  }
  if (fchmod(fd, (before.st_mode & 07777) | wanted) != 0) {
    // Read-only and non-Unix drives can refuse chmod. Let the existing mount try.
    log_debug("  [KRAKEN] could not add read access to %s (%s)", source, strerror(errno));
    return true;
  }
  struct stat after, named;
  if (fstat(fd, &after) != 0 || lstat(source, &named) != 0)
    return false;
  sm_pkg_info_t updated = {
    .source_device = before.st_dev,
    .source_inode = before.st_ino,
    .source_size = before.st_size,
    .source_mtime = before.st_mtime,
    .source_mtime_ns = before.st_mtim.tv_nsec,
    .source_ctime = after.st_ctime,
    .source_ctime_ns = after.st_ctim.tv_nsec,
  };
  if (!sm_pkg_source_matches(&updated, &after) || !sm_pkg_source_matches(&updated, &named)) {
    errno = EAGAIN;
    return false;
  }
  // chmod changes ctime. Keep the cached reader version and the applied LIH record.
  char signature[280];
  return write_reader_signature(&updated, KRAKEN_REVISION, reader, backport, signature) &&
      write_cache_marker(cache, source, signature, patch->path[0] ? patch : NULL);
}

bool sm_kraken_launch_source(const char *cache, char source[MAX_PATH]) {
  char saved[280];
  patch_record_t patch = {0};
  if (!read_ready_source(cache, source, saved, &patch))
    return false;
  char parent[MAX_PATH];
  strlcpy(parent, source, sizeof(parent));
  char *name = strrchr(parent, '/');
  if (!name || name == parent) {
    errno = EINVAL;
    return false;
  }
  *name = '\0';
  name++;
  int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (parent_fd < 0)
    return false;
  int fd = openat(parent_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  bool ready = fd >= 0 && make_launch_source_readable(cache, source, fd, saved, &patch);
  int error = ready ? 0 : errno;
  struct stat st;
  if (ready && fstat(parent_fd, &st) == 0)
    add_source_access(parent_fd, st.st_mode, 0555, parent);
  if (fd >= 0)
    close(fd);
  close(parent_fd);
  errno = error;
  return ready;
}

static bool same_package_after_copy(const char *cache, const sm_pkg_info_t *base,
                                     const sm_kraken_job_t *job) {
  package_id_t previous, current;
  if (!read_package_id(cache, previous))
    return false;
  return capture_package_id(base, job, current) && !memcmp(previous, current, sizeof(previous));
}

static bool write_image_pointer(const char *cache, const char *name) {
  char path[MAX_PATH], temporary[MAX_PATH];
  FILE *file = join_path(cache, "kraken.image", path) ? open_cache_temp(path, temporary) : NULL;
  if (!file)
    return false;
  // The pointer is read by the game's runtime.
  bool written = fchmod(fileno(file), 0644) == 0 &&
      fprintf(file, SM_KRAKEN_PACKAGE_DIR "/%s", name) >= 0;
  return finish_cache_temp(file, temporary, path, written);
}

static bool rebind_cache(const sm_pkg_info_t *base, const sm_kraken_job_t *job,
                          const char *cache, const char *existing,
                          const char *signature, const patch_record_t *patch) {
  int fd = open_source(base, PKG_KIND_BASE);
  if (fd < 0)
    return false;
  int error = 0;
  const char *name = strrchr(base->path, '/') + 1;
  const char *old_name = strrchr(existing, '/') + 1;
  bool renamed = strcmp(name, old_name) != 0;
  if (cancelled(NULL) || !job->finalize(job->ctx))
    error = ECANCELED;
  else if (sm_shellcore_title_is_protected(base->title_id))
    error = EBUSY;
  runtime_mount_state_lock();
  if (error == 0 && is_data_mounted(base->title_id) &&
      !unmount_title_runtime_layers_quiet(base->title_id))
    error = errno ? errno : EBUSY;
  if (error == 0 && !package_unchanged(base, fd))
    error = EAGAIN;
  if (error == 0 && renamed && !write_image_pointer(cache, name))
    error = errno ? errno : EIO;
  else if (error == 0 && !write_cache_marker(cache, base->path, signature, patch)) {
    error = errno ? errno : EIO;
    if (renamed && !write_image_pointer(cache, old_name))
      log_debug("  [KRAKEN] could not restore image pointer for %s", base->title_id);
  }
  runtime_mount_state_unlock();
  close(fd);
  errno = error;
  return error == 0;
}

bool sm_kraken_relink_base(const sm_pkg_info_t *base, const sm_kraken_job_t *job,
                           char cache[MAX_PATH], bool keep_patch) {
  char existing[MAX_PATH], saved[280], updated[280], index[MAX_PATH];
  patch_record_t old_patch = {0};
  if (!read_cache_marker(cache, existing, saved, sizeof(saved), &old_patch) ||
      !relink_candidate(cache, existing, base)) {
    errno = EEXIST;
    return false;
  }
  bool patched = old_patch.path[0] != '\0';
  const char *reader = signature_reader(saved, KRAKEN_REVISION);
  uint64_t backport;
  if ((!patched || keep_patch) && reader && cached_backport_fingerprint(saved, &backport) &&
      join_path(cache, "ampr_assets.index", index) && path_exists(index) &&
      directory_has_game_executable(cache) &&
      (same_package_after_rename(base, saved) ||
       (strcmp(existing, base->path) != 0 && same_package_after_copy(cache, base, job))) &&
      write_reader_signature(base, KRAKEN_REVISION, reader, backport, updated))
    return rebind_cache(base, job, cache, existing, updated, patched ? &old_patch : NULL);

  if (!keep_patch || !patched)
    return sm_kraken_prepare_base(base, job, cache);

  // Rebuild ranges from the replacement PKG before publishing a new binding.
  // Apply only the exact recorded LIH when recovering an existing update.
  sm_pkg_info_t patch;
  if (!sm_pkg_parse_file(old_patch.path, NULL, &patch) ||
      !sm_kraken_can_apply_patch(&patch) ||
      (!matches_source_signature(old_patch.signature, &patch, KRAKEN_PATCH_REVISION) &&
       !matches_source_signature(old_patch.signature, &patch, KRAKEN_PATCH_FAST_REVISION))) {
    errno = ENOENT;
    return false;
  }
  return sm_kraken_apply_patch(base, &patch, job);
}
