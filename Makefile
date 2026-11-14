CC = gcc
CFLAGS = -Wall -g

all: bin/mem_sensor bin/cpu_sensor bin/proc_sensor

bin:
	mkdir -p bin

bin/mem_sensor: src/mem_sensor.c | bin
	$(CC) $(CFLAGS) src/mem_sensor.c -o bin/mem_sensor

bin/cpu_sensor: src/cpu_sensor.c | bin
	$(CC) $(CFLAGS) src/cpu_sensor.c -o bin/cpu_sensor

bin/proc_sensor: src/proc_sensor.c | bin
	$(CC) $(CFLAGS) src/proc_sensor.c -o bin/proc_sensor

clean:
	rm -f bin/*
