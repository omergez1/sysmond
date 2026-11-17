/*
 * alert - thread that sleeps on a condition variable until the CPU sensor
 * reports a load spike, then prints an alert to stderr.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sysmon.h>

void *alert_thread(void *arg)
{
    sysmon_set_thread_name((const char *)arg);
    int color = isatty(STDERR_FILENO);

    while (1) {
        pthread_mutex_lock(&g_state.lock);

        /* pthread_cond_wait unlocks the mutex while it sleeps and locks it
         * again before returning. Always wait in a loop that checks the
         * condition: a wakeup does not guarantee the condition is true. */
        while (!g_state.alert_pending && g_state.running)
            pthread_cond_wait(&g_state.alert_cond, &g_state.lock);

        if (!g_state.running) {          /* woken for shutdown */
            pthread_mutex_unlock(&g_state.lock);
            break;
        }

        double load = g_state.last_alert_load;
        time_t now = time(NULL);
        g_state.alert_pending = 0;
        g_state.last_alert = now;
        pthread_mutex_unlock(&g_state.lock);

        /* Print after unlocking: never do slow I/O while holding the lock. */
        char stamp[16];
        struct tm tm;
        strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime_r(&now, &tm));
        fprintf(stderr, "%s*** ALERT %s: high CPU load %.2f (threshold %.2f) ***%s\n",
                color ? "\033[1;31m" : "", stamp, load,
                ALERT_LOAD_THRESHOLD, color ? "\033[0m" : "");
    }
    return NULL;
}
