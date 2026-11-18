/*
 * proc_sensor - counts running processes (numeric directories in /proc, one
 * per PID) every SAMPLE_INTERVAL seconds and writes the count as a binary
 * ProcData struct to the named pipe PROC_FIFO_PATH, which sysmond creates
 * and reads.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sysmon_ipc.h>

#define PROC_PATH "/proc"

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
    /* Waits until sysmond opens the FIFO for reading. No O_CREAT: if the
     * FIFO doesn't exist, fail instead of creating a regular file. */
    int fd = open(PROC_FIFO_PATH, O_WRONLY);
    if (fd < 0) {
        perror("proc_sensor: open " PROC_FIFO_PATH);
        return EXIT_FAILURE;
    }

    ProcData data;
    while (1) {
        data.total_procs = count_processes();
        if (data.total_procs >= 0) {
            /* No reader left (sysmond exited) -> SIGPIPE ends this process. */
            if (write_full(fd, &data, sizeof(data)) != 0) {
                perror("proc_sensor: write");
                close(fd);
                return EXIT_FAILURE;
            }
        }
        sleep(SAMPLE_INTERVAL);
    }
}
