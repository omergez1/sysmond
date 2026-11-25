CC     = gcc
CFLAGS = -Wall -Wextra -Werror -g -Iinc

BINS = bin/sysmond bin/mem_sensor bin/cpu_sensor bin/proc_sensor

all: $(BINS)

bin:
	mkdir -p $@

# Each program is built from one .c file. All of them include
# sysmon_ipc.h, so changing the header rebuilds all four.
bin/%: src/%.c inc/sysmon_ipc.h | bin
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f bin/*

.PHONY: all clean
