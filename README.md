# sysmon

A small Linux system monitor written in C. Three sensor processes collect memory, CPU load and process statistics from `/proc` and send them to a supervisor daemon, `sysmond`, which prints a live status report in the terminal.

This branch, **`phase3`**, is **v3.0**: separate processes that communicate through real Linux IPC, which is an anonymous pipe and two named pipes (FIFOs), carrying binary C structs. See [sysmond-phase-3-guide.md](sysmond-phase-3-guide.md) for the assignment.

## Versions and branches

| Branch | Version | Design |
|--------|---------|--------|
| `phase1` | v1.0 | Sensor processes write text files in `/tmp/sysmon/`, and `sysmond` reads them |
| `phase2` | v2.0 | One multi-threaded process: shared struct, mutex, condition-variable alert |
| **`phase3`** | **v3.0** | Sensor processes send binary structs to `sysmond` over a pipe and FIFOs |

`main` holds v1.0 followed by v2.0. Each version is also tagged (`git checkout v1.0`, `v2.0`, `v3.0`).

## How it works

```
                  sysmond  (supervisor: fork + execve, poll, cleanup)
                     ^                  ^                    ^
   anonymous pipe    |   named pipe     |   named pipe       |
   (fd in argv[1])   |   /tmp/sysmon_cpu.fifo                /tmp/sysmon_proc.fifo
                     |                  |                    |
               mem_sensor          cpu_sensor           proc_sensor
            /proc/meminfo        /proc/loadavg        /proc/<pid>/ count
              -> MemData           -> CpuData           -> ProcData
```

1. `sysmond` creates the two FIFOs with `mkfifo()`. A FIFO left behind by a run that was killed is replaced. If anything else is at that path, `sysmond` stops with an error.
2. It creates an anonymous pipe with `pipe()`. It forks and `execve`s `mem_sensor`, passing the pipe's write end as a number in `argv[1]` (for example `mem_sensor 4`). Then the parent closes its copy of the write end.
3. It forks and `execve`s `cpu_sensor` and `proc_sensor`. Each opens its FIFO with `O_WRONLY`.
4. It opens both FIFOs with `O_RDONLY | O_NONBLOCK`. Non-blocking matters here. A normal open waits for a writer, so it would hang forever if a sensor failed to start.
5. Every second, each sensor writes one struct (`inc/sysmon_ipc.h`) as raw bytes. Every struct is smaller than `PIPE_BUF`, so each write arrives whole.
6. `sysmond` waits on all three channels at once with `poll()`, keeps the newest struct from each, and redraws the report every second.

**Descriptor hygiene:** each child holds only its own channel. The pipe's read end is marked `FD_CLOEXEC`, and the FIFOs are opened after all the forks. You can check with `ls -l /proc/<pid>/fd`.

## Building

```bash
make          # builds bin/sysmond, bin/mem_sensor, bin/cpu_sensor, bin/proc_sensor
make clean
```

## Running

Run only the supervisor. It starts the sensors itself:

```bash
./bin/sysmond          # Ctrl+C to stop
```

```
========== sysmon v3.0 report 2026-11-18 15:31:24 ==========
Memory
  Total:        7481.0 MB
  Used:         3418.7 MB (45.7%)
  Free:         1191.6 MB
  Available:    4062.3 MB
  Swap:            0.0 / 4916.0 MB used (0.0%)
CPU load
  1 min: 0.39   5 min: 0.27   15 min: 0.14
Processes
  Running: 365
Sensors
  mem_sensor   pid 15036   running  via anonymous pipe
  cpu_sensor   pid 15037   running  via /tmp/sysmon_cpu.fifo
  proc_sensor  pid 15038   running  via /tmp/sysmon_proc.fifo
```

In a terminal the screen redraws in place with color. Redirected to a file, it's plain text.

## Shutdown and failures

| Situation | What happens |
|-----------|--------------|
| Ctrl+C or `kill` on `sysmond` | The handler sets a flag, which ends the main loop. Then: SIGTERM to each sensor, `waitpid()` for each, close all descriptors, `unlink()` both FIFOs. Exit code 0. |
| A sensor dies | `sysmond` keeps running and shows `SENSOR DOWN: <name> killed by signal N`. The child is reaped (no zombie), and its channel is closed so `poll()` doesn't spin. |
| `sysmond` is killed with `kill -9` | Each sensor's next write gets SIGPIPE (no reader left), so the sensors exit by themselves within about 1 s. The FIFO files stay behind and are replaced on the next start. |
| A sensor program is missing | The child exits with code 127, and the report shows `SENSOR DOWN: ... exited with code 127`. Nothing hangs. |

## Checking that it works

In a second terminal while `sysmond` runs:

```bash
ps -f -C sysmond,mem_sensor,cpu_sensor,proc_sensor   # sysmond + 3 sensors; sensors' PPID = sysmond's PID
ls -l /tmp/sysmon_*.fifo                   # type 'p' = named pipe
ls -l /proc/$(pgrep -x cpu_sensor)/fd      # only 0, 1, 2 and its own FIFO
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
├── inc/
│   └── sysmon_ipc.h     # MemData, CpuData, ProcData, FIFO paths, version
├── src/
│   ├── sysmond.c        # supervisor: FIFOs, pipe, fork/execve, poll loop, cleanup
│   ├── mem_sensor.c     # writes MemData to the fd given in argv[1]
│   ├── cpu_sensor.c     # writes CpuData to /tmp/sysmon_cpu.fifo
│   └── proc_sensor.c    # writes ProcData to /tmp/sysmon_proc.fifo
└── bin/                 # built programs (not tracked by git)
```
