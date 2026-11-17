/*
 * mem_sensor - thread that reads memory metrics from /proc/meminfo and
 * stores them in the shared state every SAMPLE_INTERVAL seconds.
 */

#include <stdio.h>
#include <string.h>

#include <sysmon.h>

#define MEMINFO_PATH "/proc/meminfo"

struct mem_stats {
    long mem_total;
    long mem_free;
    long mem_available;
    long swap_total;
    long swap_free;
};

/* Read /proc/meminfo and fill in the fields we care about (values in kB).
 * Returns 0 on success, -1 on failure. */
static int read_meminfo(struct mem_stats *stats)
{
    FILE *fp = fopen(MEMINFO_PATH, "r");
    if (fp == NULL) {
        perror("fopen " MEMINFO_PATH);
        return -1;
    }

    memset(stats, 0, sizeof(*stats));

    char line[256];
    char key[64];
    long value;
    int found = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Each line looks like: "MemTotal:       16318412 kB" */
        if (sscanf(line, "%63[^:]: %ld", key, &value) != 2)
            continue;

        if (strcmp(key, "MemTotal") == 0) {
            stats->mem_total = value;
            found++;
        } else if (strcmp(key, "MemFree") == 0) {
            stats->mem_free = value;
            found++;
        } else if (strcmp(key, "MemAvailable") == 0) {
            stats->mem_available = value;
            found++;
        } else if (strcmp(key, "SwapTotal") == 0) {
            stats->swap_total = value;
            found++;
        } else if (strcmp(key, "SwapFree") == 0) {
            stats->swap_free = value;
            found++;
        }
    }

    fclose(fp);

    if (found != 5) {
        fprintf(stderr, "mem_sensor: expected 5 fields, found %d\n", found);
        return -1;
    }
    return 0;
}

void *mem_sensor_thread(void *arg)
{
    sysmon_set_thread_name((const char *)arg);
    struct mem_stats stats;

    while (sysmon_running()) {
        /* Read /proc outside the lock so other threads aren't kept waiting
         * while we do file I/O. Only the copy into g_state is locked. */
        if (read_meminfo(&stats) == 0) {
            pthread_mutex_lock(&g_state.lock);
            g_state.mem_total  = stats.mem_total;
            g_state.mem_free   = stats.mem_free;
            g_state.mem_avail  = stats.mem_available;
            g_state.swap_total = stats.swap_total;
            g_state.swap_free  = stats.swap_free;
            g_state.have_mem   = 1;
            pthread_mutex_unlock(&g_state.lock);
        }
        sysmon_sleep(SAMPLE_INTERVAL);
    }
    return NULL;
}
