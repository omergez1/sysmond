# sysmon

A small Linux system monitor written in C. Three sensor processes collect memory, CPU load and process statistics from `/proc` and send them to a supervisor daemon, `sysmond`, which prints a live status report in the terminal.

This branch, **`phase3b`**, is **v3b**: the v3.0 design (separate processes that send binary C structs over an anonymous pipe and two named pipes) with a **fault-tolerant `sysmond`**. When a sensor crashes, `sysmond` reaps it, rebuilds its channel and starts it again, while the other sensors keep running. See [sysmond-phase-3-guide.md](sysmond-phase-3-guide.md) and [sysmond-phase-3b-guide.md](sysmond-phase-3b-guide.md) for the assignments.

## Versions and branches

| Branch | Version | Design |
|--------|---------|--------|
| `phase1` | v1.0 | Sensor processes write text files in `/tmp/sysmon/`, and `sysmond` reads them |
| `phase2` | v2.0 | One multi-threaded process: shared struct, mutex, condition-variable alert |
| `phase3` | v3.0 | Sensor processes send binary structs to `sysmond` over a pipe and FIFOs |
| `phase3.1` | v3.1 | v3.0 where each sensor has a different update interval |
| **`phase3b`** | **v3b** | v3.0 with a watchdog that restarts crashed sensors |

Each version is also tagged (`git checkout v1.0`, `v2.0`, `v3.0`, `v3.1`).

## How it works

```
                  sysmond  (watchdog: fork + execve, poll, waitpid, restart, cleanup)
                     ^                  ^                    ^
   anonymous pipe    |   named pipe     |   named pipe       |
   (fd in argv[1])   |   /tmp/sysmon_cpu.fifo                /tmp/sysmon_proc.fifo
                     |                  |                    |
               mem_sensor          cpu_sensor           proc_sensor
            /proc/meminfo        /proc/loadavg        /proc/<pid>/ count
              -> MemData           -> CpuData           -> ProcData
```

1. `sysmond` creates the two FIFOs with `mkfifo()`. A FIFO left behind by a run that was killed is replaced. If anything else is at that path, `sysmond` stops with an error.
2. It opens both FIFOs with `O_RDONLY | O_NONBLOCK | O_CLOEXEC` and keeps them open for the whole run. Non-blocking matters here: a normal open waits for a writer.
3. It creates an anonymous pipe with `pipe()`. It forks and `execve`s `mem_sensor`, passing the pipe's write end as a number in `argv[1]` (for example `mem_sensor 6`). Then the parent closes its copy of the write end.
4. It forks and `execve`s `cpu_sensor` and `proc_sensor`. Each opens its FIFO with `O_WRONLY`. That open returns at once, because `sysmond` is already the reader.
5. Every second, each sensor writes one struct (`inc/sysmon_ipc.h`) as raw bytes. Every struct is smaller than `PIPE_BUF`, so each write arrives whole.
6. `sysmond` waits on all three channels at once with `poll()`, keeps the newest struct from each, and redraws the report every second.

**Descriptor hygiene:** each child holds only its own channel. Every descriptor `sysmond` reads from is marked close-on-exec (`FD_CLOEXEC` / `O_CLOEXEC`), and the pipe's write end is closed in `sysmond` right after `mem_sensor` is started. You can check with `ls -l /proc/<pid>/fd`.

## Watchdog: what happens when a sensor crashes

When a sensor dies, the kernel closes its write end. Its channel has no writer left, so `read()` in `sysmond` returns **0 (end of file)**. That starts the recovery:

1. **Reap:** `waitpid()` collects the dead child right away, so it never stays a zombie. `WIFEXITED` / `WIFSIGNALED` tell how it ended, for example `killed by signal 9 (Killed)`.
2. **Rebuild the channel:**
   - **`mem_sensor`:** the old anonymous pipe has no writer and no name, so nothing can ever write to it again. `sysmond` closes it, creates a **new pipe**, and passes the new write end to the new `mem_sensor`. In `/proc/<pid>/fd` the `pipe:[inode]` number changes.
   - **`cpu_sensor` / `proc_sensor`:** the FIFO read end **stays open**, with the same fd. The new sensor opens the same FIFO for writing, and data continues on the same fd.
3. **Restart** the sensor with `fork()` + `execve()` and add one to its restart count.

The other sensors are not affected. The report shows the restart count for each sensor and a log of the last 5 recoveries:

```
Sensors
  mem_sensor   pid 11255   running    Restart Count: 1   via anonymous pipe
  cpu_sensor   pid 11261   running    Restart Count: 1   via /tmp/sysmon_cpu.fifo
  proc_sensor  pid 11277   running    Restart Count: 1   via /tmp/sysmon_proc.fifo
Recovery log
  15:49:10 proc_sensor killed by signal 9 (Killed) -> restarted as pid 11277 (restart 1)
  15:49:08 cpu_sensor killed by signal 9 (Killed) -> restarted as pid 11261 (restart 1)
  15:49:07 mem_sensor killed by signal 9 (Killed) -> restarted as pid 11255 (restart 1)
```

**Details:**

| Case | Handling |
|---|---|
| A FIFO waiting for its new writer | Until the new sensor opens the FIFO, `poll()` would report `POLLHUP` on every call and the loop would spin. So while there is no writer, that FIFO is left out of `poll()` and checked every 50 ms with a non-blocking `read()`: `0` means no writer yet, while `EAGAIN` or data means the writer is connected. |
| A sensor that dies right after starting (e.g. its program is missing, exit code 127) | A sensor is started at most once per second, so it can't restart in a tight loop. The report shows `RESTARTING: ... restart in 0.7s`. |
| A sensor that dies before it ever opened its FIFO | There is no end of file to see, so the loop also calls `waitpid(-1, WNOHANG)`, which catches any child that ended. |
| Ctrl+C / SIGTERM on `sysmond` | The loop ends and the cleanup stops every sensor. Nothing is restarted, even though Ctrl+C also kills the sensors. |

`poll()` and `fcntl()` are used as in Phase 3. `poll()` is what lets `sysmond` wait on all three channels without one blocking the others.

## Building

```bash
make          # builds bin/sysmond, bin/mem_sensor, bin/cpu_sensor, bin/proc_sensor
make clean
```

The code compiles with `-Wall -Wextra -Werror`.

## Running

Run only the supervisor. It starts the sensors itself:

```bash
./bin/sysmond          # Ctrl+C to stop
```

```
========== sysmon v3b report 2026-11-25 15:49:11 ==========
Memory
  Total:        7481.0 MB
  Used:         3384.8 MB (45.2%)
  Free:         2304.3 MB
  Available:    4096.2 MB
  Swap:            0.0 / 4916.0 MB used (0.0%)
CPU load
  1 min: 0.21   5 min: 0.16   15 min: 0.09
Processes
  Running: 370
Sensors
  mem_sensor   pid 11246   running    Restart Count: 0   via anonymous pipe
  cpu_sensor   pid 11247   running    Restart Count: 0   via /tmp/sysmon_cpu.fifo
  proc_sensor  pid 11248   running    Restart Count: 0   via /tmp/sysmon_proc.fifo
```

In a terminal the screen redraws in place with color. Redirected to a file, it's plain text.

## Shutdown and failures

| Situation | What happens |
|-----------|--------------|
| Ctrl+C or `kill` on `sysmond` | The handler sets a flag, which ends the main loop. Then: SIGTERM to each sensor, `waitpid()` for each, close all descriptors, `unlink()` both FIFOs. Exit code 0. |
| A sensor dies | `sysmond` reaps it, rebuilds its channel and restarts it (see [Watchdog](#watchdog-what-happens-when-a-sensor-crashes)). |
| `sysmond` is killed with `kill -9` | Each sensor's next write gets SIGPIPE (no reader left), so the sensors exit by themselves within about 1 s. The FIFO files stay behind and are replaced on the next start. |
| A sensor program is missing | The child exits with code 127. `sysmond` keeps trying once per second and shows `RESTARTING: ... exited with code 127`. Nothing hangs. |

## Checking that it works

In a second terminal while `sysmond` runs:

```bash
ps -f -C sysmond,mem_sensor,cpu_sensor,proc_sensor   # sysmond + 3 sensors; sensors' PPID = sysmond's PID
ls -l /tmp/sysmon_*.fifo                   # type 'p' = named pipe
ls -l /proc/$(pgrep -x sysmond)/fd         # 0-2, both FIFOs, one pipe:[inode]
ls -l /proc/$(pgrep -x cpu_sensor)/fd      # only 0, 1, 2 and its own FIFO
```

To see the watchdog work:

```bash
kill -9 $(pgrep -x mem_sensor)             # restarted; pipe:[inode] in sysmond's fd list changes
kill -9 $(pgrep -x cpu_sensor)             # restarted; sysmond's FIFO fds stay the same
ps -eo pid,ppid,stat,comm | grep -E "sysmond|sensor"   # no Z (zombie) entries
```

`ps -C` selects processes by program name, so it finds `sysmond` however it was started. The guide's `ps -ef | grep bin/` also works, but it matches every program in `/usr/bin` too. Add `| grep -v -e grep -e /usr/` to hide those.

After Ctrl+C, the same commands should show no sensors and no FIFO files.

Run a sensor by hand to see its raw binary output, decoded:
```bash
./bin/mem_sensor | head -c 120 | od -An -t d8 -w40
```
This takes 3 records (3 × 40 bytes, one per second) and prints each as 5 numbers in kB: MemTotal, MemFree, MemAvailable, SwapTotal, SwapFree. Once `head` has its 120 bytes it exits, and `mem_sensor`'s next write gets SIGPIPE, so it stops by itself. That's the same mechanism that stops the sensors if `sysmond` is killed.

## Project layout

```
.
├── Makefile
├── README.md
├── sysmond-phase-1-guide.md
├── sysmond-phase-3-guide.md
├── sysmond-phase-3b-guide.md
├── inc/
│   └── sysmon_ipc.h     # MemData, CpuData, ProcData, FIFO paths, version
├── src/
│   ├── sysmond.c        # watchdog: FIFOs, pipe, fork/execve, poll loop, restart, cleanup
│   ├── mem_sensor.c     # writes MemData to the fd given in argv[1]
│   ├── cpu_sensor.c     # writes CpuData to /tmp/sysmon_cpu.fifo
│   └── proc_sensor.c    # writes ProcData to /tmp/sysmon_proc.fifo
└── bin/                 # built programs (not tracked by git)
```

## Lab report

The Phase 3B lab (pipe inode replacement, FIFO writer re-attachment, crash stress test) is in [watchdog_diagnostics.md](watchdog_diagnostics.md). Its screenshots are in `screenshots/phase3b/`.
