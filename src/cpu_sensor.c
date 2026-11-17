/*
 * cpu_sensor - thread that reads the 1, 5 and 15 minute load averages from
 * /proc/loadavg and stores them in the shared state every SAMPLE_INTERVAL
 * seconds. When load1 rises above ALERT_LOAD_THRESHOLD it wakes the alert
 * thread, once per spike.
 */

#include <stdio.h>

#include <sysmon.h>

#define LOADAVG_PATH "/proc/loadavg"

struct cpu_stats {
    double load1;
    double load5;
    double load15;
};

/* Read /proc/loadavg. Its single line looks like:
 *   "0.40 0.28 0.20 3/1092 5850"
 * The first three numbers are the 1, 5 and 15 minute load averages.
 * Returns 0 on success, -1 on failure. */
static int read_loadavg(struct cpu_stats *stats)
{
    FILE *fp = fopen(LOADAVG_PATH, "r");
    if (fp == NULL) {
        perror("fopen " LOADAVG_PATH);
        return -1;
    }

    char line[128];
    int ok = fgets(line, sizeof(line), fp) != NULL &&
             sscanf(line, "%lf %lf %lf",
                    &stats->load1, &stats->load5, &stats->load15) == 3;

    fclose(fp);

    if (!ok) {
        fprintf(stderr, "cpu_sensor: could not parse " LOADAVG_PATH "\n");
        return -1;
    }
    return 0;
}

void *cpu_sensor_thread(void *arg)
{
    sysmon_set_thread_name((const char *)arg);
    struct cpu_stats stats;

    /* 1 while load1 is above the threshold. Used to alert only when the
     * load crosses the threshold, not on every sample while it stays high.
     * Only this thread uses it, so it needs no lock. */
    int above = 0;

    while (sysmon_running()) {
        if (read_loadavg(&stats) == 0) {
            pthread_mutex_lock(&g_state.lock);
            g_state.load1    = stats.load1;
            g_state.load5    = stats.load5;
            g_state.load15   = stats.load15;
            g_state.have_cpu = 1;

            if (stats.load1 > ALERT_LOAD_THRESHOLD) {
                if (!above) {
                    above = 1;
                    g_state.alert_pending   = 1;
                    g_state.last_alert_load = stats.load1;
                    pthread_cond_signal(&g_state.alert_cond);
                }
            } else {
                above = 0;   /* back to normal: the next spike alerts again */
            }
            pthread_mutex_unlock(&g_state.lock);
        }
        sysmon_sleep(SAMPLE_INTERVAL);
    }
    return NULL;
}
