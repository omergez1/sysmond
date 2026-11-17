CC      = gcc
CFLAGS  = -Wall -g -Iinc -pthread
LDFLAGS = -pthread

SRCS = src/sysmond.c src/mem_sensor.c src/cpu_sensor.c src/proc_sensor.c src/alert.c
OBJS = $(SRCS:src/%.c=obj/%.o)

all: bin/sysmond

bin obj:
	mkdir -p $@

# Link all object files into the single sysmond program.
bin/sysmond: $(OBJS) | bin
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# Compile each .c into a .o. Every file includes sysmon.h, so a change to
# the header rebuilds everything.
obj/%.o: src/%.c inc/sysmon.h | obj
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf bin/* obj

.PHONY: all clean
