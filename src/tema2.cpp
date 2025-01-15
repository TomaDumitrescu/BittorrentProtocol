#include <iostream>
#include <fstream>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <list>
#include <sstream>
#include <mutex>

#define TRACKER_RANK 0
#define MAX_FILES 5
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define TAG_LIMIT 5000
#define STOP_TAG 100000
#define MAX_HOSTS 20

using namespace std;

int tag = 0;
mutex owned_files_mutex;
char stop[5] = {0};

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
    MPI_Send(&num_files, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
    for (int i = 0; i < num_files; ++i) {
        fin >> filename >> num_segments;

        int size = filename.length();
        // Sending the file name to the tracker, fingerprint for the file names
        MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
        MPI_Send(filename.c_str(), size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
        MPI_Send(&num_segments, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);

        for (int j = 0; j < num_segments; ++j) {
            fin >> segment_hash;

            // Sending the hash of the segment to the tracker
            MPI_Send(segment_hash.c_str(), HASH_SIZE, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD);

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
    MPI_Recv(ack, 3, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (strcmp(ack, "ACK") != 0) {
        printf("The tracker did not send ACK");
        exit(-1);
    }
}

void request_file(int &tag, string fname, char *buffer, int *hosts_requests,
                    unordered_map<string, list<pair<string, bool>>> &downloaded_files,
                    unordered_map<string, list<int>> hosts, bool update)
{
    string msg = "request_file " + fname;
    if (update)
        msg = "update_file" + fname;
    const char *wanted_file = msg.c_str();

    int size = strlen(wanted_file);

    // size of the wanted filename
    MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
    // sending the wanted filename with its command
    MPI_Send(wanted_file, size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD);

    if (!update) {
        // waiting for the segments list
        MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(buffer, size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // marking the segments as false
        string segments(buffer), seg;
        istringstream split(segments);
        while (split >> seg) {
            downloaded_files[fname].emplace_back(seg, false);

            unique_lock<mutex> lock(owned_files_mutex);
            owned_files[fname].emplace_back(seg, false);
            lock.unlock();
        }
    }

    int host = 0;
    MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    for (int j = 0; j < size; j++) {
        MPI_Recv(&host, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (hosts_requests[host] == -1) {
            hosts[fname].push_back(host);
            hosts_requests[host] = 0;
        }
    }
}

void save_file(string fname, unordered_map<string, list<pair<string, bool>>> &downloaded_files, int rank)
{
    string out_file = "client" + to_string(rank) + "_" + fname;
    ofstream client_out(out_file.c_str());

    for (const auto& element: downloaded_files[fname])
        client_out << element.first << '\n';

    client_out.close();
}

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg, ok = 0;
    init_client(rank);

    unordered_map<string, list<pair<string, bool>>> downloaded_files;
    unordered_map<string, list<int>> hosts;

    // index = host, value = number of requests to that host
    int host_requests[MAX_HOSTS] = {0}, num_segments = 0, size;
    bool valid[MAX_HOSTS] = {0};

    char buffer[3600] = {0};
    for (const auto &element: wanted_files) {
        // Hosts initialization
        for (int i = 0; i < MAX_HOSTS; i++)
            host_requests[i] = -1;

        /////// 1. FINDING NECESSARY HASHES AND THE LIST OF HOSTS ///////
        memset(buffer, '\0', 3600);
        request_file(tag, element.first, buffer, host_requests, downloaded_files, hosts, false);

        num_segments = 0;
        /////// 2. REQUESTING THE SEGMENTS ///////
        for (const auto& segs : downloaded_files[element.first]) {
            string msg = "check " + element.first + " " + segs.first;
            size = msg.length();

            // reset segment validity vector
            for (int i = 0; i < MAX_HOSTS; i++)
                valid[i] = false;

            // Verify which hosts have the current segment
            for (int i = 0; i < MAX_HOSTS; i++) {
                if (host_requests[i] < 0)
                    continue;

                // send message size
                MPI_Send(&size, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
                // sending the wanted filename with its command
                MPI_Send(msg.c_str(), size, MPI_CHAR, i, rank, MPI_COMM_WORLD);
                // receive validity
                MPI_Recv(&ok, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                valid[i] = (bool)ok;
            }

            int optimal_id = -1, min_requests = 1000000;
            // Search the optimal valid source
            for (int i = 0; i < MAX_HOSTS; i++)
                if (host_requests[i] >= 0 && host_requests[i] <= min_requests && valid[i]) {
                    optimal_id = i;
                    min_requests = host_requests[i];
                }

            if (optimal_id <= 0) {
                printf("Error in finding a seed. Aborting...\n");
                exit(-1);
            }

            // Simulate installing (this way the validity request is NOT inefficient)
            msg = "install " + element.first + " " + segs.first;
            size = msg.length();
            // send message size
            MPI_Send(&size, 1, MPI_INT, optimal_id, 0, MPI_COMM_WORLD);
            // sending the wanted segment with its command
            MPI_Send(msg.c_str(), size, MPI_CHAR, optimal_id, rank, MPI_COMM_WORLD);
            // receive validity
            MPI_Recv(&ok, 1, MPI_INT, optimal_id, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (ok == 0) {
                printf("Failed to install a valid segment!\n");
                exit(-1);
            }

            host_requests[optimal_id]++;

            // Simulate storage for that segment
            for (auto& e: downloaded_files[element.first]) {
                string hash = e.first;
                if (hash.compare(0, HASH_SIZE, segs.first, 0, HASH_SIZE) == 0)
                    e.second = true;
            }

            unique_lock<mutex> lock(owned_files_mutex);
            for (auto& e: owned_files[element.first]) {
                string hash = e.first;
                if (hash.compare(0, HASH_SIZE, segs.first, 0, HASH_SIZE) == 0)
                    e.second = true;
            }
            lock.unlock();

            num_segments++;
            // Update the host list
            if (num_segments % 10 == 0) {
                memset(buffer, '\0', 3600);
                request_file(tag, element.first, buffer, host_requests, downloaded_files, hosts, true);
            }
        }

        /////// 3. NOTIFY THE TRACKER FOR A COMPLETING A FILE ///////
        // size of the notifying message
        string msg = "notify " + element.first;
        size = msg.length();
        MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
        // sending the notifying command for a single file
        MPI_Send(msg.c_str(), size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD);

        // waiting for the confirmation message from the tracker
        MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        memset(buffer, '\0', 3600);
        MPI_Recv(buffer, size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        if (strcmp(buffer, "NACK") == 0)
            printf("Error in notifying the tracker");

        // Print the downloaded content
        save_file(element.first, downloaded_files, rank);
    }

    //////// 4. NOTIFY THE TRACKER FOR COMPLETING ALL FILES ///////
    string final_message = "notify ALL";
    size = final_message.length();
    MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD);
    // sending the final notify message from the download client
    MPI_Send(final_message.c_str(), size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD);

    // waiting for the confirmation message from the tracker
    MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    memset(buffer, '\0', 3000);
    MPI_Recv(buffer, size, MPI_CHAR, TRACKER_RANK, ++tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (strcmp(buffer, "NACK") == 0)
        printf("Error in notifying the tracker");

    return NULL;
}

void check_segment(int ok, int dest) {
    MPI_Send(&ok, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
}

void install_segment(int ok, int dest) {
    MPI_Send(&ok, 1, MPI_INT, dest, 0, MPI_COMM_WORLD);
}

void *upload_thread_func(void *arg)
{
    int size = 0;
    char buffer[MAX_CHUNKS] = {0};

    while (true) {
        if (strcpy(stop, "stop") == 0)
            break;

        MPI_Status status1, status2;
        MPI_Recv(&size, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status1);
        memset(buffer, '\0', MAX_CHUNKS);
        MPI_Recv(buffer, size, MPI_CHAR, MPI_ANY_SOURCE, status1.MPI_SOURCE, MPI_COMM_WORLD, &status2);
        char *cmd = strtok(buffer, " "), *fname = strtok(NULL, " "), *hash = strtok(NULL, " ");
        string filename(fname), segment(hash);
        int ok = 0;
        unique_lock<mutex> lock(owned_files_mutex);
        for (const auto& e: owned_files[fname])
            if (e.first.compare(0, HASH_SIZE, segment, 0, HASH_SIZE) == 0 && e.second)
                ok = 1;
        lock.unlock();
        if (strcmp(cmd, "check") == 0) {
            check_segment(ok, status1.MPI_SOURCE);
        } else if (strcmp(cmd, "install") == 0) {
            install_segment(ok, status1.MPI_SOURCE);
        }
    }

    return NULL;
}

/********************** Data for the tracker ****************************/

unordered_map<string, list<string>> hash_db;
unordered_map<string, list<int>> seed_db;
unordered_map<string, list<int>> peer_db;
int *expected_tags;

void init_tracker(int numtasks, int rank) {
    int num_files = 0, size = 0, num_segments = 0;
    char buffer1[MAX_FILENAME + 1] = {0}, buffer2[HASH_SIZE + 1] = {0};

    expected_tags = (int *)calloc(numtasks + 1, sizeof(int));
    if (!expected_tags) {
        printf("Calloc failed!\n");
        exit(-1);
    }

    for (int i = 1; i < numtasks; i++) {
        expected_tags[i] = i * TAG_LIMIT;
        // number of files
        MPI_Recv(&num_files, 1, MPI_INT, i, ++expected_tags[i], MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < num_files; j++) {
            // file name size
            MPI_Recv(&size, 1, MPI_INT, i, ++expected_tags[i], MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // file name
            MPI_Recv(buffer1, size, MPI_CHAR, i, ++expected_tags[i], MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            seed_db[buffer1].push_back(i);

            // number of segments
            MPI_Recv(&num_segments, 1, MPI_INT, i, ++expected_tags[i], MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int k = 0; k < num_segments; k++) {
                // segment
                MPI_Recv(buffer2, HASH_SIZE, MPI_CHAR, i, ++expected_tags[i], MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                hash_db[buffer1].push_back(buffer2);
            }

            memset(buffer1, '\0', MAX_FILENAME + 1);
            memset(buffer2, '\0', HASH_SIZE + 1);
        }
    }

    char ack[4] = "ACK";
    // Sending the OK message to nodes
    for (int i = 1; i < numtasks; i++)
        MPI_Send(ack, 3, MPI_CHAR, i, ++expected_tags[i], MPI_COMM_WORLD);
}

void send_sources(int client, string fname) {
    int size = seed_db[fname].size() + peer_db[fname].size();
    for (const auto& e: seed_db[fname])
        if (e == client)
            size--;
    for (const auto& e: peer_db[fname])
        if (e == client)
            size--;
    
    MPI_Send(&size, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD);
    for (const auto& e: seed_db[fname]) {
        if (e == client)
            continue;
        MPI_Send(&e, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD);
    }

    for (const auto& e: peer_db[fname]) {
        if (e == client)
            continue;
        MPI_Send(&e, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD);
    }
}

void tracker(int numtasks, int rank) {
    init_tracker(numtasks, rank);

    int size = 0, num_done = 0;
    char buffer[3600] = {0};
    bool done[MAX_HOSTS] = {0};
    while (true) {
        if (num_done >= numtasks - 1)
            break;

        // handle clients requests
        for (int client = 1; client < numtasks; client++) {
            if (done[client])
                continue;

            // Receiving the message
            MPI_Recv(&size, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            memset(buffer, '\0', 3600);
            MPI_Recv(buffer, size, MPI_CHAR, client, ++expected_tags[client], MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // Parsing the message
            char *cmd = strtok(buffer, " "), *data = strtok(NULL, " ");
            if (strcmp(cmd, "request_file") == 0) {
                string fname(data);
                peer_db[fname].push_back(client);

                // Obtain the segments of the file
                string segments;
                for (const auto& seg: hash_db[fname])
                    segments += seg + " ";

                segments.pop_back();

                size = segments.length();
                // Send the segments
                MPI_Send(&size, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD);
                MPI_Send(segments.c_str(), size, MPI_CHAR, client, ++expected_tags[client], MPI_COMM_WORLD);

                // Send the peers and seeds list
                send_sources(client, fname);
            } else if (strcmp(cmd, "update_file")) {
                string fname(data);
                send_sources(client, fname);
            } else if (strcmp(cmd, "notify") == 0) {
                if (strcmp(data, "ALL") == 0) {
                    done[client] = true;
                    num_done++;
                } else {
                    string fname(data);
                    seed_db[fname].push_back(client);
                }

                char ack[4] = "ACK";
                size = strlen(ack);
                MPI_Send(&size, 1, MPI_INT, client, ++expected_tags[client], MPI_COMM_WORLD);
                MPI_Send(ack, size, MPI_CHAR, client, ++expected_tags[client], MPI_COMM_WORLD);
            } else {
                printf("Unknown command from %d\n", client);
            }
        }
    }

    strcpy(stop, "stop");
    MPI_Bcast(stop, strlen(stop), MPI_CHAR, STOP_TAG, MPI_COMM_WORLD);
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
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

    tag = TAG_LIMIT * rank;

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
