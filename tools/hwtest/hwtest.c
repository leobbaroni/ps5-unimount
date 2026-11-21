/*
 * unimount-hwtest: a tiny test-only payload that lets a PC on the LAN launch
 * and close titles on the console, so hardware tests do not need a person at
 * the controller. It is NOT part of UniMount and must not be autoloaded.
 *
 * Line protocol on TCP port 10190 (one command per connection):
 *   status            -> "ok bigapp=0x%08x"   (0xffffffff when none is running)
 *   launch <TITLEID>  -> "ok app_id=0x%08x" | "err busy bigapp=0x..." | "err rc=0x%08x"
 *   kill              -> "ok" | "err rc=0x%08x" | "err none"
 *   quit              -> "ok bye" and the payload exits
 *
 * Launch/kill calls follow ps5-payload-dev/websrv (sys_launch_title), GPL-3.0.
 * No authentication: load it only for a test session and send "quit" after.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define HWTEST_PORT 10190

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, app_launch_ctx_t *ctx);

static int valid_title_id(const char *s) {
  if (strlen(s) != 9)
    return 0;
  for (int i = 0; i < 9; i++) {
    if (i < 4 ? !isupper((unsigned char)s[i]) : !isdigit((unsigned char)s[i]))
      return 0;
  }
  return 1;
}

static void reply(int fd, const char *fmt, uint32_t value) {
  char buf[96];
  int n = snprintf(buf, sizeof(buf), fmt, value);
  if (n > 0)
    (void)write(fd, buf, (size_t)n);
}

static int handle(int fd) {
  char line[128];
  size_t used = 0;
  while (used < sizeof(line) - 1) {
    ssize_t n = read(fd, line + used, sizeof(line) - 1 - used);
    if (n <= 0)
      break;
    used += (size_t)n;
    if (memchr(line, '\n', used))
      break;
  }
  line[used] = '\0';
  line[strcspn(line, "\r\n")] = '\0';

  if (strcmp(line, "status") == 0) {
    reply(fd, "ok bigapp=0x%08x\n", (uint32_t)sceSystemServiceGetAppIdOfRunningBigApp());
  } else if (strncmp(line, "launch ", 7) == 0) {
    const char *title_id = line + 7;
    int running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (!valid_title_id(title_id)) {
      reply(fd, "err bad title id\n", 0);
    } else if (running > 0) {
      reply(fd, "err busy bigapp=0x%08x\n", (uint32_t)running);
    } else {
      app_launch_ctx_t ctx = {0};
      char *argv[1] = {NULL};
      int rc = sceUserServiceGetForegroundUser(&ctx.user_id);
      if (rc == 0)
        rc = sceSystemServiceLaunchApp(title_id, argv, &ctx);
      reply(fd, rc < 0 ? "err rc=0x%08x\n" : "ok app_id=0x%08x\n", (uint32_t)rc);
    }
  } else if (strcmp(line, "kill") == 0) {
    int running = sceSystemServiceGetAppIdOfRunningBigApp();
    if (running <= 0) {
      reply(fd, "err none\n", 0);
    } else {
      int rc = sceSystemServiceKillApp(running, -1, 0, 0);
      reply(fd, rc ? "err rc=0x%08x\n" : "ok\n", (uint32_t)rc);
    }
  } else if (strcmp(line, "quit") == 0) {
    reply(fd, "ok bye\n", 0);
    return 1;
  } else {
    reply(fd, "err unknown command\n", 0);
  }
  return 0;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  syscall(SYS_thr_set_name, -1, "unimount-hwtest.elf");
  sceUserServiceInitialize(NULL);

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    return 1;
  int on = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(HWTEST_PORT);
  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 4) != 0)
    return 2;

  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0)
      continue;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int quit = handle(fd);
    close(fd);
    if (quit)
      break;
  }
  close(srv);
  return 0;
}
