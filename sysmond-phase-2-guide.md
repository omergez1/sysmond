# Lab 2: Multi-Threaded Refactoring & Thread Synchronization (`pthreads`)

## Background
In Phase 1 of the `sysmon` project, you built a multi-process monitoring suite where independent sensor executables wrote metric snapshots to disk files, and `sysmond` polled those files. While process isolation provides robustness, disk I/O introduces latency, overhead, and complexity.

In Phase 2, you will refactor `sysmon` into a **multi-threaded application**. All sensor tasks will run as concurrent threads inside a single `sysmond` daemon, sharing an in-memory state structure. You will use POSIX Threads (`pthreads`), protect shared memory using Mutexes, and synchronize asynchronous events using Condition Variables.

---

## Lab Overview & Objectives
You will consolidate the project into a single multi-threaded binary (`sysmond`).

Upon completing this lab, you will be able to:
* Spawn and manage concurrent POSIX threads using `pthread_create()` and `pthread_join()`.
* Pass data arguments safely into thread start routines.
* Prevent race conditions on shared data structures using a `pthread_mutex_t`.
* Implement signal-driven thread synchronization using `pthread_cond_wait()` and `pthread_cond_signal()`.
* Compare memory-sharing performance vs file-based IPC.

---

## Technical Architecture

```
+-------------------------------------------------------------------------------+
|                             sysmond Process Space                             |
|                                                                               |
|  +--------------------+     Locks & Updates      +-------------------------+  |
|  | mem_sensor_thread  | -----------------------> |                         |  |
|  +--------------------+                          |   struct SystemState    |  |
|  +--------------------+                          |   (Shared Data Struct)  |  |
|  | cpu_sensor_thread  | -----------------------> |                         |  |
|  +--------------------+                          |  pthread_mutex_t lock   |  |
|  +--------------------+                          +-------------------------+  |
|  | proc_sensor_thread | ----------------------->             |                |
|  +--------------------+                                      | Signals        |
|  +--------------------+     Waits on Condition               v                |
|  |   alert_thread     | <------------------------- pthread_cond_t signal  |
|  +--------------------+                                                       |
|  +--------------------+     Reads Shared Data Every 2 Seconds                 |
|  |    main thread     | --------------------------------------------------->  |
|  +--------------------+                                                       |
+-------------------------------------------------------------------------------+
```

### 1. Shared State Structure (`struct SystemState`)
All threads share a single global structure containing the latest telemetry metrics:

```c
typedef struct {
    // Memory Metrics
    long mem_total;
    long mem_free;
    long mem_avail;
    long swap_total;
    long swap_free;

    // CPU Load Metrics
    double load1;
    double load5;
    double load15;

    // Process Metrics
    int total_procs;

    // Synchronization
    pthread_mutex_t lock;
    pthread_cond_t alert_cond;
} SystemState;
```

### 2. Thread Roles
* **`mem_sensor_thread`:** Periodically reads `/proc/meminfo`, acquires `lock`, updates memory fields, and releases `lock`.
* **`cpu_sensor_thread`:** Periodically reads `/proc/loadavg`, acquires `lock`, updates load fields, releases `lock`, and triggers `pthread_cond_signal()` if `load1 > 1.50`.
* **`proc_sensor_thread`:** Periodically scans `/proc`, acquires `lock`, updates `total_procs`, and releases `lock`.
* **`alert_thread`:** Blocks on `pthread_cond_wait()`. When woken up by the CPU sensor, it acquires the mutex and prints an urgent system alert to `stderr`.
* **`main` thread:** Spawns all worker threads, loops indefinitely to render the formatted ANSI terminal UI every 2 seconds, and cleanly joins threads on shutdown.

---

## Student Instructions

### 1. Project Restructuring
* Combine your Phase 1 source code using 'extern' functions and a shared header `inc/sysmon.h`.
* Include headers without path prefixes (e.g., `#include <sysmon.h>`).
* Update your `Makefile` to output a single binary `bin/sysmond`, include `-Iinc`, and append the `-pthread` linking flag to `CFLAGS`/`LDFLAGS`.

### 2. Thread Implementation & Synchronization
* Define thread routines matching the POSIX prototype: `void *thread_func(void *arg)`.
* Initialize the mutex (`pthread_mutex_init`) and condition variable (`pthread_cond_init`) in `main()` before spawning threads.
* Wrap all reads and writes to `SystemState` inside critical sections using `pthread_mutex_lock()` and `pthread_mutex_unlock()`.

### 3. Verification & Testing
* Compile the project using `make`.
* Run `./bin/sysmond`.
* Verify that telemetry metrics update continuously without intermediate `/tmp/sysmon/*.dat` files.
* Test the Alert Thread by artificially generating CPU load (e.g., running `stress --cpu 2` or a dummy busy-wait loop) to observe the conditional alert trigger.