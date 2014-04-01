#include <stdio.h> // for printf() and fprintf().
#include <sys/socket.h> // for socket(), bind(), and connect().
#include <arpa/inet.h> // for sockaddr_in and inet_nota().
#include <stdlib.h> // for atoi() and exit().
#include <string.h> // for memset().
#include <unistd.h> // for close().
#include <sys/time.h> // for timeval.
#include <pthread.h> // for pthread_create(). And use -pthread on Ubuntu, -lpthread on SLC.
#include <sys/syscall.h> // for SYS_gettid.
#include "dieWithError.h"
#include "cpuUsage.h"

#define MAXPENDING 10 // Maximum outstanding conncetion requests.
#define RCVBUFSIZE (1024*1024) // Size of receive buffer.

/* ######################## Method Declare ######################## */
// ================= Out of this file. ================
// In "dieWithError.c".
void dieWithError(const char *errorMessage);

// In "cpuUsage.c".
void getWholeCPUStatus(ProcStat* ps);
float calWholeCPUUse(ProcStat* ps1, ProcStat* ps2);
void getProcessCPUStatus(ProcPidStat* pps, pid_t pid);
float calProcessCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2);

// Thread "/proc/<pid>/task/<tid>" has the same data structure as process, ProcPidStat.
void getThreadCPUStatus(ProcPidStat* pps, pid_t pid, pid_t tid);
float calThreadCPUUse(ProcStat* ps1, ProcPidStat* pps1, ProcStat* ps2, ProcPidStat* pps2);


typedef enum CLIENTTYPE {
	L1Client = 1,
	L2Client = 2,
	MultiConnSingleThreadL2Client = 3,
	MultiConnMultiThreadL2Client = 4
} ClientType;
typedef enum SERVERTYPE {
	DefaultServer = 1,
	MultiConnSingleThreadServer = 2,
	MultiConnMultiThreadServer = 3
} ServerType;

struct PARAS {
	// Server only paras.
	char serverType;
	
	// Client only paras.
	char* servIP; // Server IP address (dotted quad).
	char clientType;
	
	// L2 client only paras.
	unsigned short prePort;	

	// Common paras.
	char isServer;
	unsigned short servPort; // Server port.
	unsigned int pkgSize; // Package size (Byte).
	unsigned int interval; // Testing time (Second). 
} Paras;

// [ ClntSockPool
// Client socket pool, main thread accept client socket and put it in pool, receive thread of server or receive-send thread of L2 client get client socket from pool.
// Used in "MultiConnMultiThreadServer", "MultiConnMultiThreadL2Client".
typedef struct clntSockPool {
	int pool[MAXPENDING];
	int poolTop;
	pthread_mutex_t poolLock;
} ClntSockPool;

ClntSockPool* clntSockPoolAlloc() {
	ClntSockPool* cspool;
	if ((cspool = (ClntSockPool*) malloc(sizeof(ClntSockPool))) != NULL) {
		cspool->poolTop = 0;
		if (pthread_mutex_init(&cspool->poolLock, NULL) != 0) {
			free(cspool);
			return NULL;
		}
	}

	return cspool;
}

void clntSockPoolRelease(ClntSockPool* cspool) {
	pthread_mutex_lock(&cspool->poolLock);
	if (cspool->poolTop == 0) {
		pthread_mutex_unlock(&cspool->poolLock);
		pthread_mutex_destroy(&cspool->poolLock);
		free(cspool);
	}
	else {
		pthread_mutex_unlock(&cspool->poolLock);
	}
}

// ]

// [ Connection 
// Connection is a structure contains: socket fd, server address, client address.
// Receive thread accept one Connection then deal with it.
typedef struct connection {
    int socketfd;
    struct sockaddr_in serverAddress;
    struct sockaddr_in clientAddress;
} Connection;
// ]

void printUsage() {
	printf("Usage: \n");                                                                                
	printf("    idaq [-c clientType|-s serverType] [-a serverIP] [-p serverPort] [-P preClientPort] [-t testInterval] [-size packageSize]\n");
}

void server() {
	printf("server\n");
	int servSock; // Socket descriptor for server. Listen on servSock.
	int clntSock; // Socket descriptor for client. New connection on clntSock.
	struct sockaddr_in servAddr; // Local address.
	struct sockaddr_in clntAddr; // Client address.
	unsigned int clntLen; // Length of client address data structure.

	unsigned short servPort = Paras.servPort;

	printf("server port: %d\n", servPort);

	// Create socket for incoming connections.
	if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("server socket() failed");
	}

	// Construct local address structure.
	memset(&servAddr, 0, sizeof(servAddr)); // Zero out structure.
	servAddr.sin_family = AF_INET; // Internet address family.
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface.
	servAddr.sin_port = htons(servPort); // Local port. Network byte order.
	
	// Bind to the local address.
	if (bind(servSock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0) {
		dieWithError("server bind() faild");
	}

	// Mark the socket so it will listen for incoming connections.
	if (listen(servSock, MAXPENDING) < 0) {
		dieWithError("server listen() failed");
	}

	fd_set fds;
	int maxsock = servSock;	
	struct timeval timeout;
	clntLen = sizeof(clntAddr);

	while (1) {
		FD_ZERO(&fds);
		FD_SET(servSock, &fds);
		timeout.tv_sec = 30;
		timeout.tv_usec = 0;
		int ret = 0;
		if ((ret = select(maxsock+1, &fds, NULL, NULL, &timeout)) < 0) {
			dieWithError("server select() failed");
		}
		else if (ret == 0) {
			printf("timeout\n");
			break;
		}

		// Process will go on until server accepts a new connection.
		if ((clntSock = accept(servSock, (struct sockaddr*) &clntAddr, &clntLen)) < 0) {
			dieWithError("server accept() failed");
		} 
		printf("new connection on socket %d\n", clntSock);
		if (clntSock > maxsock) {
			maxsock = clntSock;
		}
		FD_SET(clntSock, &fds);


		// [When comes a connection, recieve the message and calculte the CPU and speed.
		char buffer[RCVBUFSIZE];
		bzero(buffer, RCVBUFSIZE);

		unsigned long long int totalRecvMsgSize = 0;
		int recvMsgSize;
		
		// CPU calculating.
		ProcStat ps1, ps2;
		ProcPidStat pps1, pps2;	
		pid_t pid = getpid();
		getWholeCPUStatus(&ps1);
		getProcessCPUStatus(&pps1, pid);

		// Time calculating.
		struct timeval t1, t2;
		gettimeofday(&t1, NULL);
		double timeSpan = 0.0;

		while (1) {
			if ((recvMsgSize = recv(clntSock, buffer, RCVBUFSIZE, 0)) < 0) {
				dieWithError("server recv() failed");
			}
			else if (recvMsgSize > 0) {
				//buffer[RCVBUFSIZE-1] = '\0';
				totalRecvMsgSize += recvMsgSize;
				//printf("recvMsgSize: %d\n", recvMsgSize);
				//printf("totalRecvMsgSize: %lld\n", totalRecvMsgSize);
				//printf("%s\n", buffer);
			}
			else { // recvMsgSize == 0
				getWholeCPUStatus(&ps2);
				getProcessCPUStatus(&pps2, pid);
				float CPUUse = calWholeCPUUse(&ps1, &ps2);
				float processCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);
				printf("CPUUse: %f, processCPUUse: %f\n", CPUUse, processCPUUse);

				gettimeofday(&t2, NULL);
				timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
				double recvSpeed = ((double) totalRecvMsgSize * 8) / (timeSpan * 1000 *1000);
				printf("totalRecvMsgSize: %llu Bytes\n", totalRecvMsgSize);
				printf("time span: %lf s\n", timeSpan);
				printf("receive speed: %lf Mb/s\n\n", recvSpeed);

				FD_CLR(clntSock, &fds);
				close(clntSock);
				// ]

				break;
			}
		}
	}
	
	close(servSock);

	exit(0);

}

void multiConnSingleThreadServer() {
	printf("multiConnSingleThreadServer\n");
	unsigned short servPort = Paras.servPort;

	int fdArr[MAXPENDING] = {0}; // Accepted connection fds.
	int connAmount; // Current connection amount.

	int servSock, clntSock; // Listen on servSock, new connection on clntSock.
	struct sockaddr_in servAddr; // Server address info.
	struct sockaddr_in clntAddr; // connector's address info.
	socklen_t sinSize; 
	int on = 1;
	char buffer[RCVBUFSIZE];
	int ret;
	int i;

	if ((servSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		dieWithError("multiConnSingleThreadServer socket() failed");
	}

	if (setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0) {
		dieWithError("multiConnSingleThreadServer setsockopt() failed");
	}

	// Construct local address structure.
	servAddr.sin_family = AF_INET; // Host byte order.
	servAddr.sin_port = htons(servPort);
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(servAddr.sin_zero, '\0', sizeof(servAddr.sin_zero));

	if (bind(servSock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0) {
		dieWithError("multiConnSingleThreadServer bind() failed");
	}

	if (listen(servSock, MAXPENDING) < 0) {
		dieWithError("multiConnSingleThreadServer listen() failed");
	}
	printf("servPort: %d\n", servPort);

	fd_set fds;
	int maxsock;
	struct timeval timeout;

	connAmount = 0;
	sinSize = sizeof(clntAddr);
	maxsock = servSock;

	unsigned long long int totalRecvMsgSize[MAXPENDING] = {0};
	double timeSpan[MAXPENDING] = {0.0};
	float CPUUse[MAXPENDING] = {0.0};
	float processCPUUse[MAXPENDING] = {0.0};
	double recvSpeed[MAXPENDING] = {0.0};
	
	ProcStat ps1[MAXPENDING], ps2[MAXPENDING];
	ProcPidStat pps1[MAXPENDING], pps2[MAXPENDING];
	pid_t pid = getpid();

	struct timeval t1[MAXPENDING], t2[MAXPENDING];


	while (1) {
		// Init file descriptor set.
		FD_ZERO(&fds);
		FD_SET(servSock, &fds);

		// Timeout setting.
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		// Add active connection to fd set.
		for (i = 0; i < MAXPENDING; i++) {
			if (fdArr[i] != 0) {
				FD_SET(fdArr[i], &fds);
			}	
		}

		ret = select(maxsock+1, &fds, NULL, NULL, &timeout);
		if (ret < 0) {
			dieWithError("multiConnSingleThreadServer select() failed");
		}
		else if (ret == 0) {
			printf("timeout\n");
			break; // When timeout, jump out the loop and end the server.
			//continue; // When timeout, just continue to waiting for new connetions.
		}

		// Check every fd in the set.
		for (i = 0; i < MAXPENDING; i++) {
			if (FD_ISSET(fdArr[i], &fds)) {
				ret = recv(fdArr[i], buffer, RCVBUFSIZE, 0);
				if (ret < 0) {
					dieWithError("multiConnSingleThreadServer recv() failed");
				}
				else if (ret > 0) {
					totalRecvMsgSize[i] += ret;
					// Receive data.
					//if (ret < RCVBUFSIZE) {
					//	memset(&buffer[ret], '\0', 1);
					//}
					//printf("client[%d] send: %c...\n", i, buffer[0]);
				}
				else { // ret == 0
					// Close client.
					printf("close connection client[%d]\n", i);
					close(fdArr[i]);
					connAmount--;
					FD_CLR(fdArr[i], &fds);
					fdArr[i] = 0;

					// time and CPU.
					gettimeofday(&t2[i], NULL);
					getWholeCPUStatus(&ps2[i]);
					getProcessCPUStatus(&pps2[i], pid);

					timeSpan[i] = (double) (t2[i].tv_sec-t1[i].tv_sec) + (double) t2[i].tv_usec*1e-6 - (double) t1[i].tv_usec*1e-6;
					recvSpeed[i] = ((double) totalRecvMsgSize[i] * 8) / (timeSpan[i] * 1000 *1000);
					CPUUse[i] = calWholeCPUUse(&ps1[i], &ps2[i]);
					processCPUUse[i] = calProcessCPUUse(&ps1[i], &pps1[i], &ps2[i], &pps2[i]);

				}
			}
		}

		// Check whether a new connection comes.
		if (FD_ISSET(servSock, &fds)) {
			clntSock = accept(servSock, (struct sockaddr*) &clntAddr, &sinSize);
			if (clntSock <= 0) {
				dieWithError("multiConnSingleThreadServer accept() failed");
			}

			// Add to fd queue.
			if (connAmount < MAXPENDING) {
				for (i = 0; i < MAXPENDING; i++) {
					if (fdArr[i] == 0) {
						fdArr[i] = clntSock;
						connAmount++;
						
						// time and CPU.
						gettimeofday(&t1[i], NULL);
						getWholeCPUStatus(&ps1[i]);
						getProcessCPUStatus(&pps1[i], pid);

						printf("new connection client[%d] %s:%d\n", i, inet_ntoa(clntAddr.sin_addr), ntohs(clntAddr.sin_port));
						break;
					}
				}

				if (clntSock > maxsock) {
					maxsock = clntSock;
				}
			}
			else {
				printf("connections limits\n");
				//send(clntSock, "bye", 4, 0);
				close(clntSock);
				//break; // Jump out while and end the server.
				continue;
			}
		}

		/*
		// Show clients.
		printf("\n\n");
		printf("client amount: %d\n", connAmount);
		for (i = 0; i < MAXPENDING; i++) {
			printf("[%d]:%d ", i, fdArr[i]);
		}
		printf("\n\n");
		*/

	}

	// Close other connections.
	for (i = 0; i < MAXPENDING; i++) {
		if (fdArr[i] != 0) {
			close(fdArr[i]);
			connAmount--;
		}
	} 

	for (i = 0; i < MAXPENDING; i++) {
		printf("connection %d \nCPUUse: %f, processCPUUse: %f\ntotalRecvMsgSize: %llu Bytes\ntimeSpan: %lf\nrecvSpeed: %lf Mb/s\n\n", i, CPUUse[i], processCPUUse[i], totalRecvMsgSize[i], timeSpan[i], recvSpeed[i]);
	}

	exit(0);
}


pid_t gettid() {
	return syscall(SYS_gettid); // Return the tid same as "/proc/<pid>/task/<tid>"
}
unsigned long long int threadRTag = 0;
pthread_mutex_t threadRTagLock;
void* threadReceive(void* arg) {
	printf("threadReceive\n");
	ClntSockPool* cspool = (ClntSockPool*) arg;
	int clntSock = -1;
	while (1) {
		clntSock = cspool->pool[cspool->poolTop-1];
		if (clntSock != -1) {
			pthread_mutex_lock(&cspool->poolLock);
			cspool->poolTop--;
			pthread_mutex_unlock(&cspool->poolLock);
			break;
		}
	}

	// Get process id and thread id.
	pid_t pid = getpid();
    //pthread_t tid = pthread_self(); // Get tid, different from "syscall(SYS_gettid)".
    pid_t tid = gettid();
	printf("thread clntSock: %d, pid: %u, tid: %u\n\n", clntSock, (unsigned int) pid, (unsigned int) tid);

	char buffer[RCVBUFSIZE];
	bzero(buffer, RCVBUFSIZE);

	int recvMsgSize;
	unsigned long long int totalRecvMsgSize = 0;

	// CPU calculating.
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2;
	getWholeCPUStatus(&ps1);
	getThreadCPUStatus(&pps1, pid, tid); // accurate?

	// Time calculating.
	struct timeval t1, t2;
	gettimeofday(&t1, NULL);
	double timeSpan = 0.0;

	while (1) {
		if ((recvMsgSize = recv(clntSock, buffer, RCVBUFSIZE, 0)) < 0) {
			dieWithError("threadReceive recv() failed");
		}
		else if (recvMsgSize > 0) {
			totalRecvMsgSize += recvMsgSize;
			//printf("thread %u recvMsgSize: %d\n", (unsigned int) tid, recvMsgSize);
            //printf("thread %u totalRecvMsgSize: %lld\n", (unsigned int) tid, totalRecvMsgSize);
		}
		else {
			break;
		}
	}
	
	getWholeCPUStatus(&ps2);
	getThreadCPUStatus(&pps2, pid, tid);
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float threadCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);

	gettimeofday(&t2, NULL);
	timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
	double recvSpeed = ((double) totalRecvMsgSize * 8) / (timeSpan * 1000 *1000);

	pthread_mutex_lock(&threadRTagLock);
	threadRTag++;
	printf("Tag: %llu\n", threadRTag);
	pthread_mutex_unlock(&threadRTagLock);
	printf("thread %d-%d CPUUse: %f, threadCPUUse: %f\n", pid, tid, CPUUse, threadCPUUse);
	printf("thread %d-%d totalRecvMsgSize: %llu Bytes\n", pid, tid, totalRecvMsgSize);
	printf("thread %d-%d time span: %lf\n", pid, tid, timeSpan);
	printf("thread %d-%d receive speed: %lf Mb/s\n\n", pid, tid, recvSpeed);

	close(clntSock);
	pthread_exit((void*) 0);

	return ((void*) 0);
}
// Thread to deal with one Connection.
void* threadReceiveConnection(void* arg) {
    printf("threadReceiveConnection\n");

    Connection* conn = (Connection*) arg;
    int connectionSock = conn->socketfd;

    // Get process id and thread id.
    pid_t pid = getpid();
    //pthread_t tid = pthread_self(); // Get tid, different from "syscall(SYS_gettid)".
    pid_t tid = gettid();
    printf("thread connectionSock: %d, pid: %u, tid: %u\n\n", connectionSock, (unsigned int) pid, (unsigned int) tid);

    int buffer[RCVBUFSIZE];
    bzero(buffer, RCVBUFSIZE);

    int recvMsgSize;
    unsigned long long int totalRecvMsgSize = 0;

    // CPU calculating.
    ProcStat ps1, ps2;
    ProcPidStat pps1, pps2;
    getWholeCPUStatus(&ps1);
    getThreadCPUStatus(&pps1, pid, tid); // accurate?

    // Time calculating.
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);
    double timeSpan = 0.0;

    while (1) {
        if ((recvMsgSize = recv(connectionSock, buffer, RCVBUFSIZE*sizeof(int), 0)) < 0) {
            dieWithError("threadReceive recv() failed");
        }
        else if (recvMsgSize > 0) {
            totalRecvMsgSize += recvMsgSize;
            //printf("thread %u recvMsgSize: %d\n", (unsigned int) tid, recvMsgSize);
            //printf("thread %u totalRecvMsgSize: %lld\n", (unsigned int) tid, totalRecvMsgSize);
            /*int i = 0;
            for (i = 0; i < RCVBUFSIZE; i++) {
                printf("%d ", ntohs(buffer[i]));
            }
            printf("\n");*/
        }
        else {
            break;
        }
    }

    getWholeCPUStatus(&ps2);
    getThreadCPUStatus(&pps2, pid, tid);
    float CPUUse = calWholeCPUUse(&ps1, &ps2);
    float threadCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);

    gettimeofday(&t2, NULL);
    timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
    double recvSpeed = ((double) totalRecvMsgSize * 8) / (timeSpan * 1000 *1000);

    pthread_mutex_lock(&threadRTagLock);
    threadRTag++;
    printf("Tag: %llu\n", threadRTag);
    pthread_mutex_unlock(&threadRTagLock);
    printf("thread %d-%d CPUUse: %f, threadCPUUse: %f\n", pid, tid, CPUUse, threadCPUUse);
    printf("thread %d-%d totalRecvMsgSize: %llu Bytes\n", pid, tid, totalRecvMsgSize);
    printf("thread %d-%d time span: %lf\n", pid, tid, timeSpan);
    printf("thread %d-%d receive speed: %lf Mb/s\n\n", pid, tid, recvSpeed);

    close(connectionSock);
    free(conn);
    pthread_exit((void*) 0);

    return ((void*) 0);

}
void multiConnMultiThreadServer() { 
	printf("multiConnMultiThreadServer\n");
	unsigned short servPort = Paras.servPort;

	int servSock, clntSock; // Listen on servSock, new connection on clntSock.
	struct sockaddr_in servAddr; // Server address info.
	struct sockaddr_in clntAddr; // connector's address info.
	socklen_t sinSize; 
	int on = 1;

	pthread_mutex_init(&threadRTagLock, NULL);
	// Client socket pool.
	//ClntSockPool* cspool = clntSockPoolAlloc(); // Deprecated.

	// Server socket to listen.
	if ((servSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		dieWithError("multiConnMultiThreadServer socket() failed");
	}

	if (setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0) {
		dieWithError("multiConnMultiThreadServer setsockopt() failed");
	}

	// Construct local address structure.
	servAddr.sin_family = AF_INET; // Host byte order.
	servAddr.sin_port = htons(servPort);
	servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(servAddr.sin_zero, '\0', sizeof(servAddr.sin_zero));

	if (bind(servSock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0) {
		dieWithError("multiConnMultiThreadServer bind() failed");
	}

	if (listen(servSock, MAXPENDING) < 0) {
		dieWithError("multiConnMultiThreadServer listen() failed");
	}
	printf("servPort: %d\n", servPort);

	fd_set fds;
	int maxsock = servSock;
	struct timeval timeout;

	sinSize = sizeof(clntAddr);

	
	while (1) {
		FD_ZERO(&fds);
		FD_SET(servSock, &fds);
		timeout.tv_sec = 30;
		timeout.tv_usec = 0;
		int ret = 0;
		if ((ret = select(maxsock+1, &fds, NULL, NULL, &timeout)) < 0) {
			dieWithError("multiConnMultiThreadServer select() failed");
		}
		else if (ret == 0) {
			printf("timeout\n");
			break;
		}

		//printf("tag1\n");
		if ((clntSock = accept(servSock, (struct sockaddr*) &clntAddr, &sinSize)) < 0) {
			dieWithError("multiConnMultiThreadServer accept() failed");
		}
		//pthread_mutex_lock(&cspool->poolLock);
		//cspool->pool[cspool->poolTop++] = clntSock;
		//pthread_mutex_unlock(&cspool->poolLock);
		
		Connection* conn = (Connection*) malloc(sizeof(Connection));
        conn->socketfd = clntSock;
        conn->serverAddress = servAddr;
        conn->clientAddress = clntAddr;
		pthread_t ntid;
		if (pthread_create(&ntid, NULL, threadReceiveConnection, conn) < 0) {
			dieWithError("multiConnMultiThreadServer pthread_create() failed");
		}
		//pthread_join(ntid, NULL);

	}
	
	//clntSockPoolRelease(cspool);
	close(servSock);

	exit(0);
}

void client() {
	int sock; // Socket descriptor.
	struct sockaddr_in servAddr; // Server address.
	char* package;

	unsigned short servPort = Paras.servPort;
	char* servIP = Paras.servIP;
	unsigned int pkgSize = Paras.pkgSize;	
	unsigned int interval = Paras.interval;


	printf("servIP: %s\n", servIP);
	

	// Create a reliable, stream socket using TCP.
	if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("L1 client socket() failed");
	}

	// Construct the server address structure.
	memset(&servAddr, 0, sizeof(servAddr)); // Zero out structure.
	servAddr.sin_family = AF_INET; // Internet address family.
	servAddr.sin_addr.s_addr = inet_addr(servIP); // Server IP address.
	servAddr.sin_port = htons(servPort); // Server port.

	// Establish the connection to the echo server.
	if (connect(sock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0) {
		dieWithError("L1 client connect() failed");
	}

	// [Test
	package = (char*) malloc(pkgSize * sizeof(char));
	int i = 0;
	package[0] = 's';
	for (i = 1; i <= pkgSize-3; i++) {
		package[i] = 'd';
	}
	package[pkgSize-2] = 'e';
	//package[pkgSize-1] = '\0';
	package[pkgSize-1] = 'e';
	printf("package size: %d\n", pkgSize);

	// CPU calculating.
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2;	
	pid_t pid = getpid();
	getWholeCPUStatus(&ps1);
	getProcessCPUStatus(&pps1, pid);
	
	// Time calculating.
	struct timeval t1, t2; // "struct" required when use gcc.
	gettimeofday(&t1, NULL);
	double timeSpan = 0.0;
	unsigned int sendTimes = 0;
	while (1) {
		if (send(sock, package, pkgSize, 0) != pkgSize) {
			dieWithError("L1 client send() send a different number of bytes than expected");
		}
		sendTimes++;
		
		
		gettimeofday(&t2, NULL);
		timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
		if (timeSpan >= interval) {
			break;
		}
	}

	getWholeCPUStatus(&ps2);
	getProcessCPUStatus(&pps2, pid);
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float processCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);
	printf("CPUUse: %f, processCPUUse: %f\n", CPUUse, processCPUUse);

	printf("send times: %d\n", sendTimes);
	unsigned long long int totalSendMsgSize = (unsigned long long int) sendTimes * pkgSize;
	printf("totalSendMsgSize: %lld Bytes\n", totalSendMsgSize);
	printf("time span: %lf s\n", timeSpan);
	double sendSpeed = ((double) sendTimes * pkgSize * 8) / (timeSpan * 1000 * 1000);
	printf("send speed: %lf Mb/s\n\n", sendSpeed);
	// Test]

	sleep(3);

	close(sock);

}

void l2Client() {
	printf("l2client\n");
	int preSock; // Socket descriptor for L1 client.	
	int localSock; // Socket descriptor for L2 client when recieving data from L1 client. 
	struct sockaddr_in preAddr; // L1 client address.
	struct sockaddr_in localAddr; // Local address.
	unsigned int clntLen; // Length of L1 client address data structure.
	unsigned short prePort = Paras.prePort; // L1 to L2 port.

	int nextSock; // Socket descriptor for server.
	struct sockaddr_in nextAddr; // Server address.
	unsigned short nextPort = Paras.servPort; // L2 to server port.
	char* nextIP = Paras.servIP; // Server IP.
	
	// [Listen to L1 client and receive data.
	// Create socket for incoming connections.
	if ((localSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("L2 client socket() failed");
	}

	// Construct local address structure.
	memset(&localAddr, 0, sizeof(localAddr)); // Zero out structure.
	localAddr.sin_family = AF_INET; // Internet address family.
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface.
	localAddr.sin_port = htons(prePort); // Local port.
	

	// Bind to the local address.
	if (bind(localSock, (struct sockaddr*) &localAddr, sizeof(localAddr)) < 0) {
		dieWithError("L2 client bind() faild");
	}

	// Mark the socket so it will listen for incoming connections.
	if (listen(localSock, MAXPENDING) < 0) {
		dieWithError("L2 client listen() failed");
	}
	
	char buffer[RCVBUFSIZE];
	bzero(buffer, RCVBUFSIZE);
	int recvMsgSize;
	clntLen = sizeof(preAddr);
	if ((preSock = accept(localSock, (struct sockaddr*) &preAddr, &clntLen)) < 0) {
			dieWithError("L2 client accept() failed");
	}

	// [Connect level 2 client to server.
	// Create a reliable, stream socket using TCP to link L2 client and server.
	if ((nextSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("L2 client socket() failed");
	}
	
	//printf("servIP: %s, port: %d, sock: %d\n", nextIP, nextPort, nextSock);

	// Construct the server address structure.
	memset(&nextAddr, 0, sizeof(nextAddr));
	nextAddr.sin_family = AF_INET;
	nextAddr.sin_addr.s_addr = inet_addr(nextIP);
	nextAddr.sin_port = htons(nextPort);

	// Establish the connection from L2 client to server.
	if (connect(nextSock, (struct sockaddr*) &nextAddr, sizeof(nextAddr)) < 0) {
		dieWithError("L2 client connect() failed");
	}	
	// ]

	printf("Handling data from L1 client: %s\n", inet_ntoa(preAddr.sin_addr));
	unsigned long long int totalRecvMsgSize = 0;
	unsigned long long int totalSendMsgSize = 0;
	
	// CPU calculating.
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2;	
	pid_t pid = getpid();
	getWholeCPUStatus(&ps1);
	getProcessCPUStatus(&pps1, pid);

	// Time calculating.
	struct timeval t1, t2;
	gettimeofday(&t1, NULL);
	double timeSpan = 0.0;

	while (1) {
		// Receive data from L1 client to L2 client.
		if ((recvMsgSize = recv(preSock, buffer, RCVBUFSIZE, 0)) < 0) {
			dieWithError("L2 client recv() failed");
		}
		else if (recvMsgSize > 0) {
			totalRecvMsgSize += recvMsgSize;
			//printf("recvMsgSize %d\n", recvMsgSize);
			//printf("totalRecvMsgSize: %lld\n", totalRecvMsgSize);
			//printf("%s\n", buffer);

			// Send data from L2 client to server.
			//int sendMsgSize = strlen(buffer);
			int sendMsgSize = recvMsgSize;
			if (send(nextSock, buffer, sendMsgSize, 0) != sendMsgSize) {
				dieWithError("L2 client send() send a different number of bytes than expected");
			}
			totalSendMsgSize += sendMsgSize;
			//printf("sendMsgSize: %d\n", sendMsgSize);
			//printf("totalSendMsgSize: %lld\n", totalSendMsgSize);

		}
		else { // recvMsgSize == 0
			break;
		}
		
	}

	getWholeCPUStatus(&ps2);
	getProcessCPUStatus(&pps2, pid);
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float processCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);
	printf("CPUUse: %f, processCPUUse: %f\n", CPUUse, processCPUUse);

	gettimeofday(&t2, NULL);
	timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
	double sendSpeed = ((double) totalSendMsgSize * 8) / (timeSpan * 1000 * 1000);
	printf("totalRecvMsgSize: %lld\n", totalRecvMsgSize);
	printf("totalSendMsgSize: %lld\n", totalSendMsgSize);
	printf("time span: %lf\n", timeSpan);
	printf("send speed(after receive): %lf Mb/s\n", sendSpeed);

	close(preSock);
	close(nextSock);

// ]	

	
}

void multiConnSingleThreadL2Client() {
	printf("multiConnSingleThreadL2Client\n");
	unsigned short prePort = Paras.prePort;

	int fdArr[MAXPENDING] = {0}; // Accepted connection fds.
	int connAmount; // Current connection amount.

	int localSock, preSock; // Listen on localSock, new connection on preSock.
	struct sockaddr_in localAddr; // Server address info.
	struct sockaddr_in preAddr; // connector's address info.
	socklen_t sinSize; 
	int on = 1;
	char buffer[RCVBUFSIZE];
	int ret;
	int i;

	int nextSock;
	struct sockaddr_in nextAddr;
	unsigned short nextPort = Paras.servPort;
	char* nextIP = Paras.servIP;

	// [Listen to L1 client and receive data.
	if ((localSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		dieWithError("multiConnSingleThreadL2Client socket() failed");
	}

	if (setsockopt(localSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0) {
		dieWithError("multiConnSingleThreadL2Client setsockopt() failed");
	}

	// Construct local address structure.
	localAddr.sin_family = AF_INET; // Host byte order.
	localAddr.sin_port = htons(prePort);
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(localAddr.sin_zero, '\0', sizeof(localAddr.sin_zero));

	if (bind(localSock, (struct sockaddr*) &localAddr, sizeof(localAddr)) < 0) {
		dieWithError("multiConnSingleThreadL2Client bind() failed");
	}

	if (listen(localSock, MAXPENDING) < 0) {
		dieWithError("multiConnSingleThreadL2Client listen() failed");
	}
	printf("prePort: %d\n", prePort);
	// ]
	
	// [Connect L2 client to server
	if ((nextSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("multiConnSingleThreadL2Client socket() failed");
	}

	memset(&nextAddr, 0, sizeof(nextAddr));
	nextAddr.sin_family = AF_INET;
	nextAddr.sin_addr.s_addr = inet_addr(nextIP);
	nextAddr.sin_port = htons(nextPort);

	if (connect(nextSock, (struct sockaddr*) &nextAddr, sizeof(nextAddr)) < 0) {
		dieWithError("multiConnSingleThreadL2Client connect() failed");
	}
	// ]

	fd_set fds;
	int maxsock;
	struct timeval timeout;

	connAmount = 0;
	sinSize = sizeof(preAddr);
	maxsock = localSock;

	unsigned long long int totalRecvMsgSize[MAXPENDING] = {0};
	unsigned long long int totalSendMsgSize[MAXPENDING] = {0};
	double timeSpan[MAXPENDING] = {0.0};
	float CPUUse[MAXPENDING] = {0.0};
	float processCPUUse[MAXPENDING] = {0.0};
	double sendSpeed[MAXPENDING] = {0.0};
	
	ProcStat ps1[MAXPENDING], ps2[MAXPENDING];
	ProcPidStat pps1[MAXPENDING], pps2[MAXPENDING];
	pid_t pid = getpid();

	struct timeval t1[MAXPENDING], t2[MAXPENDING];


	while (1) {
		// Init file descriptor set.
		FD_ZERO(&fds);
		FD_SET(localSock, &fds);

		// Timeout setting.
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		// Add active connection to fd set.
		for (i = 0; i < MAXPENDING; i++) {
			if (fdArr[i] != 0) {
				FD_SET(fdArr[i], &fds);
			}	
		}

		ret = select(maxsock+1, &fds, NULL, NULL, &timeout);
		if (ret < 0) {
			dieWithError("multiConnSingleThreadL2Client select() failed");
		}
		else if (ret == 0) {
			printf("timeout\n");
			break; // When timeout, jump out the loop and end the server.
			//continue; // When timeout, just continue to waiting for new connetions.
		}

		// Check every fd in the set.
		for (i = 0; i < MAXPENDING; i++) {
			if (FD_ISSET(fdArr[i], &fds)) {
				ret = recv(fdArr[i], buffer, RCVBUFSIZE, 0);
				if (ret < 0) {
					dieWithError("multiConnSingleThreadL2Client L2 client recv() failed");	
				}
				else if (ret > 0) {
					totalRecvMsgSize[i] += ret;

					// Receive data.
					//int sendMsgSize = strlen(buffer);
					int sendMsgSize = ret;
					if (send(nextSock, buffer, sendMsgSize, 0) != sendMsgSize) {
						dieWithError("multiConnSingleThreadL2Client L2 client send() send a different number of bytes that expected");
					}
					totalSendMsgSize[i] += sendMsgSize;

					//
					//if (ret < RCVBUFSIZE) {
					//	memset(&buffer[ret], '\0', 1);
					//}
					//
					//printf("client[%d] send: %c...\n", i, buffer[0]);
				}
				else { // ret == 0
					// Close client.
					printf("client[%d] close\n", i);
					close(fdArr[i]);
					connAmount--;
					FD_CLR(fdArr[i], &fds);
					fdArr[i] = 0;

					// time and CPU.
					gettimeofday(&t2[i], NULL);
					getWholeCPUStatus(&ps2[i]);
					getProcessCPUStatus(&pps2[i], pid);
					
					timeSpan[i] = (double) (t2[i].tv_sec-t1[i].tv_sec) + (double) t2[i].tv_usec*1e-6 - (double) t1[i].tv_usec*1e-6;
					sendSpeed[i] = ((double) totalRecvMsgSize[i] * 8) / (timeSpan[i] * 1000 *1000);
					CPUUse[i] = calWholeCPUUse(&ps1[i], &ps2[i]);
					processCPUUse[i] = calProcessCPUUse(&ps1[i], &pps1[i], &ps2[i], &pps2[i]);


				}
			}
		}

		// Check whether a new connection comes.
		if (FD_ISSET(localSock, &fds)) {
			preSock = accept(localSock, (struct sockaddr*) &preAddr, &sinSize);
			if (preSock <= 0) {
				dieWithError("multiConnSingleThreadL2Client accept() failed");
			}

			// Add to fd queue.
			if (connAmount < MAXPENDING) {
				for (i = 0; i < MAXPENDING; i++) {
					if (fdArr[i] == 0) {
						fdArr[i] = preSock;
						connAmount++;

						// time and CPU.
						gettimeofday(&t1[i], NULL);
						getWholeCPUStatus(&ps1[i]);
						getProcessCPUStatus(&pps1[i], pid);

						printf("new connection client[%d] %s:%d\n", i, inet_ntoa(preAddr.sin_addr), ntohs(preAddr.sin_port));
						break;
					}
				}

				if (preSock > maxsock) {
					maxsock = preSock;
				}
			}
			else {
				printf("connections limits\n");
				//send(preSock, "bye", 4, 0);
				close(preSock);
				//break; // Jump out while.
				continue;
			}
		}

		/*
		// Show clients.
		printf("\n\n");
		printf("client amount: %d\n", connAmount);
		for (i = 0; i < MAXPENDING; i++) {
			printf("[%d]:%d ", i, fdArr[i]);
		}
		printf("\n\n");
		*/

	}

	// Close other connections.
	for (i = 0; i < MAXPENDING; i++) {
		if (fdArr[i] != 0) {
			close(fdArr[i]);
			connAmount--;
		}
	} 

	for (i = 0; i < MAXPENDING; i++) {
		printf("connection %d \nCPUUse: %f, processCPUUse: %f\ntotalRecvMsgSize: %llu Bytes\ntimeSpan: %lf\nsendSpeed(after receive): %lf Mb/s\n\n", i, CPUUse[i], processCPUUse[i], totalRecvMsgSize[i], timeSpan[i], sendSpeed[i]);
	}

	exit(0);


}

unsigned long long int threadRSTag = 0;
pthread_mutex_t threadRSTagLock;
void* threadReceiveAndSend(void* arg) { // Deprecated.
	printf("threadReceiveAndSend\n");
	ClntSockPool* cspool = (ClntSockPool*) arg;
	int preSock = -1;
	while (1) {
		preSock = cspool->pool[cspool->poolTop-1];
		if (preSock != -1) {
			pthread_mutex_lock(&cspool->poolLock);
			cspool->poolTop--;
			pthread_mutex_unlock(&cspool->poolLock);
			break;
		}
	}


	// Get process id and thread id.
	pid_t pid = getpid();
    //pthread_t tid = pthread_self(); // Get tid, different from "syscall(SYS_gettid)".
    pid_t tid = gettid();
	printf("thread preSock: %d, pid: %u, tid: %u\n", preSock, (unsigned int) pid, (unsigned int) tid);


	// [Connect L2 client to server.
	int nextSock;
	struct sockaddr_in nextAddr;
	unsigned short nextPort = Paras.servPort;
	char* nextIP = Paras.servIP;

	if ((nextSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("threadReceiveAndSend socket() failed");
	}
	
	memset(&nextAddr, 0, sizeof(nextAddr));
	nextAddr.sin_family = AF_INET;
	nextAddr.sin_addr.s_addr = inet_addr(nextIP);
	nextAddr.sin_port = htons(nextPort);

	if (connect(nextSock, (struct sockaddr*) &nextAddr, sizeof(nextAddr)) < 0) {
		dieWithError("threadReceiveAndSend connect() failed");
	}
	// ]

	char buffer[RCVBUFSIZE];
	bzero(buffer, RCVBUFSIZE);

	int recvMsgSize;
	unsigned long long int totalRecvMsgSize = 0;
	unsigned long long int totalSendMsgSize = 0;

	// CPU calculating.
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2;
	getWholeCPUStatus(&ps1);
	getThreadCPUStatus(&pps1, pid, tid); // accurate?

	// Time calculating.
	struct timeval t1, t2;
	gettimeofday(&t1, NULL);
	double timeSpan = 0.0;

	while (1) {
		if ((recvMsgSize = recv(preSock, buffer, RCVBUFSIZE, 0)) < 0) {
			dieWithError("threadReceiveAndSend recv() failed");
		}
		else if (recvMsgSize > 0) {
			totalRecvMsgSize += recvMsgSize;
			//printf("thread %u recvMsgSize: %d\n", (unsigned int) tid, recvMsgSize);
            //printf("thread %u totalRecvMsgSize: %lld\n", (unsigned int) tid, totalRecvMsgSize);

			// Send data from L2 client to server.
			//int sendMsgSize = strlen(buffer);
			int sendMsgSize = recvMsgSize;
			if (send(nextSock, buffer, sendMsgSize, 0) != sendMsgSize) {
				dieWithError("threadReceiveAndSend send() a different number of bytes than expected");
			}
			totalSendMsgSize += sendMsgSize;
		}
		else {
			break;
		}
	}

	getWholeCPUStatus(&ps2);
	getThreadCPUStatus(&pps2, pid, tid);
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float threadCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);

	gettimeofday(&t2, NULL);
	timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
	double sendSpeed = ((double) totalSendMsgSize * 8) / (timeSpan * 1000 *1000);

	pthread_mutex_lock(&threadRSTagLock);
	threadRSTag++;
	printf("Tag: %llu\n", threadRSTag);
    pthread_mutex_unlock(&threadRSTagLock);
	printf("thread %d-%d CPUUse: %f, threadCPUUse: %f\n", pid, tid, CPUUse, threadCPUUse);
	printf("thread %d-%d totalRecvMsgSize: %llu Bytes\n", pid, tid, totalRecvMsgSize);
	printf("thread %d-%d totalSendMsgSize: %llu Bytes\n", pid, tid, totalSendMsgSize);
	printf("thread %d-%d time span: %lf\n", pid, tid, timeSpan);
	printf("thread %d-%d send speed(after receive): %lf Mb/s\n\n", pid, tid, sendSpeed);

	close(preSock);
	close(nextSock);
	pthread_exit((void*) 0);

	return ((void*) 0);
}

void* threadReceiveConnectionAndSend(void* arg) { 
	printf("threadReceiveAndSend\n");
	Connection* conn = (Connection*) arg;
	int preSock = conn->socketfd;


	// Get process id and thread id.
	pid_t pid = getpid();
    //pthread_t tid = pthread_self(); // Get tid, different from "syscall(SYS_gettid)".
    pid_t tid = gettid();
	printf("thread preSock: %d, pid: %u, tid: %u\n", preSock, (unsigned int) pid, (unsigned int) tid);


	// [Connect L2 client to server.
	int nextSock;
	struct sockaddr_in nextAddr;
	unsigned short nextPort = Paras.servPort;
	char* nextIP = Paras.servIP;

	if ((nextSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		dieWithError("threadReceiveAndSend socket() failed");
	}
	
	memset(&nextAddr, 0, sizeof(nextAddr));
	nextAddr.sin_family = AF_INET;
	nextAddr.sin_addr.s_addr = inet_addr(nextIP);
	nextAddr.sin_port = htons(nextPort);

	if (connect(nextSock, (struct sockaddr*) &nextAddr, sizeof(nextAddr)) < 0) {
		dieWithError("threadReceiveAndSend connect() failed");
	}
	// ]

	char buffer[RCVBUFSIZE];
	bzero(buffer, RCVBUFSIZE);

	int recvMsgSize;
	unsigned long long int totalRecvMsgSize = 0;
	unsigned long long int totalSendMsgSize = 0;

	// CPU calculating.
	ProcStat ps1, ps2;
	ProcPidStat pps1, pps2;
	getWholeCPUStatus(&ps1);
	getThreadCPUStatus(&pps1, pid, tid); // accurate?

	// Time calculating.
	struct timeval t1, t2;
	gettimeofday(&t1, NULL);
	double timeSpan = 0.0;

	while (1) {
		if ((recvMsgSize = recv(preSock, buffer, RCVBUFSIZE, 0)) < 0) {
			dieWithError("threadReceiveAndSend recv() failed");
		}
		else if (recvMsgSize > 0) {
			totalRecvMsgSize += recvMsgSize;
			//printf("thread %u recvMsgSize: %d\n", (unsigned int) tid, recvMsgSize);
            //printf("thread %u totalRecvMsgSize: %lld\n", (unsigned int) tid, totalRecvMsgSize);

			// Send data from L2 client to server.
			//int sendMsgSize = strlen(buffer);
			int sendMsgSize = recvMsgSize;
			if (send(nextSock, buffer, sendMsgSize, 0) != sendMsgSize) {
				dieWithError("threadReceiveAndSend send() a different number of bytes than expected");
			}
			totalSendMsgSize += sendMsgSize;
		}
		else {
			break;
		}
	}

	getWholeCPUStatus(&ps2);
	getThreadCPUStatus(&pps2, pid, tid);
	float CPUUse = calWholeCPUUse(&ps1, &ps2);
	float threadCPUUse = calProcessCPUUse(&ps1, &pps1, &ps2, &pps2);

	gettimeofday(&t2, NULL);
	timeSpan = (double) (t2.tv_sec-t1.tv_sec) + (double) t2.tv_usec*1e-6 - (double) t1.tv_usec*1e-6;
	double sendSpeed = ((double) totalSendMsgSize * 8) / (timeSpan * 1000 *1000);

	pthread_mutex_lock(&threadRSTagLock);
	threadRSTag++;
	printf("Tag: %llu\n", threadRSTag);
    pthread_mutex_unlock(&threadRSTagLock);
	printf("thread %d-%d CPUUse: %f, threadCPUUse: %f\n", pid, tid, CPUUse, threadCPUUse);
	printf("thread %d-%d totalRecvMsgSize: %llu Bytes\n", pid, tid, totalRecvMsgSize);
	printf("thread %d-%d totalSendMsgSize: %llu Bytes\n", pid, tid, totalSendMsgSize);
	printf("thread %d-%d time span: %lf\n", pid, tid, timeSpan);
	printf("thread %d-%d send speed(after receive): %lf Mb/s\n\n", pid, tid, sendSpeed);

	close(preSock);
	close(nextSock);
	free(conn);
	pthread_exit((void*) 0);

	return ((void*) 0);
}

void multiConnMultiThreadL2Client() {
	printf("multiConnMultiThreadL2Client\n");
	unsigned short prePort = Paras.prePort;

	int localSock, preSock; // Listen on localSock, new connection on preSock.
	struct sockaddr_in localAddr; // local address info.
	struct sockaddr_in preAddr; // connector's address info.
	socklen_t sinSize; 
	int on = 1;
	
	pthread_mutex_init(&threadRSTagLock, NULL);
	// Pre client socket pool.
	//ClntSockPool* cspool = clntSockPoolAlloc(); // Deprecated.

	if ((localSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		dieWithError("multiConnMultiThreadL2Client socket() failed");
	}

	if (setsockopt(localSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) < 0) {
		dieWithError("multiConnMultiThreadL2Client setsockopt() failed");
	}

	// Construct local address structure.
	localAddr.sin_family = AF_INET; // Host byte order.
	localAddr.sin_port = htons(prePort);
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	memset(localAddr.sin_zero, '\0', sizeof(localAddr.sin_zero));

	if (bind(localSock, (struct sockaddr*) &localAddr, sizeof(localAddr)) < 0) {
		dieWithError("multiConnMultiThreadL2Client bind() failed");
	}

	if (listen(localSock, MAXPENDING) < 0) {
		dieWithError("multiConnMultiThreadL2Client listen() failed");
	}
	printf("prePort: %d\n", prePort);

	fd_set fds;
	int maxsock = localSock;
	struct timeval timeout;

	sinSize = sizeof(preAddr);


	while (1) {
		FD_ZERO(&fds);
		FD_SET(localSock, &fds);
		timeout.tv_sec = 30;
		timeout.tv_usec = 0;
		int ret = 0;
		if ((ret = select(maxsock+1, &fds, NULL, NULL, &timeout)) < 0) {
			dieWithError("multiConnMultiThreadL2Client select() failed");
		}
		else if (ret == 0) {
			printf("timeout\n");
			break;
		}

		if ((preSock = accept(localSock, (struct sockaddr*) &preAddr, &sinSize)) < 0) {
			dieWithError("multiConnMultiThreadL2Client accept() failed");
		}
		//pthread_mutex_lock(&cspool->poolLock);
		//cspool->pool[cspool->poolTop++] = preSock;
		//pthread_mutex_unlock(&cspool->poolLock);
		
		Connection* conn = (Connection*) malloc(sizeof(Connection));
		conn->socketfd = preSock;
		conn->serverAddress = localAddr;
		conn->clientAddress = preAddr;
		pthread_t ntid;
		if (pthread_create(&ntid, NULL, threadReceiveConnectionAndSend, conn) < 0) {
			dieWithError("multiConnMultiThreadL2Client pthread_create() failed");
		}
		//pthread_join(ntid, NULL);


	}
	
	//clntSockPoolRelease(cspool);
	close(localSock);

	exit(0);

}

int main(int argc, char* argv[]) {
	Paras.servPort = 5555;
	Paras.isServer = 1;
	Paras.servIP = (char*) "127.0.0.1"; 
	Paras.interval = 10;
	Paras.prePort = 6666;
	Paras.serverType = 3;
	Paras.clientType = 4;

	int i = 1;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			Paras.isServer = 0;
			i++;
			Paras.clientType = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "-s") == 0) {
			Paras.isServer = 1;
			i++;
			Paras.serverType = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "-a") == 0) {
			i++;
			Paras.servIP = argv[i];
		}
		else if (strcmp(argv[i], "-p") == 0) {
			i++;
			Paras.servPort = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "-P") == 0) {
			i++;
			Paras.prePort = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "-size") == 0) {
			i++;
			Paras.pkgSize = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "-t") == 0) {
			i++;
			Paras.interval = atoi(argv[i]);
		}
		else if (strcmp(argv[i], "--help") == 0) {
			printUsage();
			return 0;
		}
		else {

		}
	}

	if (Paras.isServer) {
		if (Paras.serverType == DefaultServer) {
			server();
		}
		else if (Paras.serverType == MultiConnSingleThreadServer) {
			multiConnSingleThreadServer();
		}
		else if (Paras.serverType == MultiConnMultiThreadServer) {
			multiConnMultiThreadServer();
		}
	}
	else {
		if (Paras.clientType == L1Client) {
			client();
		}
		else if (Paras.clientType == L2Client) {
			l2Client();
		}
		else if (Paras.clientType == MultiConnSingleThreadL2Client) {
			multiConnSingleThreadL2Client();
		}
		else if (Paras.clientType == MultiConnMultiThreadL2Client) {
			multiConnMultiThreadL2Client();
		}
	}

	return 0;
}

