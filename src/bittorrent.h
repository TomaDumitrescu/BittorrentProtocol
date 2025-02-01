#ifndef BITTORRENT_H
#define BITTORRENT_H

#include <iostream>
#include <fstream>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <list>
#include <mutex>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILENAME 50
#define HASH_SIZE 32
#define MAX_HOSTS 10
#define TRACKER_TAG 0
#define SENDTO_UPLOAD_TAG 100
#define RECVFROM_UPLOAD_TAG 200
#define ACK_TAG 300
#define MSG_LEN 3600
#define INF 1000000

#define DIE(assertion, call_description)				\
	do {								\
		if (assertion) {					\
			fprintf(stderr, "(%s, %d): ",			\
					__FILE__, __LINE__);		\
			perror(call_description);			\
			exit(errno);					\
		}							\
	} while (0)

// Data for clients
mutex owned_files_mutex;
unordered_map<string, bool> wanted_files;
unordered_map<string, bool> owned_files;

int num_tasks = 0;

// Data for the tracker
unordered_map<string, list<string>> hash_db;
unordered_map<string, list<int>> seed_db;
unordered_map<string, list<int>> peer_db;
unordered_map<string, bool> registered_hashes;

#endif