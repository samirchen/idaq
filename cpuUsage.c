#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <linux/limits.h>
#include "cpuUsage.h"
#include "dieWithError.h"

#define READBUFSIZE 1024

void getWholeCPUStatus(ProcStat* ps) {
	//printf("\n======== getWholeCPUStatus() begin ========\n");
	// Get "/proc/stat" info.
	FILE* inputFile = NULL;
	
	chdir("/proc");
	inputFile = fopen("stat", "r");
	if (!inputFile) {
		dieWithError("'/proc/stat' fopen() failed");
	}

	char buff[1024];
	fgets(buff, sizeof(buff), inputFile); // Read 1 line.
	//printf(buff);	
	sscanf(buff, "%s %lld %lld %lld %lld %lld %lld %lld %lld %lld", ps->processorName, &ps->user, &ps->nice, &ps->system, &ps->idle, &ps->iowait, &ps->irq, &ps->softirq, &ps->stealstolen, &ps->guest); // Scan from "buff".	
	//printf("user: %lld\n", ps->user);

	fclose(inputFile);
	//printf("======== getWholeCPUStatus() end ========\n");
	
}

float calWholeCPUUse(ProcStat* ps1, ProcStat* ps2) {
	//printf("\n======== calWholeCPUUse() begin ========\n");
	num totalCPUTime = (ps2->user + ps2->nice + ps2->system + ps2->idle + ps2->iowait + ps2->irq + ps2->softirq + ps2->stealstolen + ps2->guest) - (ps1->user + ps1->nice + ps1->system + ps1->idle + ps1->iowait + ps1->irq + ps1->softirq + ps1->stealstolen + ps1->guest);
	num idleCPUTime = ps2->idle - ps1->idle;

	float CPUUse = ((float) totalCPUTime - (float) idleCPUTime) / (float) totalCPUTime;
	//printf("totalCPUTime: %lld, idleCPUTime: %lld\n", totalCPUTime, idleCPUTime);
	//printf("======== calWholeCPUUse() end ========\n");

	return CPUUse;
}

void getProcessCPUStatus(ProcPidStat* pps, pid_t pid) {
	//printf("\n======== getProcessCPUStatus() begin ========\n");

	// Get "/proc/[pid]/stat" info.
	FILE* inputFile = NULL;
	char fileName[1024];
	sprintf(fileName, "/proc/%d/stat", pid);
	//printf(fileName);
	inputFile = fopen(fileName, "r");
	if (!inputFile) {
		dieWithError("fopen() failed");
	}
	
	char buff[1024];
	fgets(buff, sizeof(buff), inputFile);
	//printf(buff);
	sscanf(buff, "%lld %s %c %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld ", &pps->pid, pps->tcomm, &pps->state, &pps->ppid, &pps->pgid, &pps->sid, &pps->tty_nr, &pps->tty_pgrp, &pps->flags, &pps->min_flt, &pps->cmin_flt, &pps->maj_flt, &pps->cmaj_flt, &pps->utime, &pps->stimev, &pps->cutime, &pps->cstime);
	//printf("process: %s, utime: %lld, stimev: %lld, cutime: %lld, cstime: %lld\n", pps->tcomm, pps->utime, pps->stimev, pps->cutime, pps->cstime);
	fclose(inputFile);

	//printf("======== getProcessCPUStatus() end ========\n");

}

float calProcessCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2) {
	//printf("\n======== calProcessCPUUse() begin ========\n");
	float CPUUse = 0.0;
	num totalCPUTime = (ps2->user + ps2->nice + ps2->system + ps2->idle + ps2->iowait + ps2->irq + ps2->softirq + ps2->stealstolen + ps2->guest) - (ps1->user + ps1->nice + ps1->system + ps1->idle + ps1->iowait + ps1->irq + ps1->softirq + ps1->stealstolen + ps1->guest);
	num processTime = (pps2->utime + pps2->stimev + pps2->cutime + pps2->cstime) - (pps1->utime + pps1->stimev + pps1->cutime + pps1->cstime);

	CPUUse = ((float) processTime) / ((float) totalCPUTime);

	//printf("======== calProcessCPUUse() end ========\n");

	return CPUUse;
}

// Thread "/proc/<pid>/task/<tid>" has the same data structure as process.
void getThreadCPUStatus(ProcPidStat* pps, pid_t pid, pid_t tid) { 
	//printf("\n======== getThreadCPUStatus() begin ========\n");

	// Get "/proc/[pid]/stat" info.
	FILE* inputFile = NULL;
	char fileName[1024];
	sprintf(fileName, "/proc/%d/task/%d/stat", pid, tid);
	//printf(fileName);
	inputFile = fopen(fileName, "r");
	if (!inputFile) {
		dieWithError("fopen() failed");
	}
	
	char buff[1024];
	fgets(buff, sizeof(buff), inputFile);
	//printf(buff);
	sscanf(buff, "%lld %s %c %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld ", &pps->pid, pps->tcomm, &pps->state, &pps->ppid, &pps->pgid, &pps->sid, &pps->tty_nr, &pps->tty_pgrp, &pps->flags, &pps->min_flt, &pps->cmin_flt, &pps->maj_flt, &pps->cmaj_flt, &pps->utime, &pps->stimev, &pps->cutime, &pps->cstime);
	//printf("Thread: %s, utime: %lld, stimev: %lld, cutime: %lld, cstime: %lld\n", pps->tcomm, pps->utime, pps->stimev, pps->cutime, pps->cstime);
	fclose(inputFile);

	//printf("======== getThreadCPUStatus() end ========\n");

}

float calThreadCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2) {
	//printf("\n======== calThreadCPUUse() begin ========\n");
	float CPUUse = 0.0;

	num totalCPUTime = (ps2->user + ps2->nice + ps2->system + ps2->idle + ps2->iowait + ps2->irq + ps2->softirq + ps2->stealstolen + ps2->guest) - (ps1->user + ps1->nice + ps1->system + ps1->idle + ps1->iowait + ps1->irq + ps1->softirq + ps1->stealstolen + ps1->guest);
    num threadTime = (pps2->utime + pps2->stimev) - (pps1->utime + pps1->stimev);

    CPUUse = ((float) threadTime) / ((float) totalCPUTime);

	//printf("======== calThreadCPUUse() end ========\n");

	return CPUUse;

}

/*
int main(int argc, char* argv[]) {

	printf("%s\n", argv[0]);
	pid_t pid = getpid();
	printf("pid: %d\n", pid);

	// Get processor num.
	int processorNum = sysconf(_SC_NPROCESSORS_CONF); // "unistd.h" is required.
	printf("Processors: %d\n", processorNum);
	

	// Test
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2; 
	int i = 0;
	for (i = 0; i <= 100000; i++) {
		
		srand((unsigned) time(NULL));
		int m = rand() % 100000;
		int n = 1 + rand() % 100000;
		m = m / n;
		
		if (i == 10) {
			getWholeCPUStatus(&ps1);
			getProcessCPUStatus(&pps1, pid);
		}

		if (i == 10000) {
			getWholeCPUStatus(&ps2);	
			getProcessCPUStatus(&pps2, pid);
		}
	}
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float processCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);
	printf("CPUUse: %f, processCPUUse: %f\n", CPUUse, processCPUUse);


	return 0;
}
*/
