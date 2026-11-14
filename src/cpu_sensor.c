/*
 * cpu_sensor - reads the 1, 5 and 15 minute load averages from
 * /proc/loadavg and writes them to /tmp/sysmon/cpu.dat every INTERVAL seconds.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOADAVG_PATH "/proc/loadavg"
#define OUT_DIR      "/tmp/sysmon"
#define OUT_FILE     OUT_DIR "/cpu.dat"
#define TMP_FILE     OUT_DIR "/cpu.dat.tmp"
#define INTERVAL     2   /* seconds between samples */

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

/* Write the stats to a temp file, then rename it over cpu.dat.
 * rename() is atomic, so sysmond never reads a half-written file. */
static int write_stats(const struct cpu_stats *stats)
{
    FILE *fp = fopen(TMP_FILE, "w");
    if (fp == NULL) {
        perror("fopen " TMP_FILE);
        return -1;
    }

    fprintf(fp, "Load1: %.2f\n", stats->load1);
    fprintf(fp, "Load5: %.2f\n", stats->load5);
    fprintf(fp, "Load15: %.2f\n", stats->load15);

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

    struct cpu_stats stats;

    while (1) {
        if (read_loadavg(&stats) == 0)
            write_stats(&stats);
        sleep(INTERVAL);
    }

    return EXIT_SUCCESS;
}
