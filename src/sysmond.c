/*
 * sysmond - central daemon. On startup it launches the three sensors as
 * child processes (fork + exec). Then, every INTERVAL seconds, it reads the
 * files the sensors write in /tmp/sysmon/ and prints one combined report.
 * If a sensor dies, sysmond keeps running and reports it as stopped.
 * On Ctrl+C or kill, it stops all its sensors before exiting.
 *
 * Each sensor file holds lines of the form "Key: value", for example:
 *   MemTotal: 7660516 kB      (mem.dat)
 *   Load1: 0.61               (cpu.dat)
 *   Processes: 360            (proc.dat)
 */

#include <errno.h>
#include <limits.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DATA_DIR   "/tmp/sysmon"
#define MEM_FILE   DATA_DIR "/mem.dat"
#define CPU_FILE   DATA_DIR "/cpu.dat"
#define PROC_FILE  DATA_DIR "/proc.dat"
#define INTERVAL   2                /* seconds between reports */
#define STALE_AFTER (3 * INTERVAL)  /* data older than this is flagged */

/* ---------- child process management ---------- */

/* One entry per sensor that sysmond launches. */
struct sensor {
    const char *name;    /* program name in the same folder as sysmond */
    pid_t       pid;     /* child PID, or 0 if it was never started */
    int         alive;   /* 1 while the child is running */
    int         status;  /* waitpid() status, valid once alive == 0 */
};

static struct sensor sensors[] = {
    { "mem_sensor",  0, 0, 0 },
    { "cpu_sensor",  0, 0, 0 },
    { "proc_sensor", 0, 0, 0 },
};
#define NUM_SENSORS (sizeof(sensors) / sizeof(sensors[0]))

/* Set by the signal handler. "volatile sig_atomic_t" is the only type that
 * is safe to write from a signal handler and read from normal code. */
static volatile sig_atomic_t stop_requested = 0;

static void on_stop_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* Build the full path of a sensor program. The sensors live in the same
 * folder as sysmond, so this works no matter which folder you run it from.
 * /proc/self/exe is a link to the program that is currently running. */
static int sensor_path(const char *name, char *out, size_t size)
{
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) {
        perror("readlink /proc/self/exe");
        return -1;
    }
    self[n] = '\0';

    /* dirname() turns ".../bin/sysmond" into ".../bin" (in place). */
    int len = snprintf(out, size, "%s/%s", dirname(self), name);
    if (len < 0 || (size_t)len >= size) {
        fprintf(stderr, "sysmond: path to %s is too long\n", name);
        return -1;
    }
    return 0;
}

/* fork() a child for each sensor and exec() the sensor program in it. */
static void start_sensors(void)
{
    for (size_t i = 0; i < NUM_SENSORS; i++) {
        char path[PATH_MAX];
        if (sensor_path(sensors[i].name, path, sizeof(path)) != 0)
            continue;

        /* Flush first, or text still waiting in our stdout buffer would be
         * copied into the child too. */
        fflush(stdout);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }

        if (pid == 0) {
            /* Child: replace this process with the sensor program. */
            char *argv[] = { (char *)sensors[i].name, NULL };
            execv(path, argv);
            /* execv only returns if it failed. */
            perror(path);
            _exit(127);   /* _exit: don't run the parent's cleanup code */
        }

        /* Parent: remember the child. */
        sensors[i].pid = pid;
        sensors[i].alive = 1;
        printf("sysmond: started %s (pid %d)\n", sensors[i].name, (int)pid);
    }
    fflush(stdout);
}

/* Collect any sensors that have exited, without blocking. A child that has
 * exited stays a "zombie" until its parent calls waitpid() on it. */
static void reap_sensors(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (size_t i = 0; i < NUM_SENSORS; i++) {
            if (sensors[i].pid == pid) {
                sensors[i].alive = 0;
                sensors[i].status = status;
            }
        }
    }
}

/* Describe how a child ended, e.g. "exited with code 127". */
static void describe_exit(int status, char *out, size_t size)
{
    if (WIFEXITED(status))
        snprintf(out, size, "exited with code %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(out, size, "killed by signal %d (%s)",
                 WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        snprintf(out, size, "stopped");
}

/* Ask every running sensor to stop, then wait for each one so none is
 * left behind as an orphan or a zombie. */
static void stop_sensors(void)
{
    for (size_t i = 0; i < NUM_SENSORS; i++) {
        if (sensors[i].alive)
            kill(sensors[i].pid, SIGTERM);
    }
    for (size_t i = 0; i < NUM_SENSORS; i++) {
        if (sensors[i].alive) {
            waitpid(sensors[i].pid, NULL, 0);
            sensors[i].alive = 0;
        }
    }
}

/* ---------- reading and printing sensor data ---------- */

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

/* Find a sensor in the table by name, or NULL. */
static const struct sensor *find_sensor(const char *name)
{
    for (size_t i = 0; i < NUM_SENSORS; i++) {
        if (strcmp(sensors[i].name, name) == 0)
            return &sensors[i];
    }
    return NULL;
}

/* Print a section heading, with a warning if the sensor has died or the
 * data is missing or old. */
static void print_heading(const char *title, const struct sensor_file *f,
                          const char *sensor)
{
    printf("%s\n", title);

    const struct sensor *s = find_sensor(sensor);
    if (s != NULL && s->pid != 0 && !s->alive) {
        char how[96];
        describe_exit(s->status, how, sizeof(how));
        if (f->found)
            printf("  (SENSOR DOWN: %s %s - last data from %lds ago)\n",
                   sensor, how, (long)f->age);
        else
            printf("  (SENSOR DOWN: %s %s - no data)\n", sensor, how);
    } else if (!f->found) {
        printf("  (no data - is %s running?)\n", sensor);
    } else if (f->age > STALE_AFTER) {
        printf("  (STALE: last update %lds ago - is %s still running?)\n",
               (long)f->age, sensor);
    }
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

    /* Ctrl+C sends SIGINT, "kill" sends SIGTERM. Instead of dying right
     * away, set a flag so the loop below ends and we can stop the sensors. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    start_sensors();
    sleep(1);   /* give the sensors a moment to write their first sample */

    /* A signal wakes sleep() early, so Ctrl+C is handled right away. */
    while (!stop_requested) {
        reap_sensors();
        print_report();
        sleep(INTERVAL);
    }

    printf("sysmond: stopping sensors...\n");
    stop_sensors();
    printf("sysmond: done\n");
    return EXIT_SUCCESS;
}
