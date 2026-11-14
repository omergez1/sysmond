/*
 * sysmond - central daemon. Every INTERVAL seconds it reads the files the
 * sensors write in /tmp/sysmon/ and prints one combined status report.
 *
 * Each sensor file holds lines of the form "Key: value", for example:
 *   MemTotal: 7660516 kB      (mem.dat)
 *   Load1: 0.61               (cpu.dat)
 *   Processes: 360            (proc.dat)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DATA_DIR   "/tmp/sysmon"
#define MEM_FILE   DATA_DIR "/mem.dat"
#define CPU_FILE   DATA_DIR "/cpu.dat"
#define PROC_FILE  DATA_DIR "/proc.dat"
#define INTERVAL   2                /* seconds between reports */
#define STALE_AFTER (3 * INTERVAL)  /* data older than this is flagged */

/* The contents of one sensor file, read in a single pass. */
struct sensor_file {
    int    found;       /* 1 if the file could be opened */
    time_t age;         /* seconds since the sensor last wrote it */
    char   text[1024];  /* the whole file */
};

/* Read a whole sensor file into f->text and record how old it is.
 * Reading it once (rather than reopening it for every value) means all the
 * values in one report come from the same sample. */
static void load_file(const char *path, struct sensor_file *f)
{
    memset(f, 0, sizeof(*f));

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return;                     /* sensor not started yet */

    size_t n = fread(f->text, 1, sizeof(f->text) - 1, fp);
    f->text[n] = '\0';

    struct stat st;
    if (fstat(fileno(fp), &st) == 0)
        f->age = time(NULL) - st.st_mtime;

    fclose(fp);
    f->found = 1;
}

/* Find "key: value" in the file text and store the value in *out.
 * Returns 1 if the key was found, 0 if not. */
static int get_value(const struct sensor_file *f, const char *key, double *out)
{
    if (!f->found)
        return 0;

    const char *line = f->text;
    char name[64];
    double value;

    while (*line != '\0') {
        if (sscanf(line, "%63[^:]: %lf", name, &value) == 2 &&
            strcmp(name, key) == 0) {
            *out = value;
            return 1;
        }
        const char *next = strchr(line, '\n');
        if (next == NULL)
            break;
        line = next + 1;
    }
    return 0;
}

/* Print a section heading, with a warning if the data is missing or old. */
static void print_heading(const char *title, const struct sensor_file *f,
                          const char *sensor)
{
    printf("%s\n", title);
    if (!f->found)
        printf("  (no data - is %s running?)\n", sensor);
    else if (f->age > STALE_AFTER)
        printf("  (STALE: last update %lds ago - is %s still running?)\n",
               (long)f->age, sensor);
}

static double kb_to_mb(double kb)
{
    return kb / 1024.0;
}

static void print_memory(const struct sensor_file *f)
{
    print_heading("Memory", f, "mem_sensor");
    if (!f->found)
        return;

    double total, free, avail, swap_total, swap_free;
    if (!get_value(f, "MemTotal", &total) ||
        !get_value(f, "MemFree", &free) ||
        !get_value(f, "MemAvailable", &avail) ||
        !get_value(f, "SwapTotal", &swap_total) ||
        !get_value(f, "SwapFree", &swap_free)) {
        printf("  (could not parse " MEM_FILE ")\n");
        return;
    }

    /* "Used" here means memory that is not available to new programs. */
    double used = total - avail;
    double swap_used = swap_total - swap_free;

    printf("  Total:     %9.1f MB\n", kb_to_mb(total));
    printf("  Used:      %9.1f MB (%.1f%%)\n", kb_to_mb(used),
           total > 0 ? 100.0 * used / total : 0.0);
    printf("  Free:      %9.1f MB\n", kb_to_mb(free));
    printf("  Available: %9.1f MB\n", kb_to_mb(avail));
    printf("  Swap:      %9.1f / %.1f MB used (%.1f%%)\n",
           kb_to_mb(swap_used), kb_to_mb(swap_total),
           swap_total > 0 ? 100.0 * swap_used / swap_total : 0.0);
}

static void print_cpu(const struct sensor_file *f)
{
    print_heading("CPU load", f, "cpu_sensor");
    if (!f->found)
        return;

    double load1, load5, load15;
    if (!get_value(f, "Load1", &load1) ||
        !get_value(f, "Load5", &load5) ||
        !get_value(f, "Load15", &load15)) {
        printf("  (could not parse " CPU_FILE ")\n");
        return;
    }

    printf("  1 min: %.2f   5 min: %.2f   15 min: %.2f\n",
           load1, load5, load15);
}

static void print_processes(const struct sensor_file *f)
{
    print_heading("Processes", f, "proc_sensor");
    if (!f->found)
        return;

    double count;
    if (!get_value(f, "Processes", &count)) {
        printf("  (could not parse " PROC_FILE ")\n");
        return;
    }

    printf("  Running: %.0f\n", count);
}

static void print_report(void)
{
    struct sensor_file mem, cpu, proc;
    load_file(MEM_FILE, &mem);
    load_file(CPU_FILE, &cpu);
    load_file(PROC_FILE, &proc);

    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    printf("========== sysmon report %s ==========\n", stamp);
    print_memory(&mem);
    print_cpu(&cpu);
    print_processes(&proc);
    printf("\n");

    /* Without this, output sits in a buffer when stdout is a file or pipe. */
    fflush(stdout);
}

int main(void)
{
    /* Same as "mkdir -p /tmp/sysmon": create it, and ignore "already exists". */
    if (mkdir(DATA_DIR, 0755) != 0 && errno != EEXIST) {
        perror("mkdir " DATA_DIR);
        return EXIT_FAILURE;
    }
    /* mkdir also says "already exists" when a regular file has that name. */
    struct stat st;
    if (stat(DATA_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sysmond: " DATA_DIR " exists but is not a directory\n");
        return EXIT_FAILURE;
    }

    while (1) {
        print_report();
        sleep(INTERVAL);
    }

    return EXIT_SUCCESS;
}
