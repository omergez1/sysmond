# sysmon

A small Linux system monitor written in C. Three sensor processes collect memory, CPU load and process statistics from `/proc`, and a central daemon, `sysmond`, combines them into one status report printed to the terminal.

This is Phase 1 of a course project. The sensors and the daemon are separate programs that communicate through text files. Later phases replace the files with IPC and move to a multi-threaded design. See [sysmond-phase-1-guide.md](sysmond-phase-1-guide.md) for the full assignment.

## How it works

```
/proc/meminfo  ──>  mem_sensor   ──>  /tmp/sysmon/mem.dat  ──┐
/proc/loadavg  ──>  cpu_sensor   ──>  /tmp/sysmon/cpu.dat  ──┼──>  sysmond  ──>  terminal
/proc/<pid>/   ──>  proc_sensor  ──>  /tmp/sysmon/proc.dat ──┘
```

| Program       | Reads                                 | Writes                 |
|---------------|---------------------------------------|------------------------|
| `mem_sensor`  | `/proc/meminfo`                       | `/tmp/sysmon/mem.dat`  |
| `cpu_sensor`  | `/proc/loadavg`                       | `/tmp/sysmon/cpu.dat`  |
| `proc_sensor` | `/proc` (counts numeric PID folders)  | `/tmp/sysmon/proc.dat` |
| `sysmond`     | the three `.dat` files                | report to stdout       |

Every program runs in a loop with a 2-second interval, set by `INTERVAL` at the top of each source file. Each program creates `/tmp/sysmon` if it doesn't exist, so they can be started in any order.

## Building

Requires `gcc` and `make` on Linux.

```bash
make          # builds all four programs into bin/
make clean    # removes the built programs
```

## Running

Start the sensors in the background, then run the daemon:

```bash
./bin/mem_sensor &
./bin/cpu_sensor &
./bin/proc_sensor &
./bin/sysmond          # Ctrl+C to stop
```

Stop the sensors when you're done:

```bash
pkill -x mem_sensor; pkill -x cpu_sensor; pkill -x proc_sensor
```

Example report:

```
========== sysmon report 2026-11-14 16:44:56 ==========
Memory
  Total:        7481.0 MB
  Used:         2806.9 MB (37.5%)
  Free:         3276.1 MB
  Available:    4674.1 MB
  Swap:            0.0 / 4916.0 MB used (0.0%)
CPU load
  1 min: 0.64   5 min: 0.65   15 min: 0.34
Processes
  Running: 360
```

"Used" memory is `MemTotal - MemAvailable`, the same way `free` calculates it.

## Data file format

Each `.dat` file contains one `Key: value` pair per line:

```
# mem.dat (values in kB)          # cpu.dat         # proc.dat
MemTotal: 7660516 kB              Load1: 0.61       Processes: 360
MemFree: 2766552 kB               Load5: 0.38
MemAvailable: 4673020 kB          Load15: 0.24
SwapTotal: 5033980 kB
SwapFree: 5033980 kB
```

The sensors write to a temporary file (`*.dat.tmp`) and then `rename()` it over the real file. Because the rename happens in one step, `sysmond` never reads a half-written file.

## Error handling

`sysmond` keeps running when data is missing or bad, and labels the affected section:

| Situation                                   | What the report shows                       |
|---------------------------------------------|---------------------------------------------|
| A sensor hasn't written its file yet        | `(no data - is X running?)`                 |
| A file hasn't been updated for over 6 s     | `(STALE: last update Ns ago - ...)`         |
| A file's contents can't be parsed           | `(could not parse /tmp/sysmon/X.dat)`       |

If `/tmp/sysmon` exists but is a regular file, every program exits with an error message.

## Checking that it works

```bash
cat /tmp/sysmon/*.dat                 # see the raw sensor output
watch -n 1 'cat /tmp/sysmon/*.dat'    # watch it update every 2 seconds
cat /proc/loadavg                     # compare with cpu.dat
free -m                               # compare with the Memory section
ls /proc | grep -c '^[0-9]*$'         # compare with the Processes count
```

## Project layout

```
.
├── Makefile
├── README.md
├── sysmond-phase-1-guide.md   # assignment description
├── src/
│   ├── sysmond.c              # daemon: reads .dat files, prints the report
│   ├── mem_sensor.c
│   ├── cpu_sensor.c
│   └── proc_sensor.c
└── bin/                       # build output (not tracked by git)
```
