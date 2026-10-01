#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "platform/hyprland.h"

#include "core/bongocat.h"
#include "platform/outputs.h"
#include "platform/wayland.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// =============================================================================
// HYPRLAND HELPER FUNCTIONS
// =============================================================================

typedef struct {
  pid_t pid;
  int fd;
  int64_t deadline;
  size_t used;
  bool eof, failed, exited;
  int status;
  char buffer[8192];
} command_job_t;
static command_job_t fallback = {.fd = -1};
static bool querying_monitors = true;
static int64_t next_query;
static window_info_t active_window;
static bool active_valid;
static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}
static void job_cleanup(command_job_t *job) {
  if (job->fd >= 0) {
    close(job->fd);
  }
  if (job->pid > 0 && !job->exited) {
    kill(job->pid, SIGKILL);
    while (waitpid(job->pid, &job->status, 0) < 0 && errno == EINTR) {}
  }
  job->fd = -1;
  job->pid = 0;
}
static int job_start(command_job_t *job, const char *const argv[]) {
  *job = (command_job_t){.fd = -1};
  int pipes[2];
  if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) < 0) {
    return -1;
  }
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (!error) {
    error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (!error) {
      error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                               "/dev/null", O_WRONLY, 0);
    }
    if (!error) {
      error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    }
    if (!error) {
      error = posix_spawnp(&job->pid, argv[0], &actions, NULL,
                           (char *const *)argv, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
  }
  close(pipes[1]);
  if (error) {
    close(pipes[0]);
    job->pid = 0;
    return -1;
  }
  job->fd = pipes[0];
  job->deadline = now_ms() + 1000;
  return 0;
}
// Returns 0 while pending, 1 for complete output, -1 for any failure.
static int job_process(command_job_t *job) {
  char chunk[1024];
  ssize_t count;
  while ((count = read(job->fd, chunk, sizeof(chunk))) > 0) {
    if ((size_t)count > sizeof(job->buffer) - 1 - job->used) {
      job->failed = true;
      break;
    }
    memcpy(job->buffer + job->used, chunk, (size_t)count);
    job->used += (size_t)count;
  }
  if (!count) {
    job->eof = true;
  } else if (count < 0 && errno != EAGAIN && errno != EINTR) {
    job->failed = true;
  }
  if (!job->exited) {
    pid_t result = waitpid(job->pid, &job->status, WNOHANG);
    if (result == job->pid) {
      job->exited = true;
    } else if (result < 0 && errno != EINTR) {
      job->failed = true;
    }
  }
  if (job->failed || now_ms() >= job->deadline) {
    job_cleanup(job);
    return -1;
  }
  if (!job->eof || !job->exited) {
    return 0;
  }
  bool success = WIFEXITED(job->status) && WEXITSTATUS(job->status) == 0;
  job->buffer[job->used] = '\0';
  job_cleanup(job);
  return (int)success ? 1 : -1;
}
ssize_t safe_exec_read(const char *const argv[], char *buf, size_t size) {
  if (!argv || !argv[0] || !buf || !size) {
    return -1;
  }
  command_job_t job;
  if (job_start(&job, argv) < 0) {
    return -1;
  }
  for (;;) {
    int result = job_process(&job);
    if (result) {
      if (result < 0 || job.used >= size) {
        return -1;
      }
      memcpy(buf, job.buffer, job.used + 1);
      return (ssize_t)job.used;
    }
    struct pollfd fd = {.fd = job.fd, .events = POLLIN};
    int polled = poll(&fd, 1, 10);
    if (polled < 0) {
      job_cleanup(&job);
      return -1;
    }
  }
}

static bool parse_number(const char *text, int *value) {
  errno = 0;
  char *end;
  long parsed = strtol(text, &end, 10);
  if (errno || end == text || parsed < INT_MIN || parsed > INT_MAX ||
      (*end && !isspace((unsigned char)*end) && *end != ')' && *end != ':')) {
    return false;
  }
  *value = (int)parsed;
  return true;
}
static void parse_monitors(char *buf) {
  char *save;
  for (char *line = strtok_r(buf, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (strncmp(line, "Monitor ", 8) != 0) {
      continue;
    }
    int id;
    char name[128];
    char *label = line + 8;
    if (isdigit((unsigned char)*label)) {
      if (!parse_number(label, &id)) {
        continue;
      }
      char *quote = strchr(label, '"');
      if (!quote) {
        continue;
      }
      label = quote + 1;
      size_t count = strcspn(label, "\"");
      if (!count || count >= sizeof(name) || label[count] != '"') {
        continue;
      }
      memcpy(name, label, count);
      name[count] = '\0';
    } else {
      char *identifier = strstr(label, "(ID ");
      if (!identifier || !parse_number(identifier + 4, &id)) {
        continue;
      }
      size_t count = strcspn(label, " \t");
      if (!count || count >= sizeof(name)) {
        continue;
      }
      memcpy(name, label, count);
      name[count] = '\0';
    }
    for (size_t i = 0; i < MAX_OUTPUTS; i++) {
      if (outputs[i].wl_output && strcmp(outputs[i].name_str, name) == 0) {
        outputs[i].hypr_id = id;
      }
    }
  }
}
static bool parse_window(char *buf, window_info_t *win) {
  *win = (window_info_t){.monitor_id = -1};
  bool has_monitor = false;
  bool has_state = false;
  char *save;
  for (char *line = strtok_r(buf, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *value = strstr(line, "monitor:");
    if (value) {
      has_monitor = parse_number(value + 8, &win->monitor_id);
    }
    value = strstr(line, "fullscreen:");
    if (value) {
      int state;
      has_state = parse_number(value + 11, &state);
      if (has_state) {
        win->fullscreen = state != 0;
      }
    }
  }
  return (has_monitor && has_state) != 0;
}

void hypr_update_outputs_with_monitor_ids(void) {
  char buf[8192];
  const char *args[] = {"hyprctl", "monitors", NULL};
  if (safe_exec_read(args, buf, sizeof(buf)) >= 0) {
    parse_monitors(buf);
  }
}
bool hypr_get_active_window(window_info_t *window) {
  char buf[8192];
  const char *args[] = {"hyprctl", "activewindow", NULL};
  return (safe_exec_read(args, buf, sizeof(buf)) >= 0 &&
          parse_window(buf, window)) != 0;
}
void hypr_poll(void) {
  if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
    return;
  }
  if (fallback.pid > 0) {
    int result = job_process(&fallback);
    if (!result) {
      return;
    }
    if (result > 0) {
      if (querying_monitors) {
        parse_monitors(fallback.buffer);
      } else {
        active_valid = parse_window(fallback.buffer, &active_window);
      }
    } else {
      { active_valid = false; }
    }
    querying_monitors = ((!querying_monitors) != 0);
    next_query = now_ms() + ((int)querying_monitors ? 1000 : 0);
    wayland_request_redraw();
  }
  if (now_ms() >= next_query) {
    const char *args[] = {
        "hyprctl", (int)querying_monitors ? "monitors" : "activewindow", NULL};
    if (job_start(&fallback, args) < 0) {
      next_query = now_ms() + 1000;
    }
  }
}
bool hypr_fullscreen_for_output(struct wl_output *object) {
  if (!active_valid || !active_window.fullscreen) {
    return false;
  }
  for (size_t i = 0; i < MAX_OUTPUTS; i++) {
    if (outputs[i].wl_output == object) {
      return outputs[i].hypr_id == active_window.monitor_id;
    }
  }
  return false;
}
int hypr_poll_fd(void) {
  return (int)fallback.eof ? -1 : fallback.fd;
}
void hypr_cleanup(void) {
  job_cleanup(&fallback);
  active_valid = false;
}

int hypr_timeout(void) {
  if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
    return -1;
  }
  int64_t deadline = fallback.pid > 0 ? fallback.deadline : next_query;
  int64_t remaining = deadline - now_ms();
  // After stdout EOF, waitpid needs a bounded recheck without a HUP spin.
  if (fallback.pid > 0 && fallback.eof) {
    return 10;
  }
  return remaining > 0 ? (int)remaining : 0;
}
