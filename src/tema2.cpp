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
#define MAX_FILENAME 15
#define HASH_SIZE 32

#define DIE(assertion, call_description)				\
	do {								\
		if (assertion) {					\
			fprintf(stderr, "(%s, %d): ",			\
					__FILE__, __LINE__);		\
			perror(call_description);			\
			exit(errno);					\
		}							\
	} while (0)

mutex owned_files_mutex;

/********************** Data for the client ****************************/

unordered_map<string, list<pair<string, bool>>> owned_files;
unordered_map<string, bool> wanted_files;

void init_client(int rank)
{
    // Reading data from the client file
    string infile = "in" + to_string(rank) + ".txt";
    ifstream fin(infile.c_str());

    int num_files, num_segments;
    string filename, segment_hash;
    fin >> num_files;
    MPI_Send(&num_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    for (int i = 0; i < num_files; ++i) {
        fin >> filename >> num_segments;

        int size = filename.length();
        // Sending the file name to the tracker, fingerprint for the file names
        MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Send(filename.c_str(), size, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Send(&num_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (int j = 0; j < num_segments; ++j) {
            fin >> segment_hash;

            // Sending the hash of the segment to the tracker
            MPI_Send(segment_hash.c_str(), HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

            owned_files[filename].emplace_back(segment_hash, true);
        }
    }

    // Wanted files
    fin >> num_files;
    for (int i = 0; i < num_files; ++i) {
        fin >> filename;
        wanted_files[filename] = false;
    }

    fin.close();

    // Receiving message from the tracker
    char ack[4] = {0};
    MPI_Recv(ack, 3, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    DIE(strcmp(ack, "ACK") != 0, "The tracker did not send ACK");
}

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;
    init_client(rank);

    return NULL;
}

void *upload_thread_func(void *arg)
{

    return NULL;
}

/********************** Data for the tracker ****************************/

unordered_map<string, list<string>> hash_db;
unordered_map<string, list<int>> seed_db;
unordered_map<string, list<int>> peer_db;

void init_tracker(int numtasks, int rank) {
    int num_files = 0, size = 0, num_segments = 0;
    char buffer1[MAX_FILENAME + 1] = {0}, buffer2[HASH_SIZE + 1] = {0};

    for (int i = 1; i < numtasks; i++) {
        // number of files
        MPI_Recv(&num_files, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < num_files; j++) {
            // file name size
            MPI_Recv(&size, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // file name
            MPI_Recv(buffer1, size, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            seed_db[buffer1].push_back(i);

            // number of segments
            MPI_Recv(&num_segments, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int k = 0; k < num_segments; k++) {
                // segment
                MPI_Recv(buffer2, HASH_SIZE, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                hash_db[buffer1].push_back(buffer2);
            }

            memset(buffer1, '\0', MAX_FILENAME + 1);
            memset(buffer2, '\0', HASH_SIZE + 1);
        }
    }

    char ack[4] = "ACK";
    // Sending the OK message to nodes
    for (int i = 1; i < numtasks; i++)
        MPI_Send(ack, 3, MPI_CHAR, i, 0, MPI_COMM_WORLD);
}

void tracker(int numtasks, int rank) {
    init_tracker(numtasks, rank);
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;

    DIE(pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank), "pthread_create");
    DIE(pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank), "pthread_create");
    DIE(pthread_join(download_thread, &status), "pthread_join");
    DIE(pthread_join(upload_thread, &status), "pthread_join");
}

int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
