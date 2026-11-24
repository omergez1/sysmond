/*
 * cpu_sensor - reads the 1, 5 and 15 minute load averages from /proc/loadavg
 * every CPU_INTERVAL seconds and writes them as a binary CpuData struct
 * to the named pipe CPU_FIFO_PATH, which sysmond creates and reads.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sysmon_ipc.h>

#define LOADAVG_PATH "/proc/loadavg"

/* Read /proc/loadavg. Its single line looks like:
 *   "0.40 0.28 0.20 3/1092 5850"
 * The first three numbers are the 1, 5 and 15 minute load averages.
 * Returns 0 on success, -1 on failure. */
static int read_loadavg(CpuData *data)
{
    FILE *fp = fopen(LOADAVG_PATH, "r");
    if (fp == NULL) {
        perror("fopen " LOADAVG_PATH);
        return -1;
    }

    char line[128];
    int ok = fgets(line, sizeof(line), fp) != NULL &&
             sscanf(line, "%lf %lf %lf",
                    &data->load1, &data->load5, &data->load15) == 3;

    fclose(fp);

    if (!ok) {
        fprintf(stderr, "cpu_sensor: could not parse " LOADAVG_PATH "\n");
        return -1;
    }
    return 0;
}

/* Write all len bytes, retrying on partial writes and EINTR.
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

int main(void)
{
    /* Opening a FIFO for writing waits until someone opens it for reading
     * (sysmond does, right after starting the sensors). No O_CREAT: if the
     * FIFO doesn't exist, fail instead of creating a regular file. */
    int fd = open(CPU_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        perror("cpu_sensor: open " CPU_FIFO_PATH);
        return EXIT_FAILURE;
    }

    CpuData data;
    while (1) {
        if (read_loadavg(&data) == 0) {
            /* No reader left (sysmond exited) -> SIGPIPE ends this process. */
            if (write_full(fd, &data, sizeof(data)) != 0) {
                perror("cpu_sensor: write");
                close(fd);
                return EXIT_FAILURE;
            }
        }
        sleep(CPU_INTERVAL);
    }
}
