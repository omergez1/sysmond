/*
 * sysmond (v3) - supervisor and IPC receiver.
 *
 * On startup it:
 *   1. creates the named pipes CPU_FIFO_PATH and PROC_FIFO_PATH (mkfifo)
 *   2. creates an anonymous pipe and starts mem_sensor with fork + execve,
 *      passing the pipe's write end as argv[1]
 *   3. starts cpu_sensor and proc_sensor with fork + execve
 *   4. opens both FIFOs for reading
 * Then it waits on all three channels with poll(), keeps the newest MemData /
 * CpuData / ProcData it receives, and redraws a report every second.
 *
 * If a sensor dies, sysmond keeps running and reports it as SENSOR DOWN.
 * On SIGINT (Ctrl+C) or SIGTERM it stops every sensor, reaps it with
 * waitpid(), closes all descriptors and removes the FIFO files.
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
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

#define REDRAW_MS    1000   /* time between reports */
#define STALE_AFTER  3      /* seconds without data before flagging it */

/* ---------- the sensors ---------- */

enum { MEM, CPU, PROC, NUM_SENSORS };

struct sensor {
    const char *name;     /* program name, in the same folder as sysmond */
    const char *channel;  /* how its data arrives, for the report */
    pid_t  pid;           /* child PID, or 0 if it was never started */
    int    alive;         /* 1 until waitpid() reports that it ended */
    int    status;        /* waitpid() status, valid once alive == 0 */
    int    fd;            /* our read end of its channel, or -1 if closed */
    void  *data;          /* where its newest struct is stored */
    size_t size;          /* size of that struct */
    int    have_data;     /* 1 once a struct has arrived */
    time_t last_rx;       /* when the newest struct arrived */
};

static MemData  mem_data;
static CpuData  cpu_data;
static ProcData proc_data;

static struct sensor sensors[NUM_SENSORS] = {
    [MEM]  = { "mem_sensor",  "anonymous pipe",  0, 0, 0, -1,
               &mem_data,  sizeof(MemData),  0, 0 },
    [CPU]  = { "cpu_sensor",  CPU_FIFO_PATH,     0, 0, 0, -1,
               &cpu_data,  sizeof(CpuData),  0, 0 },
    [PROC] = { "proc_sensor", PROC_FIFO_PATH,    0, 0, 0, -1,
               &proc_data, sizeof(ProcData), 0, 0 },
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
 * child_close_fd: a descriptor the child must close before exec (-1: none).
 * Returns the child's PID, or -1 on failure. */
static pid_t spawn(struct sensor *s, char *arg1, int child_close_fd)
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
        /* Child: close what it must not hold, then become the sensor. */
        if (child_close_fd >= 0)
            close(child_close_fd);
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

/* Collect any sensors that have exited, without blocking. A child that has
 * exited stays a "zombie" until its parent calls waitpid() on it. */
static void reap_sensors(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            if (sensors[i].pid == pid) {
                sensors[i].alive = 0;
                sensors[i].status = status;
            }
        }
    }
}

/* Send SIGTERM to every running sensor, then wait for each one so none is
 * left behind as an orphan or a zombie. */
static void stop_sensors(void)
{
    for (int i = 0; i < NUM_SENSORS; i++) {
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

static void close_channel(struct sensor *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

/* Read every complete struct waiting on a sensor's channel and keep the
 * newest one. The fd is non-blocking, so read() returns -1 with EAGAIN once
 * the pipe is empty, instead of waiting. */
static void drain_channel(struct sensor *s)
{
    char buf[64];   /* larger than the largest struct */

    while (s->fd >= 0) {
        ssize_t n = read(s->fd, buf, s->size);

        if (n == (ssize_t)s->size) {
            memcpy(s->data, buf, s->size);
            s->have_data = 1;
            s->last_rx = time(NULL);
        } else if (n == 0) {
            /* End of file: the sensor closed its end (it exited). Stop
             * polling this fd, or poll() would report it again forever. */
            close_channel(s);
        } else if (n > 0) {
            /* Can't happen: every write is one whole struct of at most
             * PIPE_BUF bytes, which the kernel delivers in one piece. */
            fprintf(stderr, "sysmond: %s sent %zd of %zu bytes, closing\n",
                    s->name, n, s->size);
            close_channel(s);
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;     /* nothing more to read right now */
        } else {
            perror("sysmond: read");
            close_channel(s);
        }
    }
}

/* ---------- report ---------- */

#define ANSI_CLEAR "\033[H\033[2J"
#define ANSI_BOLD  "\033[1m"
#define ANSI_RED   "\033[1;31m"
#define ANSI_RESET "\033[0m"

static const char *bold = "", *red = "", *reset = "";

/* Describe how a child ended, e.g. "exited with code 127". */
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

/* Print a section title plus any warning about the sensor.
 * Returns 1 if there is data to print under it. */
static int print_heading(const char *title, const struct sensor *s)
{
    printf("%s%s%s\n", bold, title, reset);
    long age = (long)(time(NULL) - s->last_rx);

    if (s->pid == 0) {
        printf("  %s(%s was not started)%s\n", red, s->name, reset);
    } else if (!s->alive) {
        char how[96];
        describe_exit(s->status, how, sizeof(how));
        if (s->have_data)
            printf("  %s(SENSOR DOWN: %s %s - last data from %lds ago)%s\n",
                   red, s->name, how, age, reset);
        else
            printf("  %s(SENSOR DOWN: %s %s - no data)%s\n",
                   red, s->name, how, reset);
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
        printf("  %-12s pid %-7d %-8s via %s\n", s->name, (int)s->pid,
               s->alive ? "running" : "stopped", s->channel);
    }

    printf(ansi ? "\n(Ctrl+C to quit)\n" : "\n");
    /* Without this, output sits in a buffer when stdout is a file or pipe. */
    fflush(stdout);
}

/* ---------- main ---------- */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

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

    /* 2. Anonymous pipe for mem_sensor: pipefd[0] = read, pipefd[1] = write. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        ok = 0;
        goto cleanup;
    }
    /* FD_CLOEXEC: the read end closes automatically in any program we
     * execve(), so no sensor ever holds it by accident. */
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    sensors[MEM].fd = pipefd[0];

    char fd_arg[16];
    snprintf(fd_arg, sizeof(fd_arg), "%d", pipefd[1]);
    spawn(&sensors[MEM], fd_arg, pipefd[0]);

    /* The parent never writes to the pipe. Closing our write end also means
     * that when mem_sensor exits, no writer is left and we get end-of-file. */
    close(pipefd[1]);

    /* 3. The FIFO sensors. pipefd[0] is passed so the children close it. */
    spawn(&sensors[CPU], NULL, pipefd[0]);
    spawn(&sensors[PROC], NULL, pipefd[0]);

    /* 4. Open the FIFOs for reading, after every fork so no child inherits
     * them. O_NONBLOCK matters: a normal open waits until a writer opens the
     * other end, which would hang forever if a sensor failed to start. */
    sensors[CPU].fd  = open(CPU_FIFO_PATH,  O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    sensors[PROC].fd = open(PROC_FIFO_PATH, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (sensors[CPU].fd < 0 || sensors[PROC].fd < 0) {
        perror("sysmond: open FIFO");
        ok = 0;
        goto cleanup;
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
        struct pollfd pfd[NUM_SENSORS];
        for (int i = 0; i < NUM_SENSORS; i++) {
            pfd[i].fd = sensors[i].fd;    /* poll() skips negative fds */
            pfd[i].events = POLLIN;
            pfd[i].revents = 0;
        }

        long wait = next_redraw - now_ms();
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
            if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))
                drain_channel(&sensors[i]);
        }

        reap_sensors();

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
