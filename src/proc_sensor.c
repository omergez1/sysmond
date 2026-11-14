/*
 * proc_sensor - counts running processes by scanning /proc for numeric
 * directory names (one per PID) and writes the count to /tmp/sysmon/proc.dat
 * every INTERVAL seconds.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROC_PATH "/proc"
#define OUT_DIR   "/tmp/sysmon"
#define OUT_FILE  OUT_DIR "/proc.dat"
#define TMP_FILE  OUT_DIR "/proc.dat.tmp"
#define INTERVAL  2   /* seconds between samples */

/* Return 1 if the string is made only of digits (like "1234"), else 0. */
static int is_number(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

/* Count the entries in /proc whose names are numbers. Each one is a
 * directory for a running process, named after its PID.
 * Returns the count, or -1 on failure. */
static int count_processes(void)
{
    DIR *dir = opendir(PROC_PATH);
    if (dir == NULL) {
        perror("opendir " PROC_PATH);
        return -1;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && is_number(entry->d_name))
            count++;
    }

    closedir(dir);
    return count;
}

/* Write the count to a temp file, then rename it over proc.dat.
 * rename() is atomic, so sysmond never reads a half-written file. */
static int write_stats(int processes)
{
    FILE *fp = fopen(TMP_FILE, "w");
    if (fp == NULL) {
        perror("fopen " TMP_FILE);
        return -1;
    }

    fprintf(fp, "Processes: %d\n", processes);

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
    /* mkdir also says "already exists" when a regular file has that name. */
    struct stat st;
    if (stat(OUT_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "proc_sensor: " OUT_DIR " exists but is not a directory\n");
        return EXIT_FAILURE;
    }

    while (1) {
        int processes = count_processes();
        if (processes >= 0)
            write_stats(processes);
        sleep(INTERVAL);
    }

    return EXIT_SUCCESS;
}
