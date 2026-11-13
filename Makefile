CC = gcc
CFLAGS = -Wall -g

all: bin/cpu_sensor

bin/cpu_sensor: src/cpu_sensor.c
	$(CC) $(CFLAGS) src/cpu_sensor.c -o bin/cpu_sensor

clean:
	rm -f bin/*
