#ifndef CPUUSAGE_H
#define CPUUSAGE_H

#include <stdlib.h>
#include <linux/limits.h>

typedef long long int num;

typedef struct procstat {
	char processorName[20];
	num user;
	num nice;
	num system;
	num idle;
	num iowait;
	num irq;
	num softirq;
	num stealstolen;
	num guest;
} ProcStat;

typedef struct procpidstat {
	num pid;
	char tcomm[PATH_MAX];
	char state;

	num ppid;
	num pgid;
	num sid;
	num tty_nr;
	num tty_pgrp;

	num flags;
	num min_flt;
	num cmin_flt;
	num maj_flt;
	num cmaj_flt;
	num utime;
	num stimev;

	num cutime;
	num cstime;
	num priority;
	num nicev;
	num num_threads;
	num it_real_value;

	unsigned long long start_time;

	num vsize;
	num rss;
	num rsslim;
	num start_code;
	num end_code;
	num start_stack;
	num esp;
	num eip;

	num pending;
	num blocked;
	num sigign;
	num sigcatch;
	num wchan;
	num zero1;
	num zero2;
	num exit_signal;
	num cpu;
	num rt_priority;
	num policy;	
} ProcPidStat;


void getWholeCPUStatus(ProcStat* ps);
float calWholeCPUUse(ProcStat* ps1, ProcStat* ps2);
void getProcessCPUStatus(ProcPidStat* pps, pid_t pid);
float calProcessCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2);

// Thread "/proc/<pid>/task/<tid>" has the same data structure as process, ProcPidStat.
void getThreadCPUStatus(ProcPidStat* pps, pid_t pid, pid_t tid); 
float calThreadCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2);

#endif // CPUUSAGE_H
