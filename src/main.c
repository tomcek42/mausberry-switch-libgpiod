/*
 * mausberry-switch
 *
 * GPIO monitoring daemon for use with Mausberry switches on the Raspberry Pi.
 * Uses libgpiod for GPIO access.
 */

#include <errno.h>
#include <gpiod.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

#define CONFIG_PATH SYSCONFDIR "/mausberry-switch.conf"
#define MAX_LINE 256

/* Default configuration */
#define DEFAULT_PIN_OUT 23
#define DEFAULT_PIN_IN 24
#define DEFAULT_DELAY 0
#define DEFAULT_SHUTDOWN_CMD "systemctl poweroff"
#define GPIO_CHIP "gpiochip0"
#define CONSUMER "mausberry-switch"

/* The switch must stay off this long before we act on it (debounce) */
#define DEBOUNCE_MS 100

extern char **environ;

struct config {
  int pin_out;
  int pin_in;
  int delay;
  char shutdown_cmd[MAX_LINE];
};

enum wait_result { WAIT_ERROR, WAIT_TIMEOUT, WAIT_EVENT, WAIT_SIGNAL };

static void trim(char *s) {
  char *end;
  while (*s == ' ' || *s == '\t')
    memmove(s, s + 1, strlen(s));
  end = s + strlen(s) - 1;
  while (end > s &&
         (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t'))
    *end-- = '\0';
}

/* Parse a non-negative integer up to max; keep *out unchanged on error. */
static void parse_int(const char *key, const char *val, int max, int *out) {
  char *end;
  long n;

  errno = 0;
  n = strtol(val, &end, 10);
  if (errno != 0 || end == val || *end != '\0' || n < 0 || n > max) {
    fprintf(stderr, "Invalid value '%s' for %s, using %d.\n", val, key, *out);
    return;
  }
  *out = (int)n;
}

static void load_config(struct config *cfg) {
  FILE *fp;
  char line[MAX_LINE];
  char section[MAX_LINE] = "";

  /* Set defaults */
  cfg->pin_out = DEFAULT_PIN_OUT;
  cfg->pin_in = DEFAULT_PIN_IN;
  cfg->delay = DEFAULT_DELAY;
  snprintf(cfg->shutdown_cmd, MAX_LINE, "%s", DEFAULT_SHUTDOWN_CMD);

  fp = fopen(CONFIG_PATH, "r");
  if (!fp) {
    fprintf(stderr, "Config file '%s' not found, using defaults.\n",
            CONFIG_PATH);
    return;
  }

  while (fgets(line, sizeof(line), fp)) {
    char *p = line;
    trim(p);

    /* Skip empty lines and comments */
    if (*p == '\0' || *p == '#')
      continue;

    /* Section header */
    if (*p == '[') {
      char *end = strchr(p, ']');
      if (end) {
        *end = '\0';
        snprintf(section, MAX_LINE, "%s", p + 1);
      }
      continue;
    }

    /* Key=Value */
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = p;
    char *val = eq + 1;
    trim(key);
    trim(val);

    if (strcmp(section, "Pins") == 0) {
      if (strcmp(key, "Out") == 0)
        parse_int("Out", val, INT_MAX, &cfg->pin_out);
      else if (strcmp(key, "In") == 0)
        parse_int("In", val, INT_MAX, &cfg->pin_in);
    } else if (strcmp(section, "Config") == 0) {
      if (strcmp(key, "ShutdownCommand") == 0 && *val != '\0')
        snprintf(cfg->shutdown_cmd, MAX_LINE, "%s", val);
      else if (strcmp(key, "Delay") == 0)
        parse_int("Delay", val, INT_MAX / 1000, &cfg->delay);
    }
  }

  fclose(fp);
}

static void print_config(const struct config *cfg) {
  printf("\n== Mausberry Switch Configuration ==\n");
  printf("Command:  %s\n", cfg->shutdown_cmd);
  printf("Delay  :  %d\n", cfg->delay);
  printf("Pin OUT:  %d\n", cfg->pin_out);
  printf("Pin IN :  %d\n", cfg->pin_in);
  printf("== End Configuration ==\n\n");
}

/*
 * Sleep in poll() until a GPIO event, a termination signal or the timeout
 * (-1 = none). A negative line_fd waits for signals only. Without a timeout
 * the process does not wake up at all until something happens.
 */
static enum wait_result wait_for(int line_fd, int sig_fd, int timeout_ms) {
  struct pollfd pfds[2] = {{.fd = sig_fd, .events = POLLIN},
                           {.fd = line_fd, .events = POLLIN}};
  int ret;

  do
    ret = poll(pfds, 2, timeout_ms);
  while (ret < 0 && errno == EINTR);

  if (ret < 0) {
    perror("Failed to wait for events");
    return WAIT_ERROR;
  }
  if (ret == 0)
    return WAIT_TIMEOUT;
  if (pfds[0].revents)
    return WAIT_SIGNAL;
  return WAIT_EVENT;
}

/*
 * Run the shutdown command via /bin/sh. posix_spawn() instead of system(),
 * because the child must not inherit our blocked SIGTERM/SIGINT.
 */
static int run_command(const char *cmd) {
  posix_spawnattr_t attr;
  sigset_t empty;
  pid_t pid;
  int status;
  char *argv[] = {"sh", "-c", (char *)cmd, NULL};

  sigemptyset(&empty);
  posix_spawnattr_init(&attr);
  posix_spawnattr_setsigmask(&attr, &empty);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
  errno = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
  posix_spawnattr_destroy(&attr);
  if (errno != 0) {
    perror("Failed to execute shutdown command");
    return -1;
  }

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("Failed to wait for shutdown command");
      return -1;
    }
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "Shutdown command failed (status %d)\n", status);
    return -1;
  }
  return 0;
}

/*
 * Main loop. Acts on the level of the switch line rather than on single
 * edges, so a switch that is already off at startup and edges lost while
 * the queue was full are handled as well.
 */
static int monitor(struct gpiod_line *input_line, int sig_fd,
                   const struct config *cfg) {
  int line_fd = gpiod_line_event_get_fd(input_line);
  int delay_ms = cfg->delay > 0 ? cfg->delay * 1000 : DEBOUNCE_MS;
  struct gpiod_line_event events[16];
  enum wait_result res;
  int value;

  for (;;) {
    value = gpiod_line_get_value(input_line);
    if (value < 0) {
      perror("Failed to read switch state");
      return 1;
    }

    /* High = switch turned off */
    if (value == 1) {
      if (cfg->delay > 0)
        printf("Switch turned off, shutting down in %d seconds\n",
               cfg->delay);

      /* Wait for the delay without watching the line; edges stay queued */
      res = wait_for(-1, sig_fd, delay_ms);
      if (res == WAIT_SIGNAL)
        return 0;
      if (res == WAIT_ERROR)
        return 1;

      value = gpiod_line_get_value(input_line);
      if (value < 0) {
        perror("Failed to read switch state");
        return 1;
      }
      if (value == 1) {
        printf("Executing: %s\n", cfg->shutdown_cmd);
        /* On failure exit non-zero, so systemd restarts us and we retry */
        if (run_command(cfg->shutdown_cmd) < 0)
          return 1;

        /* Keep holding the IN line high until systemd stops us */
        printf("Waiting for system shutdown\n");
        return wait_for(-1, sig_fd, -1) == WAIT_SIGNAL ? 0 : 1;
      }
      if (cfg->delay > 0)
        printf("Switch turned back on, shutdown cancelled\n");
    }

    res = wait_for(line_fd, sig_fd, -1);
    if (res == WAIT_SIGNAL)
      return 0;
    if (res == WAIT_ERROR)
      return 1;

    /* Discard queued edges, only the current level matters */
    if (gpiod_line_event_read_multiple(input_line, events, 16) < 0) {
      perror("Failed to read line events");
      return 1;
    }
  }
}

int main(void) {
  struct gpiod_chip *chip = NULL;
  struct gpiod_line *input_line = NULL;
  struct gpiod_line *output_line = NULL;
  struct config cfg;
  sigset_t mask;
  int sig_fd = -1;
  int ret;
  int exit_code = 1;

  /* stdout is a pipe under systemd; flush every line to the journal */
  setvbuf(stdout, NULL, _IOLBF, 0);

  /* Load configuration */
  load_config(&cfg);
  print_config(&cfg);

  if (cfg.pin_out == cfg.pin_in) {
    fprintf(stderr, "Pins OUT and IN must differ.\n");
    return 1;
  }

  /* Receive SIGINT/SIGTERM through a file descriptor that poll() watches */
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
    perror("Failed to block signals");
    return 1;
  }
  sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
  if (sig_fd < 0) {
    perror("Failed to create signalfd");
    return 1;
  }

  printf("Mausberry switch daemon starting\n");

  /* Open GPIO chip */
  chip = gpiod_chip_open_by_name(GPIO_CHIP);
  if (!chip) {
    perror("Failed to open GPIO chip");
    goto cleanup;
  }

  if ((unsigned int)cfg.pin_out >= gpiod_chip_num_lines(chip) ||
      (unsigned int)cfg.pin_in >= gpiod_chip_num_lines(chip)) {
    fprintf(stderr, "Pin out of range, %s has %u lines.\n", GPIO_CHIP,
            gpiod_chip_num_lines(chip));
    goto cleanup;
  }

  /* The switch's OUT lead is our input, its IN lead our output */
  input_line = gpiod_chip_get_line(chip, cfg.pin_out);
  output_line = gpiod_chip_get_line(chip, cfg.pin_in);
  if (!input_line || !output_line) {
    perror("Failed to get GPIO lines");
    goto cleanup;
  }

  /*
   * Pull-down keeps an unconnected input low, so a missing switch never
   * triggers a shutdown. Kernels before 5.5 cannot set the bias; fall back
   * to the hardware default then.
   */
  ret = gpiod_line_request_both_edges_events_flags(
      input_line, CONSUMER, GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_DOWN);
  if (ret < 0)
    ret = gpiod_line_request_both_edges_events(input_line, CONSUMER);
  if (ret < 0) {
    perror("Failed to request line events");
    goto cleanup;
  }

  /* Drive high right away to signal the switch that the system is running */
  ret = gpiod_line_request_output(output_line, CONSUMER, 1);
  if (ret < 0) {
    perror("Failed to request output line");
    goto cleanup;
  }

  printf("Mausberry switch monitoring started\n");

  exit_code = monitor(input_line, sig_fd, &cfg);

cleanup:
  if (input_line)
    gpiod_line_release(input_line);
  if (output_line)
    gpiod_line_release(output_line);
  if (chip)
    gpiod_chip_close(chip);
  close(sig_fd);

  printf("Mausberry switch daemon stopping\n");

  return exit_code;
}
