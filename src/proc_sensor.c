/*
 * proc_sensor - thread that counts running processes by scanning /proc for
 * numeric directory names (one per PID) and stores the count in the shared
 * state every SAMPLE_INTERVAL seconds.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>

#include <sysmon.h>

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

    /* readdir() is safe here: each thread uses its own DIR stream. */
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && is_number(entry->d_name))
            count++;
    }

    closedir(dir);
    return count;
}

void *proc_sensor_thread(void *arg)
{
    sysmon_set_thread_name((const char *)arg);

    while (sysmon_running()) {
        int processes = count_processes();
        if (processes >= 0) {
            pthread_mutex_lock(&g_state.lock);
            g_state.total_procs = processes;
            g_state.have_procs  = 1;
            pthread_mutex_unlock(&g_state.lock);
        }
        sysmon_sleep(SAMPLE_INTERVAL);
    }
    return NULL;
}
