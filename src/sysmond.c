/*
 * sysmond (v3b) - fault-tolerant supervisor (watchdog) and IPC receiver.
 *
 * On startup it:
 *   1. creates the named pipes CPU_FIFO_PATH and PROC_FIFO_PATH (mkfifo)
 *      and opens them for reading
 *   2. creates an anonymous pipe and starts mem_sensor with fork + execve,
 *      passing the pipe's write end as argv[1]
 *   3. starts cpu_sensor and proc_sensor with fork + execve
 * Then it waits on all three channels with poll(), keeps the newest MemData /
 * CpuData / ProcData it receives, and redraws a report every second.
 *
 * Watchdog: when a sensor dies, read() on its channel returns 0 (end of
 * file). sysmond then reaps the child with waitpid(), rebuilds the channel
 * and starts the sensor again:
 *   - mem_sensor: the old anonymous pipe can never get a writer again, so it
 *     is closed and a new pipe is created for the new mem_sensor.
 *   - cpu_sensor / proc_sensor: the FIFO read end stays open. The new sensor
 *     opens the same FIFO for writing and the data continues on the same fd.
 * The other sensors are not affected. Each sensor's restarts are counted and
 * shown in the report.
 *
 * On SIGINT (Ctrl+C) or SIGTERM it stops every sensor (no restarts), reaps
 * them with waitpid(), closes all descriptors and removes the FIFO files.
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sysmon_ipc.h>

extern char **environ;      /* the current environment, passed to execve() */

#define REDRAW_MS        1000  /* time between reports */
#define STALE_AFTER      3     /* seconds without data before flagging it */
#define RESTART_MIN_MS   1000  /* at least this long between two starts of
                                  the same sensor, so a sensor that dies
                                  right away can't restart in a tight loop */
#define ATTACH_CHECK_MS  50    /* how often to check whether a new FIFO
                                  writer has connected */
#define LOG_LINES        5     /* recovery events kept for the report */

/* ---------- the sensors ---------- */

enum { MEM, CPU, PROC, NUM_SENSORS };

struct sensor {
    const char *name;     /* program name, in the same folder as sysmond */
    const char *channel;  /* how its data arrives, for the report */
    int    uses_pipe;     /* 1: anonymous pipe (mem_sensor), 0: FIFO */
    pid_t  pid;           /* current child PID, or 0 if never started */
    int    alive;         /* 1 from fork() until waitpid() reports its end */
    int    status;        /* waitpid() status of the last child that ended */
    int    fd;            /* our read end of its channel, or -1 if closed */
    int    attached;      /* 1 while a writer is connected to fd */
    int    restarts;      /* how many times the watchdog restarted it */
    long   started_ms;    /* when the current child was started */
    long   restart_at;    /* if > 0: restart it at this time (now_ms) */
    void  *data;          /* where its newest struct is stored */
    size_t size;          /* size of that struct */
    int    have_data;     /* 1 once a struct has arrived */
    time_t last_rx;       /* when the newest struct arrived */
};

static MemData  mem_data;
static CpuData  cpu_data;
static ProcData proc_data;

static struct sensor sensors[NUM_SENSORS] = {
    [MEM]  = { .name = "mem_sensor",  .channel = "anonymous pipe",
               .uses_pipe = 1, .fd = -1,
               .data = &mem_data,  .size = sizeof(MemData) },
    [CPU]  = { .name = "cpu_sensor",  .channel = CPU_FIFO_PATH,
               .fd = -1, .data = &cpu_data,  .size = sizeof(CpuData) },
    [PROC] = { .name = "proc_sensor", .channel = PROC_FIFO_PATH,
               .fd = -1, .data = &proc_data, .size = sizeof(ProcData) },
};

/* ---------- signals ---------- */

/* The handler only sets this flag. The real cleanup runs in main(), because
 * very few functions are safe to call from inside a signal handler. */
static volatile sig_atomic_t stop_requested = 0;

static void on_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* ---------- helpers ---------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Describe how a child ended, e.g. "killed by signal 9 (Killed)". */
static void describe_exit(int status, char *out, size_t size)
{
    if (WIFEXITED(status))
        snprintf(out, size, "exited with code %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(out, size, "killed by signal %d (%s)",
                 WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        snprintf(out, size, "stopped");
}

/* Recovery log: the last LOG_LINES events, each starting with the time. */
static char event_log[LOG_LINES][160];
static int  log_next = 0, log_count = 0;

static void log_event(const char *fmt, ...)
{
    char stamp[16];
    struct tm tm;
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime_r(&now, &tm));

    char text[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    snprintf(event_log[log_next], sizeof(event_log[log_next]), "%s %s",
             stamp, text);
    log_next = (log_next + 1) % LOG_LINES;
    if (log_count < LOG_LINES)
        log_count++;
}

/* ---------- starting and stopping sensors ---------- */

/* Build the full path of a sensor program. The sensors live in the same
 * folder as sysmond, so this works no matter which folder you run it from.
 * /proc/self/exe is a link to the program that is currently running. */
static int sensor_path(const char *name, char *out, size_t size)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) {
        perror("readlink /proc/self/exe");
        return -1;
    }
    self[n] = '\0';

    /* dirname() turns ".../bin/sysmond" into ".../bin" (in place). */
    int len = snprintf(out, size, "%s/%s", dirname(self), name);
    if (len < 0 || (size_t)len >= size) {
        fprintf(stderr, "sysmond: path to %s is too long\n", name);
        return -1;
    }
    return 0;
}

/* fork() a child and execve() the sensor program in it.
 * arg1: optional first argument for the sensor (NULL: none).
 * Every descriptor sysmond reads from is marked FD_CLOEXEC, so execve()
 * closes them in the child: a sensor only keeps the channel it writes to.
 * Returns the child's PID, or -1 on failure. */
static pid_t spawn(struct sensor *s, char *arg1)
{
    char path[PATH_MAX];
    if (sensor_path(s->name, path, sizeof(path)) != 0)
        return -1;

    /* argv[0] is, by convention, the path the program was started from.
     * It's also what `ps` shows, e.g. ".../bin/cpu_sensor". */
    char *argv[] = { path, arg1, NULL };

    /* Flush first, or text still waiting in our stdout buffer would be
     * copied into the child too. */
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execve(path, argv, environ);
        /* execve only returns if it failed. */
        perror(path);
        _exit(127);   /* _exit: don't run the parent's cleanup code */
    }

    s->pid = pid;
    s->alive = 1;
    printf("sysmond: started %s (pid %d)\n", s->name, (int)pid);
    return pid;
}

static void close_channel(struct sensor *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    s->attached = 0;
}

/* Start (or restart) a sensor together with its channel.
 * Returns 0 on success, -1 on failure. */
static int start_sensor(struct sensor *s)
{
    s->started_ms = now_ms();

    if (!s->uses_pipe) {
        /* FIFO: our read end is already open. The sensor opens the FIFO for
         * writing itself; until it does, there is no writer (attached = 0). */
        s->attached = 0;
        return spawn(s, NULL) > 0 ? 0 : -1;
    }

    /* Anonymous pipe. An old pipe whose writer died can never get a new
     * writer (it has no name to open), so it is closed and a new one made. */
    close_channel(s);

    int pipefd[2];          /* pipefd[0] = read end, pipefd[1] = write end */
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }
    /* FD_CLOEXEC: the read end closes automatically in any program we
     * execve(), so no sensor ever holds it by accident. */
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    char fd_arg[16];
    snprintf(fd_arg, sizeof(fd_arg), "%d", pipefd[1]);
    pid_t pid = spawn(s, fd_arg);

    /* The parent never writes to the pipe. Closing our write end means that
     * when mem_sensor exits, no writer is left and we get end-of-file. It
     * also means no later child can inherit it. */
    close(pipefd[1]);

    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }
    s->fd = pipefd[0];
    s->attached = 1;
    return 0;
}

static int drain_channel(struct sensor *s);

/* A sensor's child has ended and was reaped: keep its status, retire its
 * channel and schedule the restart (unless sysmond itself is stopping). */
static void sensor_ended(struct sensor *s, int status)
{
    s->alive = 0;
    s->status = status;

    /* Keep any data it wrote just before it died. */
    if (s->fd >= 0 && s->attached)
        drain_channel(s);

    if (s->uses_pipe)
        close_channel(s);   /* dead for good; restart makes a new pipe */
    else
        s->attached = 0;    /* keep the FIFO open for the next writer */

    if (!stop_requested) {
        long now = now_ms();
        long earliest = s->started_ms + RESTART_MIN_MS;
        s->restart_at = now > earliest ? now : earliest;
    }
}

/* End-of-file on a sensor's channel: its writer is gone, which means the
 * sensor died. Reap that child now so it doesn't stay a zombie. */
static void reap_sensor(struct sensor *s)
{
    int status = 0;
    pid_t r;

    while ((r = waitpid(s->pid, &status, WNOHANG)) < 0 && errno == EINTR)
        ;
    if (r == 0) {
        /* Not finished exiting yet, or it closed its channel and is still
         * running. Either way it can't send data any more: end it. */
        kill(s->pid, SIGKILL);
        while ((r = waitpid(s->pid, &status, 0)) < 0 && errno == EINTR)
            ;
    }
    sensor_ended(s, status);
}

/* Collect any sensor that has exited, without blocking. This catches a
 * sensor that died without us seeing end-of-file, e.g. one that failed
 * before it ever opened its FIFO. */
static void reap_any(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            if (sensors[i].alive && sensors[i].pid == pid)
                sensor_ended(&sensors[i], status);
        }
    }
}

/* Restart every sensor whose restart time has come. */
static void restart_due_sensors(void)
{
    long now = now_ms();

    for (int i = 0; i < NUM_SENSORS; i++) {
        struct sensor *s = &sensors[i];
        if (s->alive || s->restart_at == 0 || now < s->restart_at)
            continue;

        s->restart_at = 0;
        char how[96];
        describe_exit(s->status, how, sizeof(how));

        if (start_sensor(s) == 0) {
            s->restarts++;
            log_event("%s %s -> restarted as pid %d (restart %d)",
                      s->name, how, (int)s->pid, s->restarts);
        } else {
            log_event("%s %s -> restart failed, retrying", s->name, how);
            s->restart_at = now + RESTART_MIN_MS;
        }
    }
}

/* Send SIGTERM to every running sensor, then wait for each one so none is
 * left behind as an orphan or a zombie. Nothing is restarted. */
static void stop_sensors(void)
{
    for (int i = 0; i < NUM_SENSORS; i++) {
        sensors[i].restart_at = 0;
        if (sensors[i].alive)
            kill(sensors[i].pid, SIGTERM);
    }
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (!sensors[i].alive)
            continue;
        while (waitpid(sensors[i].pid, &sensors[i].status, 0) < 0 &&
               errno == EINTR)
            ;   /* interrupted by another signal: wait again */
        sensors[i].alive = 0;
    }
}

/* ---------- IPC channels ---------- */

/* Set once each FIFO is created, so cleanup only removes FIFOs this run
 * made (never, say, a regular file that happened to be at that path). */
static int made_cpu_fifo = 0, made_proc_fifo = 0;

/* Create a fresh FIFO at path. A FIFO left over from a run that was killed
 * is removed first; anything else at that path is an error. */
static int make_fifo(const char *path)
{
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "sysmond: %s exists and is not a FIFO\n", path);
            return -1;
        }
        if (unlink(path) != 0) {
            perror(path);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(path);
        return -1;
    }

    if (mkfifo(path, 0600) != 0) {
        perror(path);
        return -1;
    }
    return 0;
}

/* Read every complete struct waiting on a sensor's channel and keep the
 * newest one. The fd is non-blocking, so read() returns -1 with EAGAIN once
 * the pipe is empty, instead of waiting.
 * read() returning 0 (end of file) means no writer is connected.
 * Returns 1 on end of file (or a broken channel), 0 otherwise. */
static int drain_channel(struct sensor *s)
{
    char buf[64];   /* larger than the largest struct */

    for (;;) {
        ssize_t n = read(s->fd, buf, s->size);

        if (n == (ssize_t)s->size) {
            memcpy(s->data, buf, s->size);
            s->have_data = 1;
            s->last_rx = time(NULL);
            s->attached = 1;
        } else if (n == 0) {
            return 1;
        } else if (n > 0) {
            /* Can't happen: every write is one whole struct of at most
             * PIPE_BUF bytes, which the kernel delivers in one piece. */
            fprintf(stderr, "sysmond: %s sent %zd of %zu bytes\n",
                    s->name, n, s->size);
            return 1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Empty, but a writer is connected (with no writer, read()
             * would have returned 0). */
            s->attached = 1;
            return 0;
        } else {
            perror("sysmond: read");
            return 1;
        }
    }
}

/* Read a channel and react to end of file. */
static void check_channel(struct sensor *s)
{
    int was_attached = s->attached;

    if (drain_channel(s) == 0)
        return;

    /* End of file. On a FIFO whose new writer hasn't connected yet this is
     * normal: keep waiting. Otherwise the writer is gone: the sensor died. */
    if (!was_attached && !s->attached)
        return;
    if (s->alive)
        reap_sensor(s);
    else if (s->uses_pipe)
        close_channel(s);
    else
        s->attached = 0;
}

/* ---------- report ---------- */

#define ANSI_CLEAR "\033[H\033[2J"
#define ANSI_BOLD  "\033[1m"
#define ANSI_RED   "\033[1;31m"
#define ANSI_RESET "\033[0m"

static const char *bold = "", *red = "", *reset = "";

/* Print a section title plus any warning about the sensor.
 * Returns 1 if there is data to print under it. */
static int print_heading(const char *title, const struct sensor *s)
{
    printf("%s%s%s\n", bold, title, reset);
    long age = (long)(time(NULL) - s->last_rx);

    if (s->pid == 0 && s->restart_at == 0) {
        printf("  %s(%s was not started)%s\n", red, s->name, reset);
    } else if (!s->alive) {
        char how[96];
        describe_exit(s->status, how, sizeof(how));
        if (s->restart_at > 0) {
            long in = s->restart_at - now_ms();
            printf("  %s(RESTARTING: %s %s - restart in %.1fs)%s\n",
                   red, s->name, how, in > 0 ? (double)in / 1000.0 : 0.0,
                   reset);
        } else {
            printf("  %s(SENSOR DOWN: %s %s)%s\n", red, s->name, how, reset);
        }
    } else if (!s->have_data) {
        printf("  (waiting for data)\n");
    } else if (age > STALE_AFTER) {
        printf("  %s(STALE: last update %lds ago)%s\n", red, age, reset);
    }
    return s->have_data;
}

static double kb_to_mb(long kb)
{
    return (double)kb / 1024.0;
}

static double percent(long part, long whole)
{
    return whole > 0 ? 100.0 * (double)part / (double)whole : 0.0;
}

static void print_report(int ansi)
{
    char stamp[32];
    struct tm tm;
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));

    if (ansi)
        printf(ANSI_CLEAR);
    printf("%s========== sysmon v%s report %s ==========%s\n",
           bold, SYSMON_VERSION, stamp, reset);

    if (print_heading("Memory", &sensors[MEM])) {
        const MemData *m = &mem_data;
        /* "Used" = memory not available to new programs, as `free` shows. */
        long used = m->mem_total - m->mem_avail;
        long swap_used = m->swap_total - m->swap_free;
        printf("  Total:     %9.1f MB\n", kb_to_mb(m->mem_total));
        printf("  Used:      %9.1f MB (%.1f%%)\n", kb_to_mb(used),
               percent(used, m->mem_total));
        printf("  Free:      %9.1f MB\n", kb_to_mb(m->mem_free));
        printf("  Available: %9.1f MB\n", kb_to_mb(m->mem_avail));
        printf("  Swap:      %9.1f / %.1f MB used (%.1f%%)\n",
               kb_to_mb(swap_used), kb_to_mb(m->swap_total),
               percent(swap_used, m->swap_total));
    }

    if (print_heading("CPU load", &sensors[CPU]))
        printf("  1 min: %.2f   5 min: %.2f   15 min: %.2f\n",
               cpu_data.load1, cpu_data.load5, cpu_data.load15);

    if (print_heading("Processes", &sensors[PROC]))
        printf("  Running: %d\n", proc_data.total_procs);

    printf("%sSensors%s\n", bold, reset);
    for (int i = 0; i < NUM_SENSORS; i++) {
        const struct sensor *s = &sensors[i];
        const char *state = s->alive ? "running"
                          : s->restart_at > 0 ? "restarting" : "stopped";
        printf("  %-12s pid %-7d %-10s Restart Count: %-3d via %s\n",
               s->name, (int)s->pid, state, s->restarts, s->channel);
    }

    if (log_count > 0) {
        printf("%sRecovery log%s\n", bold, reset);
        /* Newest first. */
        for (int k = 1; k <= log_count; k++)
            printf("  %s\n", event_log[(log_next - k + LOG_LINES) % LOG_LINES]);
    }

    printf(ansi ? "\n(Ctrl+C to quit)\n" : "\n");
    /* Without this, output sits in a buffer when stdout is a file or pipe. */
    fflush(stdout);
}

/* ---------- main ---------- */

int main(void)
{
    int ok = 1;

    /* SIGINT = Ctrl+C, SIGTERM = `kill`. No SA_RESTART: a signal makes
     * poll() return early with EINTR, so the loop notices the flag at once.
     * Installed before forking; execve() resets them to default in the
     * sensors. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* 1. Named pipes for cpu_sensor and proc_sensor. */
    if (make_fifo(CPU_FIFO_PATH) != 0) {
        ok = 0;
        goto cleanup;
    }
    made_cpu_fifo = 1;
    if (make_fifo(PROC_FIFO_PATH) != 0) {
        ok = 0;
        goto cleanup;
    }
    made_proc_fifo = 1;

    /* 2. Open the FIFOs for reading. They stay open for the whole run, so a
     * restarted sensor's open(O_WRONLY) finds a reader and returns at once.
     * O_NONBLOCK: open() doesn't wait for a writer, and read() never blocks.
     * O_CLOEXEC: sensors started later don't inherit them. */
    sensors[CPU].fd  = open(CPU_FIFO_PATH,  O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    sensors[PROC].fd = open(PROC_FIFO_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (sensors[CPU].fd < 0 || sensors[PROC].fd < 0) {
        perror("sysmond: open FIFO");
        ok = 0;
        goto cleanup;
    }

    /* 3. Start the sensors (mem_sensor also gets its anonymous pipe). If
     * one can't be started, the watchdog tries again later. */
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (start_sensor(&sensors[i]) != 0)
            sensors[i].restart_at = now_ms() + RESTART_MIN_MS;
    }

    int ansi = isatty(STDOUT_FILENO);
    if (ansi) {
        bold = ANSI_BOLD;
        red = ANSI_RED;
        reset = ANSI_RESET;
    }

    /* First report after a short delay, so the first samples have arrived. */
    long next_redraw = now_ms() + 300;

    while (!stop_requested) {
        long now = now_ms();
        long wait = next_redraw - now;
        int checking = 0;
        struct pollfd pfd[NUM_SENSORS];

        for (int i = 0; i < NUM_SENSORS; i++) {
            struct sensor *s = &sensors[i];
            /* Only poll channels with a connected writer. A FIFO with no
             * writer reports POLLHUP on every call, which would make this
             * loop spin; those are checked every ATTACH_CHECK_MS instead.
             * poll() skips negative fds. */
            pfd[i].fd = (s->fd >= 0 && s->attached) ? s->fd : -1;
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
            if (s->fd >= 0 && !s->attached && s->alive)
                checking = 1;
            if (s->restart_at > 0 && s->restart_at - now < wait)
                wait = s->restart_at - now;
        }
        if (checking && wait > ATTACH_CHECK_MS)
            wait = ATTACH_CHECK_MS;
        if (wait < 0)
            wait = 0;

        int n = poll(pfd, NUM_SENSORS, (int)wait);
        if (n < 0) {
            if (errno == EINTR)
                continue;     /* a signal: the loop condition checks it */
            perror("poll");
            ok = 0;
            break;
        }

        /* POLLIN = data waiting. POLLHUP = the writer is gone. Either way,
         * read: it returns the remaining data and then end-of-file. */
        for (int i = 0; i < NUM_SENSORS; i++) {
            struct sensor *s = &sensors[i];
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
                check_channel(s);
            else if (s->fd >= 0 && !s->attached && s->alive)
                check_channel(s);   /* has the new writer connected yet? */
        }

        reap_any();
        if (!stop_requested)
            restart_due_sensors();

        long t = now_ms();
        if (t >= next_redraw) {
            print_report(ansi);
            next_redraw += REDRAW_MS;
            if (next_redraw <= t)          /* fell behind: don't catch up */
                next_redraw = t + REDRAW_MS;
        }
    }

cleanup:
    if (ok)
        printf("sysmond: stopping sensors...\n");
    stop_sensors();
    for (int i = 0; i < NUM_SENSORS; i++)
        close_channel(&sensors[i]);
    if (made_cpu_fifo)
        unlink(CPU_FIFO_PATH);
    if (made_proc_fifo)
        unlink(PROC_FIFO_PATH);
    printf("sysmond: stopped\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
