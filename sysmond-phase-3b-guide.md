# Student Lab Guide: `sysmond` Phase 3B — Fault-tolerance

## Overview & Prerequisites
In Phase 3, if a sensor process crashes mid-stream, `sysmond` either stalls or exits on `EOF`. In this lab, you will upgrade `sysmond` into a **fault-tolerant watchdog supervisor** that detects sensor termination at runtime, reaps crashed child processes, restores broken IPC channels, and respawns failed sensors automatically without interrupting the remaining telemetry streams.

**Constraints:** Use **only** the POSIX system calls covered in Phase 3 (`fork()`, `execve()`, `pipe()`, `mkfifo()`, `open()`, `read()`, `write()`, `close()`, `unlink()`, `signal()`, `kill()`, and `waitpid()`).

**Prerequisites:**
* **Working Phase 3 Codebase:** Completed Phase 3 binaries (`sysmond`, `mem_sensor`, `cpu_sensor`, `proc_sensor`) compiled cleanly with `-Wall -Wextra -Werror -Iinc`.
* **Git Branch:** Create and switch to a new branch named `phase3b` from your `phase3` branch.
* **Clean Build & Runtime Hygiene:** Before starting, verify `make clean && make` compiles with zero warnings, and confirm zero background `*_sensor` processes, zombie processes, or leftover `/tmp/sysmon_*.fifo` files exist. **Capture screenshot evidence** of this clean baseline (`make clean && make`, `ps -ef | grep bin/`, and `ls -l /tmp/sysmon_*.fifo`).
* **Lab Report Document:** A report file (PDF, DOCX, or Markdown—e.g., `watchdog_diagnostics.pdf`, `watchdog_diagnostics.docx`, or `watchdog_diagnostics.md`) in your repository root containing **full-window screenshots** (showing prompt, command, and output) and technical answers for all experiments below.

---

## Part 1: Watchdog Design Requirements

Design and implement your recovery architecture inside `src/sysmond.c` (keeping `inc/sysmon_ipc.h` and the sensor binaries intact unless minor cleanup is needed):

1. **Per-Sensor Lifecycle Tracking:** Maintain state for each of the three sensors (`mem_sensor`, `cpu_sensor`, `proc_sensor`), including its active child PID, IPC read file descriptor, and cumulative restart counter.
2. **Mid-Stream Crash Detection & Reaping:**
   * Detect unexpected sensor termination via `read()` return values (`EOF`) in the main telemetry loop.
   * Immediately reap any terminated child via `waitpid()` to prevent zombie processes, inspect its termination status (`WIFSIGNALED` / `WIFEXITED`), and log a recovery notice with the updated restart count on the live dashboard.
3. **IPC Channel Recovery:**
   * **Anonymous Pipe (`mem_sensor`):** Close the obsolete read descriptor, establish a new anonymous pipe, pass the new write descriptor to the respawned `mem_sensor`, and ensure no unused file descriptors leak across processes.
   * **Named Pipes (`cpu_sensor` & `proc_sensor`):** Respawn the failed sensor executable and resume reading from its FIFO channel without blocking the other active sensors.
4. **Clean Shutdown Guard:** Ensure `SIGINT` / `SIGTERM` (`Ctrl+C`) cleanly terminates and reaps all active sensors and unlinks both FIFOs without triggering false crash recoveries.

---

## Part 2: Analysis Procedures

Execute the following three experiments against your running watchdog and document your findings in your report.

### Experiment 1: Anonymous Pipe Inode Replacement (`/proc/<pid>/fd`)
**Goal:** Verify how the kernel allocates a new anonymous pipe buffer during `mem_sensor` recovery while leaving FIFO descriptors untouched.

1. Launch `./bin/sysmond` and record the initial process table (`ps -ef | grep bin/`) and `sysmond`'s open file descriptors (`ls -l /proc/<sysmond_pid>/fd`).
2. Terminate `mem_sensor` abruptly using `kill -9 <mem_sensor_pid>`.
3. Once `sysmond` respawns `mem_sensor`, re-run `ps -ef | grep bin/` and `ls -l /proc/<sysmond_pid>/fd`.
4. **Evidence & Questions:**
   * **Screenshot Evidence:** Before and after `ps -ef` and `/proc/<sysmond_pid>/fd` listings, plus the `sysmond` dashboard showing the recovery message and incremented restart count.
   * **Question 1A:** Compare the `pipe:[inode]` number before and after recovery. Why could `sysmond` not reuse the original `pipefd[0]` descriptor?
   * **Question 1B:** Verify that the file descriptors for `/tmp/sysmon_cpu.fifo` and `/tmp/sysmon_proc.fifo` remained unchanged across the recovery.

### Experiment 2: Named FIFO Writer Re-Attachment
**Goal:** Observe how a persistent `O_RDONLY` FIFO descriptor behaves when its writer dies (`EOF`) and a replacement writer opens the same FIFO.

1. With `./bin/sysmond` running, kill `cpu_sensor` with `kill -9 <cpu_sensor_pid>`.
2. Observe `sysmond` detect `EOF`, reap `cpu_sensor`, and respawn the replacement process.
3. Inspect `ls -l /proc/<sysmond_pid>/fd` and `ls -l /proc/<new_cpu_sensor_pid>/fd`.
4. **Evidence & Questions:**
   * **Screenshot Evidence:** Live `sysmond` recovery output alongside the `/proc/<pid>/fd` listings for `sysmond` and the new `cpu_sensor`.
   * **Question 2A:** Why does `read()` on `/tmp/sysmon_cpu.fifo` return `0` (`EOF`) when `cpu_sensor` dies, even though the FIFO node still exists in `/tmp`?
   * **Question 2B:** Why does the newly spawned `cpu_sensor` not block when opening `/tmp/sysmon_cpu.fifo` in `O_WRONLY` mode?

### Experiment 3: Cascading Crash Stress Test & Resource Leak Audit
**Goal:** Confirm zero zombie processes and zero leaked file descriptors across repeated mid-stream crashes.

1. While `./bin/sysmond` is running, kill each of the three sensors twice in sequence (6 crashes total, spaced ~2 seconds apart).
2. Inspect the process table (`ps -eo pid,ppid,stat,comm | grep -E "sysmond|sensor"`) and `sysmond`'s open file descriptors (`ls -l /proc/<sysmond_pid>/fd`).
3. Press `Ctrl+C` in `sysmond` and verify clean teardown (`ps -ef | grep bin/` and `ls -l /tmp/sysmon_*.fifo`).
4. **Evidence:**
   * **Screenshot Evidence:** Dashboard showing `Restart Count: 2` for all three sensors, `ps` output confirming zero `Z` (`defunct`) processes, `/proc/<sysmond_pid>/fd` showing a constant FD count, and post-`Ctrl+C` cleanup verification.
  
---

## Submission Checklist (`phase3b` Branch)
1. **Source Code:** Commit all updated source files (`inc/sysmon_ipc.h`, `src/*.c`, `Makefile`) to the **`phase3b`** git branch.
2. **Lab Report (PDF, DOCX, or Markdown):** Commit your completed report containing written technical answers and **full-window screenshot evidence** for:
   * **Prerequisites Check:** Clean warning-free build (`make clean && make`) and clean runtime state.
   * **Experiment 1:** Before/after `/proc/<sysmond_pid>/fd` pipe inode replacement screenshots + Questions 1A–1B.
   * **Experiment 2:** `cpu_sensor` FIFO recovery and `/proc/<pid>/fd` screenshots + Questions 2A–2B.
   * **Experiment 3:** 6-crash stress test (`Restart Count: 2` per sensor), zero-zombie `ps`, constant FD count, post-`Ctrl+C` cleanup screenshots.
   
   === END OF FILE ===