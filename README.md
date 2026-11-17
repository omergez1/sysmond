# sysmon

A small Linux system monitor written in C. `sysmond` samples memory, CPU load and process statistics from `/proc` and prints a live status report in the terminal. If the CPU load spikes, it prints an alert.

This is a course project built in phases:

| Version | Phase | Design |
|---------|-------|--------|
| **v2.0** (current) | 2 | One multi-threaded program. Threads share an in-memory struct protected by a mutex, and a condition variable wakes the alert thread. See [sysmond-phase-2-guide.md](sysmond-phase-2-guide.md). |
| v1.0 | 1 | Separate sensor processes write `/tmp/sysmon/*.dat` files, and `sysmond` starts them with `fork` + `exec` and reads the files. See [sysmond-phase-1-guide.md](sysmond-phase-1-guide.md). |

To look at or build v1.0: `git checkout v1.0` (return with `git switch main`).

## How it works

```
                       sysmond (one process)
  ┌──────────────────────────────────────────────────────────────┐
  │  mem_sensor  ──┐  lock, update, unlock                       │
  │  cpu_sensor  ──┼──────────────────────>  SystemState g_state │
  │  proc_sensor ──┘                         (mutex: lock)       │
  │       │                                        │             │
  │       │ load1 > 1.50: pthread_cond_signal      │ lock, copy, │
  │       v                                        │ unlock      │
  │  alert ── waits on alert_cond,                 v             │
  │           prints alert to stderr          main thread:       │
  │                                           redraws the report │
  │                                           every 2 s          │
  └──────────────────────────────────────────────────────────────┘
```

| Thread        | Job |
|---------------|-----|
| `mem_sensor`  | Reads `/proc/meminfo` (MemTotal, MemFree, MemAvailable, SwapTotal, SwapFree) |
| `cpu_sensor`  | Reads `/proc/loadavg` (1, 5 and 15 minute load). Signals `alert` when the 1-minute load goes above 1.50 |
| `proc_sensor` | Counts the numeric folders in `/proc` (one per running process) |
| `alert`       | Sleeps in `pthread_cond_wait` until signaled, then prints an alert to stderr |
| main          | Starts the threads, redraws the report every 2 s, and shuts everything down on Ctrl+C |

**Thread-safety rules used throughout:**
- Every read or write of `g_state` happens between `pthread_mutex_lock` and `pthread_mutex_unlock`.
- Slow work, like reading `/proc` or printing, happens *outside* the lock. Sensors read into local variables and then lock only to copy the values in. The main thread copies a snapshot under the lock and prints after unlocking.
- `pthread_cond_wait` is always called in a loop that checks a flag (`alert_pending`). This protects against wakeups with no signal ("spurious wakeups") and against signals sent before the thread started waiting.
- SIGINT and SIGTERM are blocked in every thread and collected only by the main thread with `sigtimedwait`, so a signal never interrupts a worker at a random point.

**Alerts happen once per spike:** an alert fires when the 1-minute load *crosses* above 1.50. It fires again only after the load drops back to 1.50 or below and then rises again. While the load is high, the report also shows a red `HIGH LOAD` line.

## Building

Requires `gcc` and `make` on Linux.

```bash
make          # builds bin/sysmond (object files go in obj/)
make clean    # removes bin/ contents and obj/
```

The alert threshold can be changed when building, for example to test the alert without creating CPU load:
```bash
gcc -Wall -g -Iinc -pthread -DALERT_LOAD_THRESHOLD=0.0 src/*.c -o bin/sysmond_test
```

## Running

```bash
./bin/sysmond          # Ctrl+C to stop
```

In a terminal, the screen is redrawn in place with colors. When the output goes to a file or a pipe, it's plain text with no color codes.

```
========== sysmon v2.0 report 2026-11-17 20:18:11 ==========
Memory
  Total:        7481.0 MB
  Used:         3607.1 MB (48.2%)
  Free:         1179.2 MB
  Available:    3873.9 MB
  Swap:          262.5 / 4916.0 MB used (5.3%)
CPU load
  1 min: 1.84   5 min: 0.87   15 min: 0.54
  HIGH LOAD: 1-min load is above 1.50
Processes
  Running: 364
Alerts
  Last alert: 20:18:52 (load 1.84)
```

The alert itself goes to stderr:
```
*** ALERT 20:18:52: high CPU load 1.84 (threshold 1.50) ***
```

To trigger it with real load, run a few busy loops for a minute, then stop them:
```bash
for i in 1 2 3 4; do yes > /dev/null & done     # wait ~30-60 s for the 1-min load to rise
kill %1 %2 %3 %4
```

## Checking that it works

```bash
ps -T -p $(pgrep -x sysmond)    # 5 threads: sysmond, mem_sensor, cpu_sensor, proc_sensor, alert
free -m                         # compare with the Memory section
cat /proc/loadavg               # compare with CPU load
ls /proc | grep -c '^[0-9]*$'   # compare with Processes
valgrind --tool=helgrind ./bin/sysmond   # checks for data races (Ctrl+C to stop)
```

## v1 vs v2

Both versions measured on the same machine at the same time:

| | v1.0 (processes + files) | v2.0 (threads + shared memory) |
|---|---|---|
| Processes | 4 | 1 (5 threads) |
| Memory (RSS) | 6.7 MB total | 1.8 MB |
| System calls in 20 s | 718 | 347 |
| CPU time per minute | ~0.01 s | ~0.01 s |
| Files written | 3 `.dat` files every 2 s | none |
| A sensor can crash alone | yes | no (a crash takes the whole program down) |

## Project layout

```
.
├── Makefile
├── README.md
├── sysmond-phase-1-guide.md
├── sysmond-phase-2-guide.md
├── inc/
│   └── sysmon.h        # SystemState, shared declarations, version, threshold
├── src/
│   ├── sysmond.c       # main thread: setup, report, shutdown
│   ├── mem_sensor.c    # mem_sensor_thread
│   ├── cpu_sensor.c    # cpu_sensor_thread + alert trigger
│   ├── proc_sensor.c   # proc_sensor_thread
│   └── alert.c         # alert_thread
├── obj/                # compiled .o files (not tracked by git)
└── bin/                # sysmond (not tracked by git)
```
