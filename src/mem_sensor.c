/*
 * mem_sensor - reads memory metrics from /proc/meminfo every SAMPLE_INTERVAL
 * seconds and writes them as a binary MemData struct to a file descriptor.
 *
 * Usage: mem_sensor [fd]
 *   fd  the write end of the anonymous pipe that sysmond created and passed
 *       in. Defaults to standard output (fd 1), which lets you test the
 *       sensor by hand:  ./bin/mem_sensor | xxd
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sysmon_ipc.h>

#define MEMINFO_PATH "/proc/meminfo"

/* Read /proc/meminfo into *data (values in kB).
 * Returns 0 on success, -1 on failure. */
static int read_meminfo(MemData *data)
{
    FILE *fp = fopen(MEMINFO_PATH, "r");
    if (fp == NULL) {
        perror("fopen " MEMINFO_PATH);
        return -1;
    }

    memset(data, 0, sizeof(*data));

    char line[256];
    char key[64];
    long value;
    int found = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Each line looks like: "MemTotal:       16318412 kB" */
        if (sscanf(line, "%63[^:]: %ld", key, &value) != 2)
            continue;

        if (strcmp(key, "MemTotal") == 0) {
            data->mem_total = value;
            found++;
        } else if (strcmp(key, "MemFree") == 0) {
            data->mem_free = value;
            found++;
        } else if (strcmp(key, "MemAvailable") == 0) {
            data->mem_avail = value;
            found++;
        } else if (strcmp(key, "SwapTotal") == 0) {
            data->swap_total = value;
            found++;
        } else if (strcmp(key, "SwapFree") == 0) {
            data->swap_free = value;
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

/* write() may write fewer bytes than asked, or be interrupted by a signal
 * (EINTR). Keep going until everything is written.
 * Returns 0 on success, -1 on error. */
static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Parse the fd argument. Returns the fd, or -1 if it isn't a valid number. */
static int parse_fd(const char *s)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > INT_MAX)
        return -1;
    return (int)v;
}

int main(int argc, char *argv[])
{
    int fd = STDOUT_FILENO;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [fd]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        fd = parse_fd(argv[1]);
        if (fd < 0) {
            fprintf(stderr, "mem_sensor: invalid fd '%s'\n", argv[1]);
            return EXIT_FAILURE;
        }
    }
    /* F_GETFD fails if the descriptor isn't open. */
    if (fcntl(fd, F_GETFD) == -1) {
        fprintf(stderr, "mem_sensor: fd %d is not open\n", fd);
        return EXIT_FAILURE;
    }

    MemData data;
    while (1) {
        if (read_meminfo(&data) == 0) {
            /* If sysmond has exited, the pipe has no reader and this write
             * raises SIGPIPE, which ends this process. That's what we want:
             * a sensor should not outlive its supervisor. */
            if (write_full(fd, &data, sizeof(data)) != 0) {
                perror("mem_sensor: write");
                return EXIT_FAILURE;
            }
        }
        sleep(SAMPLE_INTERVAL);
    }
}
