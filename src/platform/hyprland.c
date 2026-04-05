#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "platform/hyprland.h"

#include "core/bongocat.h"
#include "platform/wayland.h"
#include "utils/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

// =============================================================================
// HYPRLAND HELPER FUNCTIONS
// =============================================================================

ssize_t safe_exec_read(const char *const argv[], char *buf, size_t buf_size) {
  if (!argv || !argv[0] || !buf || buf_size == 0)
    return -1;

  int pipefd[2];
  if (pipe(pipefd) < 0)
    return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    // Child: redirect stdout to pipe, suppress stderr
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }

  // Parent: read output from pipe
  close(pipefd[1]);
  size_t total = 0;
  while (total < buf_size - 1) {
    ssize_t n = read(pipefd[0], buf + total, buf_size - 1 - total);
    if (n <= 0)
      break;
    total += (size_t)n;
  }
  buf[total] = '\0';
  close(pipefd[0]);

  int status;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return -1;

  return (ssize_t)total;
}

void hypr_update_outputs_with_monitor_ids(void) {
  char buf[8192];
  const char *argv[] = {"hyprctl", "monitors", NULL};
  if (safe_exec_read(argv, buf, sizeof(buf)) < 0)
    return;

  char *saveptr = NULL;
  char *line = strtok_r(buf, "\n", &saveptr);
  while (line) {
    int id = -1;
    char name[256];
    int result = sscanf(line, "Monitor %d \"%255[^\"]\"", &id, name);
    if (result < 2) {
      result = sscanf(line, "Monitor %255s (ID %d)", name, &id);
    }
    if (result == 2) {
      for (size_t i = 0; i < output_count; i++) {
        if (outputs[i].name_received &&
            strcmp(outputs[i].name_str, name) == 0) {
          outputs[i].hypr_id = id;
          bongocat_log_debug("Mapped xdg-output '%s' to Hyprland ID %d\n", name,
                             id);
          break;
        }
      }
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }
}

bool hypr_get_active_window(window_info_t *win) {
  char buf[4096];
  const char *argv[] = {"hyprctl", "activewindow", NULL};
  if (safe_exec_read(argv, buf, sizeof(buf)) < 0)
    return false;

  bool has_window = false;
  win->monitor_id = -1;
  win->fullscreen = false;

  char *saveptr = NULL;
  char *line = strtok_r(buf, "\n", &saveptr);
  while (line) {
    if (strstr(line, "monitor:")) {
      sscanf(line, "%*[\t ]monitor: %d", &win->monitor_id);
      has_window = true;
    }
    if (strstr(line, "fullscreen:")) {
      int val;
      if (sscanf(line, "%*[\t ]fullscreen: %d", &val) == 1) {
        win->fullscreen = (val != 0);
      }
    }
    if (strstr(line, "at:")) {
      if (sscanf(line, "%*[\t ]at: [%d, %d]", &win->x, &win->y) < 2) {
        sscanf(line, "%*[\t ]at: %d,%d", &win->x, &win->y);
      }
    }
    if (strstr(line, "size:")) {
      if (sscanf(line, "%*[\t ]size: [%d, %d]", &win->width, &win->height) <
          2) {
        sscanf(line, "%*[\t ]size: %d,%d", &win->width, &win->height);
      }
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  return has_window;
}
