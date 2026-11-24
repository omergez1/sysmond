#ifndef SYSMON_IPC_H
#define SYSMON_IPC_H

/*
 * sysmon_ipc.h - data sent from the sensors to sysmond (v3).
 *
 * Each sensor writes one whole struct at a time, as raw bytes, to its
 * channel. All structs are far smaller than PIPE_BUF (4096 bytes), so the
 * kernel guarantees each write arrives in one piece, never mixed with
 * another write.
 *
 *   mem_sensor  -> anonymous pipe (fd number given in argv[1]) -> MemData
 *   cpu_sensor  -> CPU_FIFO_PATH                               -> CpuData
 *   proc_sensor -> PROC_FIFO_PATH                              -> ProcData
 */

#define CPU_FIFO_PATH  "/tmp/sysmon_cpu.fifo"
#define PROC_FIFO_PATH "/tmp/sysmon_proc.fifo"

#define SYSMON_VERSION  "3.1"

/* Each sensor sends at its own rate (seconds between samples). sysmond
 * includes this header too, so it knows how often to expect each one. */
#define MEM_INTERVAL    1
#define CPU_INTERVAL    2
#define PROC_INTERVAL   4

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
