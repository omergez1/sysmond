# Phase 3: Supervised Multi-Process IPC (Pipes, FIFOs & `fork`/`execve`)

## Background
In Phase 1 of the `sysmon` project, independent sensor executables wrote metric snapshots to disk files, and `sysmond` polled those files. In Phase 2, you consolidated everything into a single multi-threaded process using shared memory.

In Phase 3, you return to a **multi-process architecture** with strict process isolation, replacing disk-file I/O with real Linux Inter-Process Communication (IPC): **Anonymous Pipes** (`pipe()`) and **Named Pipes / FIFOs** (`mkfifo()`). Rather than launching sensors manually from the shell, `sysmond` will act as a **process supervisor**, spawning and managing the lifecycle of all sensor processes using `fork()` and `execve()`.

---

## Lab Overview & Objectives
You will build four independent executables (`sysmond`, `mem_sensor`, `cpu_sensor`, and `proc_sensor`), where `sysmond` launches and coordinates the three sensors automatically.

Upon completing this lab, you will be able to:
* Spawn child processes and replace process images using `fork()` and `execve()`.
* Create unidirectional anonymous pipes using `pipe()` and pass inherited file descriptors to child executables.
* Create and manage file-system named pipes (FIFOs) using `mkfifo()` and standard file descriptors (`open()`, `read()`, `write()`).
* Transmit raw binary C structures across IPC streams to eliminate text parsing overhead.
* Handle termination signals (`SIGINT`, `SIGTERM`), cleanly terminate child processes, reap zombie processes via `waitpid()`, and unlink FIFO nodes on exit.

---

## Technical Architecture & IPC Flow

```
+-----------------------------------------------------------------------------------+
|                        sysmond (Supervisor & IPC Receiver)                        |
|                                                                                   |
|  fork()/execve()           fork()/execve()                fork()/execve()         |
|  Reads pipefd[0]           Reads cpu.fifo                 Reads proc.fifo         |
+-------^--------------------------^------------------------------^-----------------+
        |                          |                              |
  Anonymous Pipe              Named Pipe (FIFO)              Named Pipe (FIFO)
  (Inherited fd)            (/tmp/sysmon_cpu.fifo)         (/tmp/sysmon_proc.fifo)
        |                          |                              |
+-------+-----------+      +-------+-----------+          +-------+-----------+
|    mem_sensor     |      |    cpu_sensor     |          |    proc_sensor    |
|  (Child Process)  |      |  (Child Process)  |          |  (Child Process)  |
+-------------------+      +-------------------+          +-------------------+
```

---

## Telemetry Data Structures (`inc/sysmon_ipc.h`)

Instead of formatting text files, sensors transmit packed binary C structures directly across IPC channels:

```c
#ifndef SYSMON_IPC_H
#define SYSMON_IPC_H

#define CPU_FIFO_PATH  "/tmp/sysmon_cpu.fifo"
#define PROC_FIFO_PATH "/tmp/sysmon_proc.fifo"

typedef struct {
    long mem_total;
    long mem_free;
    long mem_avail;
    long swap_total;
    long swap_free;
} MemData;

typedef struct {
    double load1;
    double load5;
    double load15;
} CpuData;

typedef struct {
    int total_procs;
} ProcData;

#endif /* SYSMON_IPC_H */
```

---

## Student Instructions

### 1. Project & Build Setup
* Place the shared IPC header in `inc/sysmon_ipc.h`.
* Place your source files in `src/` (`sysmond.c`, `mem_sensor.c`, `cpu_sensor.c`, `proc_sensor.c`).
* Configure your root `Makefile` using `-Iinc` so all source files can include `<sysmon_ipc.h>` directly without relative paths, and compile all four binaries into `bin/`.

### 2. Sensor Implementation
* **`src/mem_sensor.c` (Anonymous Pipe):**
  * Accept the target write file descriptor via `argv[1]` (defaulting to `STDOUT_FILENO` if omitted).
  * Read `/proc/meminfo` every 1 second, populate a `MemData` struct, and write the raw binary bytes to the pipe descriptor.
* **`src/cpu_sensor.c` (Named Pipe):**
  * Open `/tmp/sysmon_cpu.fifo` in write-only mode (`O_WRONLY`).
  * Read `/proc/loadavg` every 1 second, populate a `CpuData` struct, and write the raw binary bytes to the FIFO.
* **`src/proc_sensor.c` (Named Pipe):**
  * Open `/tmp/sysmon_proc.fifo` in write-only mode (`O_WRONLY`).
  * Scan `/proc` every 1 second to count numeric PID directories, populate a `ProcData` struct, and write the raw binary bytes to the FIFO.

### 3. Supervisor Daemon Implementation (`src/sysmond.c`)
* **Anonymous Pipe & `mem_sensor` Launch:**
  * Create an anonymous pipe using `pipe(pipefd)` before forking.
  * Fork and execute `./bin/mem_sensor`, passing `pipefd[1]` formatted as a string argument in `argv`.
  * Close unused pipe file descriptors in both parent and child processes.
* **FIFO Setup & Sensor Spawning:**
  * Create `/tmp/sysmon_cpu.fifo` and `/tmp/sysmon_proc.fifo` using `mkfifo()`.
  * Spawn `./bin/cpu_sensor` and `./bin/proc_sensor` using `fork()` and `execve()`, storing their child PIDs.
  * Open both FIFOs in read-only mode (`O_RDONLY`).
* **Telemetry Loop & Graceful Teardown:**
  * Continuously read `MemData`, `CpuData`, and `ProcData` structs from their respective descriptors and render the live terminal dashboard.
  * Register signal handlers for `SIGINT` and `SIGTERM` that send `SIGTERM` to all three child PIDs, reap them with `waitpid()`, close all open descriptors, and remove the FIFO files with `unlink()`.

---

## Verification & Testing

1. **Build the Suite:**
   ```bash
   make clean && make
   ```
2. **Launch the Supervisor:**
   Run only the supervisor daemon from a single terminal (do not start the sensors manually):
   ```bash
   ./bin/sysmond
   ```
3. **Inspect Active Processes & IPC Nodes:**
   In a second terminal while `sysmond` is running:
   * Verify the process tree and parent-child PID relationships:
     ```bash
     ps -ef | grep bin/
     ```
   * Inspect the named pipes in the filesystem (verify the `p` file type indicator):
     ```bash
     ls -l /tmp/sysmon_*.fifo
     ```
4. **Verify Clean Shutdown:**
   * Press `Ctrl+C` in the `sysmond` terminal.
   * Re-run `ps -ef | grep bin/` and `ls -l /tmp/sysmon_*.fifo` to confirm that no orphaned sensors, zombie processes, or leftover FIFO files remain.
   
## Submission Method

* Submit your code to the git repository.

* Create a new branch "phase3" and upload the code to it.

=== END OF FILE ===



