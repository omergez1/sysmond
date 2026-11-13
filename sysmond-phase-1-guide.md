# Phase 1: Multi-Process Architecture & File-Based Telemetry

## Background
In this course, you will incrementally build **`sysmon`** (Server Monitoring Tool), a multi-process Linux system daemon designed to gather real-time performance metrics and serve them to a command-line interface. 

Begin with a multi-process suite where individual sensor processes collect data from the `/proc` virtual filesystem and accumulate data snapshots into intermediate files. 
In subsequent phases, you will refactor this architecture to use IPC mechanisms and transition to a multi-threaded architecture.

---

## Lab Overview & Objectives
In Phase 1, you will create four distinct binaries:
1. `mem_sensor`: Collects memory metrics.
2. `cpu_sensor`: Collects CPU load averages.
3. `proc_sensor`: Collects running process statistics.
4. `sysmond`: Central daemon process that aggregates data from all sensors.

The sensor processes will write periodic metric reports to text files in `/tmp/sysmon/`, and `sysmond` will read these files to generate a unified terminal report.

Upon completing this lab, you will be able to:
* Parse key system metrics from the Linux `/proc` virtual filesystem.
* Configure a modular Makefile compiling multiple distinct binary targets.
* Implement process isolation across multiple concurrent executables.
* Perform file-based telemetry logging and aggregation using C file I/O operations.

---

## Linux Data Sources Overview (`/proc`)

The Linux `/proc` filesystem is a pseudo-filesystem generated dynamically by the kernel to expose system state and process metadata:

* **Memory Metrics (`/proc/meminfo`):**
  * Formatted as `Key: Value kB` key-value pairs.
  * **Target Fields:** `MemTotal` (total usable RAM), `MemFree` (unused RAM), `MemAvailable` (RAM available for starting new applications without swapping), `SwapTotal`, and `SwapFree`.
* **CPU Load Metrics (`/proc/loadavg`):**
  * Provides system load averages over 1-minute, 5-minute, and 15-minute intervals.
* **Process Metrics (`/proc` Directory):**
  * Every running process has a corresponding sub-directory named after its Process ID (e.g., `/proc/1234`).
  * Enumerating `/proc` using directory scanning functions (`opendir`/`readdir`) and filtering for numeric directory names yields total active process counts.

---

## Student Instructions

### 1. Build & Project Setup
* Create the project workspace with directories `src/` and `bin/`.
* Construct a modular `Makefile` in the root directory configured to compile four separate executables: `sysmond`, `mem_sensor`, `cpu_sensor`, and `proc_sensor`.
* Include compiler flags `-Wall -g` for debugging support.

### 2. Sensor Development
* **`mem_sensor.c`:**
  * Open `/proc/meminfo` using standard C file I/O (`fopen`, `fgets`).
  * Parse values for `MemTotal`, `MemFree`, `MemAvailable`, `SwapTotal`, and `SwapFree`.
  * Write formatted metrics to `/tmp/sysmon/mem.dat` every X seconds.
* **`cpu_sensor.c`:**
  * Open `/proc/loadavg`.
  * Extract 1-minute, 5-minute, and 15-minute load averages.
  * Write results to `/tmp/sysmon/cpu.dat` every X seconds.
* **`proc_sensor.c`:**
  * Scan `/proc` using `opendir()` and `readdir()` to count active PID directories.
  * Determine the total number of active processes.
  * Write process statistics to `/tmp/sysmon/proc.dat` every X seconds.

### 3. Daemon Development
* **`sysmond.c`:**
  * Ensure the storage directory exists on startup (`mkdir -p /tmp/sysmon`).
  * Periodically open and read `/tmp/sysmon/mem.dat`, `/tmp/sysmon/cpu.dat`, and `/tmp/sysmon/proc.dat`.
  * Aggregate the file contents and print a formatted system status report to standard output every X seconds.

### 4. Verification & Testing
* Compile all targets using `make`.
* Run each sensor process in separate terminal windows or execute them in the background:
  ```bash
  ./bin/mem_sensor &
  ./bin/cpu_sensor &
  ./bin/proc_sensor &
  
* Run ./bin/sysmond and verify that output metrics update continuously based on file changes.

* Inspect the intermediate files manually (e.g., cat /tmp/sysmon/mem.dat) to verify formatting and output intervals.