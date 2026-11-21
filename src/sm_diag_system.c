// GET /api/v1/system: what this console and this UniMount run can do, with the
// reason for every missing capability. Measured values come from the same
// calls the daemon uses; inferred values (kstuff identification) are labelled.

#include "sm_platform.h"

#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <json-c/json.h>

#include "sm_config_mount.h"
#include "sm_diag.h"
#include "sm_diag_core.h"
#include "sm_filesystem.h"
#include "sm_install.h"
#include "sm_kraken.h"
#include "sm_kstuff.h"
#include "sm_mount_defs.h"
#include "sm_mount_device.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_offsets.h"
#include "sm_types.h"

// Source: PS5 Payload Manager include/pldmgr.h (AUTOLOAD_CONFIG_PATH,
// PAYLOADS_STORAGE_DIR) and src/slot.c (<slot>/meta.json).
#define PLDMGR_AUTOLOAD_FILE "/data/pldmgr/autoload.txt"
#define PLDMGR_PAYLOADS_DIR "/data/pldmgr/payloads"
#define SYSTEM_FILE_MAX_BYTES (256u * 1024u)
#define SYSTEM_MAX_KSTUFF_PROCESSES 4u

typedef struct {
  struct json_object *items;
} limitations_t;

static void add_limitation(limitations_t *list, const char *code,
                           const char *severity, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void add_limitation(limitations_t *list, const char *code,
                           const char *severity, const char *fmt, ...) {
  char message[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  struct json_object *item = json_object_new_object();
  if (!item)
    return;
  json_object_object_add(item, "code", json_object_new_string(code));
  json_object_object_add(item, "severity", json_object_new_string(severity));
  json_object_object_add(item, "message", json_object_new_string(message));
  json_object_array_add(list->items, item);
}

static char *read_text_file(const char *path, size_t *size_out) {
  *size_out = 0;
  int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size > SYSTEM_FILE_MAX_BYTES) {
    close(fd);
    return NULL;
  }
  char *data = malloc((size_t)st.st_size + 1u);
  if (!data) {
    close(fd);
    return NULL;
  }
  size_t done = 0;
  while (done < (size_t)st.st_size) {
    ssize_t count = read(fd, data + done, (size_t)st.st_size - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    done += (size_t)count;
  }
  close(fd);
  data[done] = '\0';
  *size_out = done;
  return data;
}

// --- Firmware / ShellCore ---

static struct json_object *firmware_json(uint32_t raw) {
  struct json_object *object = json_object_new_object();
  if (!object)
    return NULL;
  char display[8];
  char hex[11];
  sm_diag_format_fw_bcd((raw >> 16) & 0xffffu, display);
  snprintf(hex, sizeof(hex), "0x%08X", raw);
  json_object_object_add(object, "display", json_object_new_string(display));
  json_object_object_add(object, "raw", json_object_new_int64(raw));
  json_object_object_add(object, "raw_hex", json_object_new_string(hex));
  return object;
}

typedef struct {
  const char *path;
  int available; // 1, 0, -1 (not probed)
  char reason[160];
} registration_t;

// Source: sm_install.c sm_install_register_title_dir() and
// process_scan_candidates() (app_install_all batch mode).
static registration_t registration_path(const runtime_config_t *cfg,
                                        bool hooks_known,
                                        const sm_shellcore_hooks_info_t *hooks) {
  registration_t result = {.available = -1};
  if (cfg->app_install_all_enabled) {
    result.path = "batch_install_all";
    snprintf(result.reason, sizeof(result.reason),
             "[library] install_all is on: new titles are queued for one "
             "sceAppInstUtilAppInstallAll batch (not probed)");
  } else if (sm_shellcore_install_bridge_enabled()) {
    result.path = "shellcore_bridge";
    if (!hooks_known) {
      snprintf(result.reason, sizeof(result.reason),
               "hook state is busy (install or repair in progress)");
    } else if (hooks->installed && hooks->install_hook) {
      result.available = 1;
      snprintf(result.reason, sizeof(result.reason),
               "AppInstallAll bridge installed in SceShellCore");
    } else {
      result.available = 0;
      snprintf(result.reason, sizeof(result.reason),
               "firmware 12.00+ registers through the SceShellCore "
               "AppInstallAll bridge, which is not installed (hooks %s)",
               hooks->state);
    }
  } else {
    result.path = "direct_appinstutil";
    result.available = sm_install_direct_registration_available() ? 1 : 0;
    snprintf(result.reason, sizeof(result.reason), "%s",
             result.available ? "sceAppInstUtilAppInstallTitleDir resolved"
                              : "sceAppInstUtilAppInstallTitleDir could not "
                                "be resolved from libSceAppInstUtil.sprx");
  }
  return result;
}

static struct json_object *tri_json(int value) {
  return value < 0 ? NULL : json_object_new_boolean(value != 0);
}

// --- kstuff identification ---

static void find_kstuff_processes(struct json_object *processes,
                                  char first_name[32]) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  first_name[0] = '\0';
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || size == 0)
    return;
  size += size / 8u + 4096u;
  uint8_t *buffer = malloc(size);
  if (!buffer)
    return;
  if (sysctl(mib, 4, buffer, &size, NULL, 0) != 0) {
    free(buffer);
    return;
  }
  // Source: PS5 Payload Manager include/process_records.h: ki_comm is at
  // offset 447 (20 bytes) in this SDK's kinfo_proc, ki_pid at 72.
  const size_t comm_offset = offsetof(struct kinfo_proc, ki_comm);
  const size_t comm_size = sizeof(((struct kinfo_proc *)0)->ki_comm);
  size_t pos = 0;
  size_t found = 0;
  while (size - pos >= sizeof(int) && found < SYSTEM_MAX_KSTUFF_PROCESSES) {
    int record_size = 0;
    memcpy(&record_size, buffer + pos, sizeof(record_size));
    if (record_size <= 0 || (size_t)record_size < comm_offset + comm_size ||
        (size_t)record_size > size - pos)
      break;
    pid_t pid = 0;
    char name[32] = "";
    memcpy(&pid, buffer + pos + offsetof(struct kinfo_proc, ki_pid), sizeof(pid));
    memcpy(name, buffer + pos + comm_offset,
           comm_size < sizeof(name) - 1u ? comm_size : sizeof(name) - 1u);
    name[sizeof(name) - 1u] = '\0';
    pos += (size_t)record_size;
    if (strncasecmp(name, "kstuff", 6) != 0)
      continue;
    struct json_object *item = json_object_new_object();
    if (!item)
      continue;
    json_object_object_add(item, "pid", json_object_new_int64(pid));
    json_object_object_add(item, "name", json_object_new_string(name));
    json_object_array_add(processes, item);
    if (!first_name[0])
      (void)strlcpy(first_name, name, 32);
    found++;
  }
  free(buffer);
}

static struct json_object *kstuff_json(bool loaded, const char **implementation_out) {
  struct json_object *object = json_object_new_object();
  struct json_object *processes = json_object_new_array();
  struct json_object *evidence = json_object_new_object();
  if (!object || !processes || !evidence) {
    if (object)
      json_object_put(object);
    if (processes)
      json_object_put(processes);
    if (evidence)
      json_object_put(evidence);
    return NULL;
  }
  char process_name[32];
  find_kstuff_processes(processes, process_name);

  sm_diag_pldmgr_kstuff_t pldmgr;
  bool have_entry = false;
  bool have_meta = false;
  char meta_path[MAX_PATH] = "";
  size_t size = 0;
  char *autoload = read_text_file(PLDMGR_AUTOLOAD_FILE, &size);
  if (autoload) {
    have_entry = sm_diag_pldmgr_find_kstuff(autoload, size, &pldmgr);
    free(autoload);
  }
  if (have_entry && pldmgr.slot_key[0]) {
    snprintf(meta_path, sizeof(meta_path), "%s/%s/meta.json", PLDMGR_PAYLOADS_DIR,
             pldmgr.slot_key);
    char *meta = read_text_file(meta_path, &size);
    if (meta) {
      have_meta = sm_diag_pldmgr_parse_meta(meta, size, &pldmgr);
      free(meta);
    }
  }

  const char *implementation = "unknown";
  if (have_entry)
    implementation = pldmgr.implementation;
  if (strcmp(implementation, "unknown") == 0 && process_name[0])
    implementation = sm_diag_kstuff_implementation(process_name);
  // kstuff has no version query; only PLDMGR's slot metadata names one.
  bool have_version = have_meta && pldmgr.version[0];

  json_object_object_add(object, "loaded", json_object_new_boolean(loaded));
  json_object_object_add(object, "loaded_source", json_object_new_string("KEKCALL_CHECK"));
  json_object_object_add(object, "inferred", json_object_new_boolean(true));
  json_object_object_add(object, "implementation", json_object_new_string(implementation));
  json_object_object_add(object, "version",
                         have_version ? json_object_new_string(pldmgr.version) : NULL);
  json_object_object_add(object, "version_source",
                         have_version ? json_object_new_string("pldmgr_meta") : NULL);
  json_object_object_add(evidence, "processes", processes);
  json_object_object_add(evidence, "pldmgr_autoload_file",
                         json_object_new_string(PLDMGR_AUTOLOAD_FILE));
  json_object_object_add(evidence, "pldmgr_autoload_entry",
                         have_entry ? json_object_new_string(pldmgr.entry) : NULL);
  json_object_object_add(evidence, "pldmgr_slot_meta",
                         have_meta ? json_object_new_string(meta_path) : NULL);
  json_object_object_add(evidence, "pldmgr_payload_file",
                         have_meta && pldmgr.filename[0]
                             ? json_object_new_string(pldmgr.filename) : NULL);
  json_object_object_add(evidence, "note",
                         json_object_new_string(
                             "An autoload entry shows what PS5 Payload Manager "
                             "is configured to load, not what is running now."));
  json_object_object_add(object, "evidence", evidence);
  // Static string: implementation may point into the local pldmgr record.
  *implementation_out = sm_diag_kstuff_implementation(implementation);
  return object;
}

// --- Mechanisms ---

typedef struct {
  bool known;
  char names[32][16];
  size_t count;
  int error;
} vfs_list_t;

// Source: FreeBSD getvfsbyname(3) reads the "vfs.conflist" sysctl, an array
// of struct xvfsconf.
static void read_vfs_list(vfs_list_t *list) {
  memset(list, 0, sizeof(*list));
  size_t size = 0;
  if (sysctlbyname("vfs.conflist", NULL, &size, NULL, 0) != 0) {
    list->error = errno;
    return;
  }
  if (size == 0 || size % sizeof(struct xvfsconf) != 0) {
    list->error = EPROTO;
    return;
  }
  struct xvfsconf *entries = malloc(size);
  if (!entries) {
    list->error = ENOMEM;
    return;
  }
  if (sysctlbyname("vfs.conflist", entries, &size, NULL, 0) != 0 ||
      size % sizeof(struct xvfsconf) != 0) {
    list->error = errno ? errno : EPROTO;
    free(entries);
    return;
  }
  size_t count = size / sizeof(struct xvfsconf);
  for (size_t i = 0; i < count && list->count < 32u; ++i) {
    char name[16];
    memcpy(name, entries[i].vfc_name, sizeof(name) - 1u);
    name[sizeof(name) - 1u] = '\0';
    (void)strlcpy(list->names[list->count++], name, sizeof(list->names[0]));
  }
  free(entries);
  list->known = true;
}

static int vfs_registered(const vfs_list_t *list, const char *name) {
  if (!list->known)
    return -1;
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->names[i], name) == 0)
      return 1;
  }
  return 0;
}

static size_t count_mounts(const struct statfs *mounts, int count,
                           const char *fstype) {
  size_t total = 0;
  for (int i = 0; mounts && i < count; ++i) {
    if (strcmp(mounts[i].f_fstypename, fstype) == 0)
      total++;
  }
  return total;
}

static struct json_object *mechanism(const char *name, int available,
                                     const char *reason) {
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;
  json_object_object_add(item, "name", json_object_new_string(name));
  json_object_object_add(item, "available", tri_json(available));
  json_object_object_add(item, "reason", reason ? json_object_new_string(reason) : NULL);
  return item;
}

static int probe_control_device(const char *path, char reason[160]) {
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    snprintf(reason, 160, "open(%s) failed: %s", path, strerror(errno));
    return 0;
  }
  close(fd);
  snprintf(reason, 160, "%s opened read/write", path);
  return 1;
}

static void vfs_reason(const vfs_list_t *vfs, const char *name, int registered,
                       size_t mounted, char reason[160]) {
  if (registered < 0)
    snprintf(reason, 160, "vfs.conflist unreadable (%s); %zu %s mount(s) present",
             strerror(vfs->error), mounted, name);
  else if (registered)
    snprintf(reason, 160, "%s registered in the kernel; %zu mount(s) present",
             name, mounted);
  else
    snprintf(reason, 160, "%s is not registered in the kernel", name);
}

struct json_object *sm_diag_system_report(void) {
  struct json_object *report = json_object_new_object();
  struct json_object *mechanisms = json_object_new_array();
  limitations_t limitations = {.items = json_object_new_array()};
  if (!report || !mechanisms || !limitations.items) {
    if (report)
      json_object_put(report);
    if (mechanisms)
      json_object_put(mechanisms);
    if (limitations.items)
      json_object_put(limitations.items);
    return NULL;
  }
  const runtime_config_t cfg = runtime_config();
  const uint32_t raw_fw = kernel_get_fw_version();
  const uint32_t fw_bcd = (raw_fw >> 16) & 0xffffu;
  char fw_display[8];
  sm_diag_format_fw_bcd(fw_bcd, fw_display);
  json_object_object_add(report, "firmware", firmware_json(raw_fw));

  // ShellCore.
  const sm_shellcore_firmware_offsets_t *offsets =
      sm_shellcore_offsets_for_firmware(raw_fw);
  sm_shellcore_hooks_info_t hooks = {.state = "unknown"};
  bool hooks_known = sm_shellcore_hooks_get_info(&hooks);
  registration_t registration = registration_path(&cfg, hooks_known, &hooks);
  struct json_object *shellcore = json_object_new_object();
  if (shellcore) {
    struct json_object *hook_json = json_object_new_object();
    if (hook_json) {
      json_object_object_add(hook_json, "installed",
                             hooks_known ? json_object_new_boolean(hooks.installed) : NULL);
      json_object_object_add(hook_json, "state", json_object_new_string(
                                                     hooks_known ? hooks.state : "busy"));
      json_object_object_add(hook_json, "hook_count",
                             json_object_new_int64((int64_t)hooks.hook_count));
      json_object_object_add(hook_json, "install_hook",
                             json_object_new_boolean(hooks.install_hook));
    }
    struct json_object *reg = json_object_new_object();
    if (reg) {
      json_object_object_add(reg, "path", json_object_new_string(registration.path));
      json_object_object_add(reg, "available", tri_json(registration.available));
      json_object_object_add(reg, "reason", json_object_new_string(registration.reason));
    }
    json_object_object_add(shellcore, "offsets_available",
                           json_object_new_boolean(offsets != NULL));
    json_object_object_add(shellcore, "offsets_firmware",
                           offsets ? json_object_new_string(offsets->name) : NULL);
    json_object_object_add(shellcore, "install_bridge_firmware",
                           json_object_new_boolean(sm_shellcore_install_bridge_enabled()));
    json_object_object_add(shellcore, "hooks", hook_json);
    json_object_object_add(shellcore, "registration", reg);
  }
  json_object_object_add(report, "shellcore", shellcore);

  // kstuff.
  bool kstuff_loaded = sm_kstuff_is_loaded();
  const char *implementation = "unknown";
  json_object_object_add(report, "kstuff", kstuff_json(kstuff_loaded, &implementation));

  // Mechanisms.
  struct statfs *mounts = NULL;
  int mount_count = sm_mount_table_snapshot(&mounts);
  vfs_list_t vfs;
  read_vfs_list(&vfs);
  char reason[160];

  int lvd = probe_control_device(LVD_CTRL_PATH, reason);
  json_object_array_add(mechanisms, mechanism("lvd", lvd, reason));
  int md = probe_control_device(MD_CTRL_PATH, reason);
  json_object_array_add(mechanisms, mechanism("md", md, reason));

  int nullfs = vfs_registered(&vfs, "nullfs");
  size_t nullfs_mounts = count_mounts(mounts, mount_count, "nullfs");
  if (nullfs < 0 && nullfs_mounts > 0)
    nullfs = 1; // An existing nullfs mount proves the filesystem works.
  vfs_reason(&vfs, "nullfs", vfs_registered(&vfs, "nullfs"), nullfs_mounts, reason);
  json_object_array_add(mechanisms, mechanism("nullfs", nullfs, reason));

  int unionfs = vfs_registered(&vfs, "unionfs");
  size_t unionfs_mounts = count_mounts(mounts, mount_count, "unionfs");
  if (unionfs < 0 && unionfs_mounts > 0)
    unionfs = 1;
  vfs_reason(&vfs, "unionfs", vfs_registered(&vfs, "unionfs"), unionfs_mounts, reason);
  json_object_array_add(mechanisms, mechanism("unionfs", unionfs, reason));

  // Source: sm_image.c mounts PFS images with nmount("pfs") over an LVD unit.
  int pfs_fs = vfs_registered(&vfs, "pfs");
  int pfs = lvd == 0 || pfs_fs == 0 ? 0 : (pfs_fs < 0 ? -1 : 1);
  if (lvd == 0)
    snprintf(reason, sizeof(reason), "PFS images attach through LVD, which is unavailable");
  else
    vfs_reason(&vfs, "pfs", pfs_fs, count_mounts(mounts, mount_count, "pfs"), reason);
  json_object_array_add(mechanisms, mechanism("pfs_images", pfs, reason));

  // Kraken direct .pkg mount.
  char shared_path[MAX_PATH];
  const unsigned char *runtime = NULL;
  size_t runtime_size = 0;
  unsigned char *owned = NULL;
  bool shared = sm_kraken_runtime_selection(
      cfg.update_emulators_enabled ? cfg.emulators_path : NULL, shared_path,
      &runtime, &runtime_size, &owned);
  char ampr_version[32] = "";
  bool have_ampr_version = sm_diag_ampr_version(runtime, runtime_size, ampr_version);
  free(owned);
  struct json_object *kraken = mechanism(
      "kraken_pkg", nullfs == 0 ? 0 : (nullfs < 0 ? -1 : 1),
      nullfs == 0 ? "Kraken caches are mounted with nullfs, which is unavailable"
                  : "Kraken Mount bridge is linked into this build");
  if (kraken) {
    json_object_object_add(kraken, "bridge_revision",
                           json_object_new_string(sm_kraken_bundled_revision()));
    json_object_object_add(kraken, "ampr_runtime",
                           json_object_new_string(shared ? "shared" : "bundled"));
    json_object_object_add(kraken, "ampr_runtime_path",
                           shared ? json_object_new_string(shared_path) : NULL);
    json_object_object_add(kraken, "ampr_runtime_version",
                           have_ampr_version ? json_object_new_string(ampr_version) : NULL);
    json_object_array_add(mechanisms, kraken);
  }

  struct json_object *reg_mechanism =
      mechanism("per_title_registration", registration.available, registration.reason);
  if (reg_mechanism) {
    json_object_object_add(reg_mechanism, "path",
                           json_object_new_string(registration.path));
    json_object_array_add(mechanisms, reg_mechanism);
  }
  free(mounts);
  json_object_object_add(report, "mechanisms", mechanisms);

  // Limitations derived from the measurements above.
  if (!offsets) {
    if (fw_bcd >= 0x1200u)
      add_limitation(&limitations, "shellcore_offsets_missing", "error",
                     "No SceShellCore offsets for firmware %s: launch hooks and "
                     "per-title registration (firmware 12.00+) are unavailable.",
                     fw_display);
    else
      add_limitation(&limitations, "shellcore_offsets_missing", "warning",
                     "No SceShellCore offsets for firmware %s: launch-time "
                     "mounting and launch observation are unavailable.",
                     fw_display);
  } else if (hooks_known && !hooks.installed) {
    add_limitation(&limitations, "shellcore_hooks_not_installed", "error",
                   "SceShellCore hooks are not installed (state %s): titles are "
                   "not mounted on launch and launches are not observed.",
                   hooks.state);
  }
  if (!kstuff_loaded)
    add_limitation(&limitations, "kstuff_not_loaded", "error",
                   "kstuff did not answer KEKCALL_CHECK: kernel patches needed "
                   "for mounting and game launches are missing.");
  if (registration.available == 0)
    add_limitation(&limitations, "registration_unavailable", "error",
                   "Per-title registration (%s) is unavailable: %s",
                   registration.path, registration.reason);
  // Source: EchoStretch/kstuff-lite ps5-kstuff/uelf/fpkg.c:
  // "if(FWVER > 0x1160) return NULL;" - plaintext package interception is
  // not armed above 11.60.
  if (fw_bcd > 0x1160u && strcmp(implementation, "kstuff") != 0)
    add_limitation(&limitations, "native_debug_pkg_unavailable", "info",
                   "Native debug .pkg installation is not available on firmware "
                   "%s: kstuff-lite does not arm plaintext package interception "
                   "above 11.60. UniMount mounts debug .pkg files through the "
                   "Kraken backend instead.",
                   fw_display);
  if (lvd == 0)
    add_limitation(&limitations, "lvd_unavailable",
                   cfg.exfat_backend == ATTACH_BACKEND_LVD ||
                           cfg.ufs_backend == ATTACH_BACKEND_LVD
                       ? "error"
                       : "warning",
                   "LVD (%s) is unavailable: images using the LVD backend and "
                   "PFS images cannot be mounted.",
                   LVD_CTRL_PATH);
  if (md == 0)
    add_limitation(&limitations, "md_unavailable",
                   cfg.exfat_backend == ATTACH_BACKEND_MD ||
                           cfg.ufs_backend == ATTACH_BACKEND_MD
                       ? "error"
                       : "info",
                   "MD (%s) is unavailable: images configured for the MD "
                   "backend cannot be mounted.",
                   MD_CTRL_PATH);
  if (nullfs == 0)
    add_limitation(&limitations, "nullfs_unavailable", "error",
                   "nullfs is unavailable: titles cannot be mounted into "
                   "/system_ex/app.");
  if (unionfs == 0)
    add_limitation(&limitations, "unionfs_unavailable", "warning",
                   "unionfs is unavailable: fakelib overlays cannot be mounted.");
  json_object_object_add(report, "limitations", limitations.items);
  return report;
}
