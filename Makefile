All:
	gcc -Wall -g -pthread dieWithError.h dieWithError.c cpuUsage.h cpuUsage.c idaq.c -o idaq.o

clean:
	rm -rf idaq.o
