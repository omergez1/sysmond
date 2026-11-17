/*
 * sysmon.h - shared declarations for the multi-threaded sysmond (v2).
 *
 * All threads share one global SystemState (g_state). Every read or write of
 * its fields must happen while holding g_state.lock.
 */

#ifndef SYSMON_H
#define SYSMON_H

#include <pthread.h>
#include <time.h>

#define SYSMON_VERSION   "2.0"
#define SAMPLE_INTERVAL  2      /* seconds between sensor samples / UI redraws */

/* Alert when the 1-minute load average rises above this value.
 * Wrapped in #ifndef so a test build can override it, e.g.
 * -DALERT_LOAD_THRESHOLD=0.0 to trigger an alert immediately. */
#ifndef ALERT_LOAD_THRESHOLD
#define ALERT_LOAD_THRESHOLD 1.50
#endif

typedef struct {
    /* Memory metrics (kB) */
    long mem_total;
    long mem_free;
    long mem_avail;
    long swap_total;
    long swap_free;

    /* CPU load metrics */
    double load1;
    double load5;
    double load15;

    /* Process metrics */
    int total_procs;

    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t  alert_cond;

    /* ---- Additions beyond the guide's struct ---- */

    /* 1 while the program runs; set to 0 on shutdown so threads exit. */
    int running;

    /* Sensor threads sleep on this between samples (see sysmon_sleep), so
     * shutdown can wake them at once instead of waiting out a sleep(). */
    pthread_cond_t stop_cond;

    /* The condition alert_cond waits for. pthread_cond_wait can wake up
     * without a signal ("spurious wakeup"), and a signal sent before the
     * waiter is waiting would be lost, so the waiter checks this flag. */
    int alert_pending;

    /* Details of the most recent alert, so the UI can show it. */
    time_t last_alert;          /* 0 = no alert yet */
    double last_alert_load;

    /* 0 until each sensor has stored its first sample. */
    int have_mem;
    int have_cpu;
    int have_procs;
} SystemState;

/* The one shared state, defined in sysmond.c. */
extern SystemState g_state;

/* Thread routines. arg is the thread's name (const char *). */
extern void *mem_sensor_thread(void *arg);
extern void *cpu_sensor_thread(void *arg);
extern void *proc_sensor_thread(void *arg);
extern void *alert_thread(void *arg);

/* Give the calling thread a name (shown by `ps -T` and in gdb).
 * Each thread calls this first with the name it received as arg. */
extern void sysmon_set_thread_name(const char *name);

/* Return 1 while the program should keep running (reads g_state.running
 * under the lock). Used by the sensor threads' loops. */
extern int sysmon_running(void);

/* Like sleep(seconds), but returns early when shutdown starts.
 * Returns 1 if still running, 0 if the program is stopping. */
extern int sysmon_sleep(int seconds);

#endif /* SYSMON_H */
