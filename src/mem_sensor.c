/*
 * mem_sensor - reads memory metrics from /proc/meminfo and writes them
 * to /tmp/sysmon/mem.dat every INTERVAL seconds.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MEMINFO_PATH "/proc/meminfo"
#define OUT_DIR      "/tmp/sysmon"
#define OUT_FILE     OUT_DIR "/mem.dat"
#define TMP_FILE     OUT_DIR "/mem.dat.tmp"
#define INTERVAL     2   /* seconds between samples */

struct mem_stats {
    unsigned long mem_total;
    unsigned long mem_free;
    unsigned long mem_available;
    unsigned long swap_total;
    unsigned long swap_free;
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
    unsigned long value;
    int found = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Each line looks like: "MemTotal:       16318412 kB" */
        if (sscanf(line, "%63[^:]: %lu", key, &value) != 2)
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

/* Write the stats to a temp file, then rename it over mem.dat.
 * rename() is atomic, so sysmond never reads a half-written file. */
static int write_stats(const struct mem_stats *stats)
{
    FILE *fp = fopen(TMP_FILE, "w");
    if (fp == NULL) {
        perror("fopen " TMP_FILE);
        return -1;
    }

    fprintf(fp, "MemTotal: %lu kB\n", stats->mem_total);
    fprintf(fp, "MemFree: %lu kB\n", stats->mem_free);
    fprintf(fp, "MemAvailable: %lu kB\n", stats->mem_available);
    fprintf(fp, "SwapTotal: %lu kB\n", stats->swap_total);
    fprintf(fp, "SwapFree: %lu kB\n", stats->swap_free);

    if (fclose(fp) != 0) {
        perror("fclose " TMP_FILE);
        return -1;
    }

    if (rename(TMP_FILE, OUT_FILE) != 0) {
        perror("rename " TMP_FILE);
        return -1;
    }
    return 0;
}

int main(void)
{
    /* The sensor may start before sysmond, so make sure the directory exists. */
    if (mkdir(OUT_DIR, 0755) != 0 && errno != EEXIST) {
        perror("mkdir " OUT_DIR);
        return EXIT_FAILURE;
    }

    struct mem_stats stats;

    while (1) {
        if (read_meminfo(&stats) == 0)
            write_stats(&stats);
        sleep(INTERVAL);
    }

    return EXIT_SUCCESS;
}
