#include "sm_platform.h"

#include "sm_appdb.h"
#include "sm_filesystem.h"
#include "sm_install.h"
#include "sm_log.h"
#include "sm_paths.h"
#include "sm_shell_icon.h"
#include "sm_unimount_logic.h"

#define SM_SHELL_ICON_TITLE_ID "UNMT00001"
#define SM_SHELL_ICON_DIR APP_BASE "/" SM_SHELL_ICON_TITLE_ID
#define SM_SHELL_ICON_SCE_SYS_DIR SM_SHELL_ICON_DIR "/sce_sys"
// Largest param.json read when verifying a legacy tile.
#define SM_LEGACY_TILE_PARAM_MAX (8u * 1024u)

// Web-shortcut tiles of ShadowMountPlus and Kraken Mount.
static const char *const k_legacy_tiles[] = {"FAKE10101", "KRKN00001"};

extern unsigned char assets_unimount_icon_png[];
extern unsigned int assets_unimount_icon_png_len;
extern unsigned char assets_shell_icon_param_json[];
extern unsigned int assets_shell_icon_param_json_len;

static bool ensure_directory(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      return true;
    errno = ENOTDIR;
    return false;
  }
  if (errno != ENOENT)
    return false;
  return mkdir(path, 0755) == 0;
}

static bool install_asset(const char *path, const unsigned char *data,
                          size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;

  size_t offset = 0;
  while (offset < size) {
    ssize_t written = write(fd, data + offset, size - offset);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (written == 0) {
      errno = EIO;
      break;
    }
    offset += (size_t)written;
  }

  int saved_errno = offset == size ? 0 : errno;
  if (close(fd) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (saved_errno == 0)
    return true;

  (void)unlink(path);
  errno = saved_errno;
  return false;
}

// True when dir/sce_sys/param.json is a small local web shortcut of title_id;
// otherwise *reason says why.
static bool legacy_tile_verified(const char *dir, const char *title_id,
                                 char *reason, size_t reason_size) {
  char path[MAX_PATH];
  struct stat st;
  int written = snprintf(path, sizeof(path), "%s/sce_sys", dir);
  if (written < 0 || (size_t)written >= sizeof(path)) {
    (void)strlcpy(reason, "path too long", reason_size);
    return false;
  }
  if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || lstat(path, &st) != 0 ||
      !S_ISDIR(st.st_mode)) {
    (void)strlcpy(reason, "no sce_sys directory", reason_size);
    return false;
  }
  (void)strlcat(path, "/param.json", sizeof(path));
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    (void)snprintf(reason, reason_size, "param.json unreadable (%s)",
                   strerror(errno));
    return false;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size > (off_t)SM_LEGACY_TILE_PARAM_MAX) {
    (void)close(fd);
    (void)strlcpy(reason, "param.json is not a small regular file",
                  reason_size);
    return false;
  }
  char buf[SM_LEGACY_TILE_PARAM_MAX];
  size_t length = 0;
  int read_errno = 0;
  while (length < sizeof(buf)) {
    ssize_t received = read(fd, buf + length, sizeof(buf) - length);
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0)
      read_errno = errno;
    if (received <= 0)
      break;
    length += (size_t)received;
  }
  (void)close(fd);
  if (read_errno != 0) {
    (void)snprintf(reason, reason_size, "param.json unreadable (%s)",
                   strerror(read_errno));
    return false;
  }
  if (!sm_legacy_tile_param_matches(buf, length, title_id)) {
    (void)strlcpy(reason, "param.json is not its web shortcut", reason_size);
    return false;
  }
  return true;
}

// Only the files a web-shortcut tile consists of, then the empty directories.
static bool remove_legacy_tile_files(const char *dir) {
  static const char *const files[] = {"sce_sys/param.json",
                                      "sce_sys/icon0.png"};
  char path[MAX_PATH];
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    int written = snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
    if (written > 0 && (size_t)written < sizeof(path))
      (void)unlink(path);
  }
  int written = snprintf(path, sizeof(path), "%s/sce_sys", dir);
  if (written > 0 && (size_t)written < sizeof(path))
    (void)rmdir(path);
  return rmdir(dir) == 0 || errno == ENOENT;
}

static void remove_legacy_tile(const char *title_id) {
  char dir[MAX_PATH];
  struct stat st;
  int written = snprintf(dir, sizeof(dir), "%s/%s", APP_BASE, title_id);
  if (written < 0 || (size_t)written >= sizeof(dir))
    return;
  if (lstat(dir, &st) != 0) {
    if (errno != ENOENT)
      log_debug("  [SHELLICON] legacy tile %s left alone: %s", title_id,
                strerror(errno));
    return;
  }
  char reason[96];
  if (!legacy_tile_verified(dir, title_id, reason, sizeof(reason))) {
    log_debug("  [SHELLICON] legacy tile %s left alone: %s", title_id, reason);
    return;
  }

  int res = sceAppInstUtilAppUnInstall(title_id);
  if (res != 0)
    log_debug("  [SHELLICON] legacy tile uninstall failed: %s code=0x%08X",
              title_id, (uint32_t)res);
  invalidate_app_db_title_cache();
  if (lstat(dir, &st) != 0 && errno == ENOENT) {
    log_debug("  [SHELLICON] removed legacy tile: %s", title_id);
    return;
  }
  // Still there, e.g. the title was not in the console's database.
  if (remove_legacy_tile_files(dir)) {
    log_debug("  [SHELLICON] removed legacy tile: %s (directory removed after "
              "the uninstall call)",
              title_id);
  } else {
    log_debug("  [SHELLICON] legacy tile %s: directory kept: %s", title_id,
              strerror(errno));
  }
}

void sm_shell_icon_install_if_missing(void) {
  for (size_t i = 0; i < sizeof(k_legacy_tiles) / sizeof(k_legacy_tiles[0]);
       ++i)
    remove_legacy_tile(k_legacy_tiles[i]);

  if (is_installed(SM_SHELL_ICON_TITLE_ID)) {
    log_debug("  [SHELLICON] already installed: %s",
              SM_SHELL_ICON_TITLE_ID);
    return;
  }

  if (!ensure_directory(SM_SHELL_ICON_DIR) ||
      !ensure_directory(SM_SHELL_ICON_SCE_SYS_DIR)) {
    log_debug("  [SHELLICON] failed to create app directory: %s",
              strerror(errno));
    return;
  }

  if (!install_asset(SM_SHELL_ICON_SCE_SYS_DIR "/icon0.png",
                     assets_unimount_icon_png,
                     (size_t)assets_unimount_icon_png_len)) {
    log_debug("  [SHELLICON] failed to stage icon0.png: %s",
              strerror(errno));
    return;
  }
  if (!install_asset(SM_SHELL_ICON_SCE_SYS_DIR "/param.json",
                     assets_shell_icon_param_json,
                     (size_t)assets_shell_icon_param_json_len)) {
    log_debug("  [SHELLICON] failed to stage param.json: %s",
              strerror(errno));
    return;
  }

  int result = 0;
  if (!sm_install_register_title_dir(SM_SHELL_ICON_TITLE_ID, APP_BASE "/",
                                     &result)) {
    log_debug("  [SHELLICON] AppInstallTitleDir unavailable");
    return;
  }

  if (result != 0 && (uint32_t)result != 0x80990002u) {
    log_debug("  [SHELLICON] registration failed: code=0x%08X",
              (uint32_t)result);
    return;
  }

  invalidate_app_db_title_cache();
  log_debug("  [SHELLICON] installed: %s", SM_SHELL_ICON_TITLE_ID);
}
