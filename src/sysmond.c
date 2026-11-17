/*
 * sysmond (v2) - multi-threaded system monitor.
 *
 * The main thread starts four worker threads:
 *   mem_sensor_thread, cpu_sensor_thread, proc_sensor_thread - sample /proc
 *     every SAMPLE_INTERVAL seconds and store the values in g_state
 *   alert_thread - waits on g_state.alert_cond and prints an alert to stderr
 *     when the CPU sensor reports a load spike
 * Then it redraws a status report every SAMPLE_INTERVAL seconds until it
 * receives SIGINT (Ctrl+C) or SIGTERM, and shuts every thread down cleanly.
 */

#define _GNU_SOURCE   /* for pthread_setname_np() */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sysmon.h>

/* The shared state. Globals start zeroed; the mutex and condition variable
 * are initialized in main() before any thread starts. */
SystemState g_state;

void sysmon_set_thread_name(const char *name)
{
    /* Linux limits thread names to 15 characters; ours are shorter. */
    pthread_setname_np(pthread_self(), name);
}

int sysmon_running(void)
{
    pthread_mutex_lock(&g_state.lock);
    int running = g_state.running;
    pthread_mutex_unlock(&g_state.lock);
    return running;
}

int sysmon_sleep(int seconds)
{
    /* stop_cond uses CLOCK_MONOTONIC (set in main), which only moves
     * forward, so changing the system time can't break the wait. */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&g_state.lock);
    while (g_state.running) {
        /* Returns ETIMEDOUT when the time is up; any other return means we
         * were woken (or woke spuriously), so loop and re-check running. */
        if (pthread_cond_timedwait(&g_state.stop_cond, &g_state.lock,
                                   &deadline) == ETIMEDOUT)
            break;
    }
    int running = g_state.running;
    pthread_mutex_unlock(&g_state.lock);
    return running;
}

/* ---------- worker threads ---------- */

struct worker {
    const char *name;
    void *(*func)(void *);
    pthread_t   tid;
    int         started;
};

static struct worker workers[] = {
    { "mem_sensor",  mem_sensor_thread,  0, 0 },
    { "cpu_sensor",  cpu_sensor_thread,  0, 0 },
    { "proc_sensor", proc_sensor_thread, 0, 0 },
    { "alert",       alert_thread,       0, 0 },
};
#define NUM_WORKERS (sizeof(workers) / sizeof(workers[0]))

/* Tell every thread to stop, wake the alert thread (it may be asleep in
 * pthread_cond_wait), and wait for each thread to finish. */
static void stop_workers(void)
{
    pthread_mutex_lock(&g_state.lock);
    g_state.running = 0;
    pthread_cond_broadcast(&g_state.alert_cond);
    pthread_cond_broadcast(&g_state.stop_cond);
    pthread_mutex_unlock(&g_state.lock);

    for (size_t i = 0; i < NUM_WORKERS; i++) {
        if (workers[i].started) {
            pthread_join(workers[i].tid, NULL);
            workers[i].started = 0;
        }
    }
}

/* ---------- report ---------- */

/* A copy of the metrics, taken under the lock, so the report can be printed
 * without holding the lock (printing is slow; the sensors would wait). */
struct snapshot {
    long   mem_total, mem_free, mem_avail, swap_total, swap_free;
    double load1, load5, load15;
    int    total_procs;
    int    have_mem, have_cpu, have_procs;
    time_t last_alert;
    double last_alert_load;
};

static void take_snapshot(struct snapshot *s)
{
    pthread_mutex_lock(&g_state.lock);
    s->mem_total       = g_state.mem_total;
    s->mem_free        = g_state.mem_free;
    s->mem_avail       = g_state.mem_avail;
    s->swap_total      = g_state.swap_total;
    s->swap_free       = g_state.swap_free;
    s->load1           = g_state.load1;
    s->load5           = g_state.load5;
    s->load15          = g_state.load15;
    s->total_procs     = g_state.total_procs;
    s->have_mem        = g_state.have_mem;
    s->have_cpu        = g_state.have_cpu;
    s->have_procs      = g_state.have_procs;
    s->last_alert      = g_state.last_alert;
    s->last_alert_load = g_state.last_alert_load;
    pthread_mutex_unlock(&g_state.lock);
}

static double kb_to_mb(long kb)
{
    return (double)kb / 1024.0;
}

static double percent(long part, long whole)
{
    return whole > 0 ? 100.0 * (double)part / (double)whole : 0.0;
}

/* ANSI escape codes, used only when stdout is a terminal. */
#define ANSI_CLEAR "\033[H\033[2J"
#define ANSI_BOLD  "\033[1m"
#define ANSI_RED   "\033[1;31m"
#define ANSI_RESET "\033[0m"

static void print_report(int ansi)
{
    struct snapshot s;
    take_snapshot(&s);

    const char *bold  = ansi ? ANSI_BOLD  : "";
    const char *red   = ansi ? ANSI_RED   : "";
    const char *reset = ansi ? ANSI_RESET : "";

    char stamp[32];
    struct tm tm;
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));

    if (ansi)
        printf(ANSI_CLEAR);
    printf("%s========== sysmon v%s report %s ==========%s\n",
           bold, SYSMON_VERSION, stamp, reset);

    printf("%sMemory%s\n", bold, reset);
    if (!s.have_mem) {
        printf("  (waiting for data)\n");
    } else {
        /* "Used" = memory not available to new programs, as `free` shows. */
        long used = s.mem_total - s.mem_avail;
        long swap_used = s.swap_total - s.swap_free;
        printf("  Total:     %9.1f MB\n", kb_to_mb(s.mem_total));
        printf("  Used:      %9.1f MB (%.1f%%)\n", kb_to_mb(used),
               percent(used, s.mem_total));
        printf("  Free:      %9.1f MB\n", kb_to_mb(s.mem_free));
        printf("  Available: %9.1f MB\n", kb_to_mb(s.mem_avail));
        printf("  Swap:      %9.1f / %.1f MB used (%.1f%%)\n",
               kb_to_mb(swap_used), kb_to_mb(s.swap_total),
               percent(swap_used, s.swap_total));
    }

    printf("%sCPU load%s\n", bold, reset);
    if (!s.have_cpu) {
        printf("  (waiting for data)\n");
    } else {
        printf("  1 min: %.2f   5 min: %.2f   15 min: %.2f\n",
               s.load1, s.load5, s.load15);
        if (s.load1 > ALERT_LOAD_THRESHOLD)
            printf("  %sHIGH LOAD: 1-min load is above %.2f%s\n",
                   red, ALERT_LOAD_THRESHOLD, reset);
    }

    printf("%sProcesses%s\n", bold, reset);
    if (!s.have_procs)
        printf("  (waiting for data)\n");
    else
        printf("  Running: %d\n", s.total_procs);

    printf("%sAlerts%s\n", bold, reset);
    if (s.last_alert == 0) {
        printf("  none\n");
    } else {
        char when[16];
        strftime(when, sizeof(when), "%H:%M:%S", localtime_r(&s.last_alert, &tm));
        printf("  %sLast alert: %s (load %.2f)%s\n",
               red, when, s.last_alert_load, reset);
    }

    if (ansi)
        printf("\n(Ctrl+C to quit)\n");
    else
        printf("\n");

    /* Without this, output sits in a buffer when stdout is a file or pipe. */
    fflush(stdout);
}

/* ---------- main ---------- */

/* Wait up to `ms` milliseconds for SIGINT/SIGTERM.
 * Returns 1 if one arrived (time to shut down), 0 on timeout. */
static int wait_for_stop_signal(const sigset_t *set, long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (1) {
        int sig = sigtimedwait(set, NULL, &ts);
        if (sig == SIGINT || sig == SIGTERM)
            return 1;
        if (sig == -1 && errno == EAGAIN)
            return 0;               /* timed out: no signal */
        /* EINTR (interrupted by another signal): wait again. Restarting
         * with the full timeout at worst delays one redraw slightly. */
    }
}

int main(void)
{
    int rc;
    pthread_condattr_t mono;

    if ((rc = pthread_mutex_init(&g_state.lock, NULL)) != 0 ||
        (rc = pthread_cond_init(&g_state.alert_cond, NULL)) != 0 ||
        (rc = pthread_condattr_init(&mono)) != 0 ||
        (rc = pthread_condattr_setclock(&mono, CLOCK_MONOTONIC)) != 0 ||
        (rc = pthread_cond_init(&g_state.stop_cond, &mono)) != 0) {
        fprintf(stderr, "sysmond: init failed: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }
    pthread_condattr_destroy(&mono);
    g_state.running = 1;    /* no lock needed: no other thread exists yet */

    /* Block SIGINT and SIGTERM in this thread *before* creating the others;
     * new threads inherit the mask, so the signals stay pending until the
     * main thread collects them with sigtimedwait(). This way a Ctrl+C can
     * never land in a worker thread at a random moment. */
    sigset_t stop_set;
    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGINT);
    sigaddset(&stop_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_set, NULL);

    int ok = 1;
    for (size_t i = 0; i < NUM_WORKERS; i++) {
        /* The thread's name is passed as its argument. A string literal
         * lives for the whole program, so it's safe to share. */
        rc = pthread_create(&workers[i].tid, NULL, workers[i].func,
                            (void *)workers[i].name);
        if (rc != 0) {
            fprintf(stderr, "sysmond: cannot start %s thread: %s\n",
                    workers[i].name, strerror(rc));
            ok = 0;
            break;
        }
        workers[i].started = 1;
    }

    if (ok) {
        int ansi = isatty(STDOUT_FILENO);

        /* Give the sensors a moment to take their first sample. */
        if (!wait_for_stop_signal(&stop_set, 300)) {
            do {
                print_report(ansi);
            } while (!wait_for_stop_signal(&stop_set, SAMPLE_INTERVAL * 1000L));
        }
    }

    stop_workers();
    pthread_cond_destroy(&g_state.alert_cond);
    pthread_cond_destroy(&g_state.stop_cond);
    pthread_mutex_destroy(&g_state.lock);

    printf("sysmond: stopped\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
