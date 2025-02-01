#include "bittorrent.h"

void send_message(string command, int dest, int tag) {
    int length = command.size();
    MPI_Send(&length, 1, MPI_INT, dest, tag, MPI_COMM_WORLD);
    MPI_Send(command.c_str(), length, MPI_CHAR, dest, tag + 1, MPI_COMM_WORLD);
}

int receive_message(char *buffer, int tag) {
    int len = 0;
    MPI_Status status;
    MPI_Recv(&len, 1, MPI_INT, MPI_ANY_SOURCE, tag, MPI_COMM_WORLD, &status);
    memset(buffer, '\0', (len + 3 < MSG_LEN)? len + 3 : MSG_LEN - 1);
    MPI_Recv(buffer, len, MPI_CHAR, status.MPI_SOURCE, tag + 1, MPI_COMM_WORLD, &status);
    return status.MPI_SOURCE;
}

void init_client(int rank) {
    // Reading data from the client file
    string infile = "in" + to_string(rank) + ".txt";
    ifstream fin(infile.c_str());

    int num_files = 0, num_segments = 0, tag = TRACKER_TAG;
    string filename, segment_hash;
    fin >> num_files;
    MPI_Send(&num_files, 1, MPI_INT, TRACKER_RANK, tag++, MPI_COMM_WORLD);
    for (int i = 0; i < num_files; ++i) {
        fin >> filename >> num_segments;

        int size = filename.length();
        // Sending the file name to the tracker, fingerprint for the file names
        MPI_Send(&size, 1, MPI_INT, TRACKER_RANK, tag++, MPI_COMM_WORLD);
        MPI_Send(filename.c_str(), size, MPI_CHAR, TRACKER_RANK, tag++, MPI_COMM_WORLD);
        MPI_Send(&num_segments, 1, MPI_INT, TRACKER_RANK, tag++, MPI_COMM_WORLD);

        for (int j = 0; j < num_segments; ++j) {
            fin >> segment_hash;

            // Sending the hash of the segment to the tracker
            MPI_Send(segment_hash.c_str(), HASH_SIZE, MPI_CHAR, TRACKER_RANK, tag++, MPI_COMM_WORLD);
            string key = filename + " " + segment_hash;
            owned_files[key] = true;
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
    MPI_Recv(ack, 3, MPI_CHAR, TRACKER_RANK, ACK_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    DIE(strncmp(ack, "ACK", 3) != 0, "The tracker did not send ACK");
}

void save_file(int id, string filename, unordered_map<string, list<pair<string, bool>>> *downloaded) {
    string out_name = "client" + to_string(id) + "_" + filename;
    ofstream out(out_name.c_str());
    bool first = true;
    for (const auto& e : (*downloaded)[filename]) {
        if (!first)
            out << "\n";

        out << e.first;
        first = false;
    }

    out.close();
}

void *download_thread_func(void *arg) {
    int rank = *(int*) arg;
    init_client(rank);

    unordered_map<string, list<pair<string, bool>>>* downloaded_files =
            new unordered_map<string, list<pair<string, bool>>>();
    DIE(!downloaded_files, "alloc failed");

    int *host_requests = new int[MAX_HOSTS];
    char *buffer = new char[MSG_LEN];
    bool *valid = new bool[MAX_HOSTS];
    for (const auto &element: wanted_files) {
        for (int i = 0; i < num_tasks; i++)
            host_requests[i] = -1;

        // 1. Finding the list of available hosts
        string command = "request_file " + element.first;
        send_message(command, TRACKER_RANK, TRACKER_TAG);
        receive_message(buffer, TRACKER_TAG);

        char *token = strtok(buffer, " ");
        while (token) {
            string seg(token);
            string key = element.first + " " + token;
            (*downloaded_files)[element.first].emplace_back(seg, false);
            owned_files[key] = false;
            token = strtok(NULL, " ");
        }

        int host = 0, size = 0;
        MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, TRACKER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for (int j = 0; j < size; j++) {
            MPI_Recv(&host, 1, MPI_INT, TRACKER_RANK, TRACKER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (host_requests[host] == -1)
                host_requests[host] = 0;
        }

        // 2. Requesting the segments
        int num_segments = 0;
        for (const auto& segs : (*downloaded_files)[element.first]) {
            string msg = "check " + element.first + " " + segs.first;
            for (int i = 0; i < num_tasks; i++)
                valid[i] = false;

            int ok = 0;
            for (int i = 1; i < num_tasks; i++) {
                if (host_requests[i] < 0 || i == rank)
                    continue;

                send_message(msg, i, SENDTO_UPLOAD_TAG);
                MPI_Recv(&ok, 1, MPI_INT, i, RECVFROM_UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                valid[i] = (bool)ok;
            }

            int optimal_id = -1, min_requests = INF;
            for (int i = 1; i < num_tasks; i++)
                if (i != rank && host_requests[i] >= 0 && host_requests[i] <= min_requests && valid[i]) {
                    optimal_id = i;
                    min_requests = host_requests[i];
                }

            // 3. Intalling the segments from the optimal node
            DIE(optimal_id == -1, "optimal_id");
            msg = "install " + element.first + " " + segs.first;
            send_message(msg, optimal_id, SENDTO_UPLOAD_TAG);
            MPI_Recv(&ok, 1, MPI_INT, optimal_id, RECVFROM_UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            host_requests[optimal_id]++;

            unique_lock<mutex> lock(owned_files_mutex);
            string key = element.first + " " + segs.first;
            owned_files[key] = true;
            lock.unlock();

            num_segments++;
            // 4. Request an updated swarm
            if (num_segments % 10 == 0) {
                command = "update " + element.first;
                send_message(command, TRACKER_RANK, TRACKER_TAG);

                host = 0, size = 0;
                MPI_Recv(&size, 1, MPI_INT, TRACKER_RANK, TRACKER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                for (int j = 0; j < size; j++) {
                    MPI_Recv(&host, 1, MPI_INT, TRACKER_RANK, TRACKER_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    if (host_requests[host] == -1)
                        host_requests[host] = 0;
                }
            }
        }

        command = "downloaded " + element.first;
        wanted_files[element.first] = true;
        send_message(command, TRACKER_RANK, TRACKER_TAG);
        save_file(rank, element.first, downloaded_files);
    }

    string notify_tracker = "done";
    send_message(notify_tracker, TRACKER_RANK, TRACKER_TAG);

    delete[] host_requests;
    delete[] valid;
    delete[] buffer;
    delete downloaded_files;

    return NULL;
}

void check_segment(int ok, int dest) {
    MPI_Send(&ok, 1, MPI_INT, dest, RECVFROM_UPLOAD_TAG, MPI_COMM_WORLD);
}

void upload_segment(int ok, int dest) {
    MPI_Send(&ok, 1, MPI_INT, dest, RECVFROM_UPLOAD_TAG, MPI_COMM_WORLD);
}

void *upload_thread_func(void *arg) {
    char buffer[MSG_LEN] = {0};
    while (true) {
        int source = receive_message(buffer, SENDTO_UPLOAD_TAG), ok = 0;
        char *cmd = strtok(buffer, " "), *fname = strtok(NULL, " "), *hash = strtok(NULL, " ");
        string filename(fname), segment(hash);

        unique_lock<mutex> lock(owned_files_mutex);
        string key = filename + " " + segment;
        ok = owned_files[key];
        lock.unlock();

        if (strncmp(cmd, "check", 5) == 0)
            check_segment(ok, source);
        else if (strncmp(cmd, "install", 7) == 0)
            upload_segment(ok, source);
        else if (strncmp(cmd, "close", 5) == 0)
            break;
    }

    return NULL;
}

void init_tracker(int numtasks, int rank) {
    int num_files = 0, size = 0, num_segments = 0;
    char buffer1[MAX_FILENAME + 1] = {0}, buffer2[HASH_SIZE + 1] = {0};

    for (int i = 1; i < numtasks; i++) {
        int tag = 0;
        // number of files
        MPI_Recv(&num_files, 1, MPI_INT, i, tag++, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        for (int j = 0; j < num_files; j++) {
            // file name size
            MPI_Recv(&size, 1, MPI_INT, i, tag++, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // file name
            MPI_Recv(buffer1, size, MPI_CHAR, i, tag++, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            seed_db[buffer1].push_back(i);

            // number of segments
            MPI_Recv(&num_segments, 1, MPI_INT, i, tag++, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            string key;
            for (int k = 0; k < num_segments; k++) {
                // segment
                MPI_Recv(buffer2, HASH_SIZE, MPI_CHAR, i, tag++, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                key = string(buffer1) + " " + string(buffer2);
                if (registered_hashes.find(key) == registered_hashes.end()) {
                    hash_db[buffer1].push_back(buffer2);
                    registered_hashes[key] = true;
                }
            }

            memset(buffer1, '\0', MAX_FILENAME + 1);
            memset(buffer2, '\0', HASH_SIZE + 1);
        }
    }

    char ack[4] = "ACK";
    // Sending the OK message to nodes
    for (int i = 1; i < numtasks; i++)
        MPI_Send(ack, 3, MPI_CHAR, i, ACK_TAG, MPI_COMM_WORLD);
}

void tracker(int numtasks, int rank) {
    init_tracker(numtasks, rank);

    char buffer[MSG_LEN] = {0};
    int done = 0;
    while (true) {
        int source = receive_message(buffer, TRACKER_TAG);

        if (strncmp(buffer, "request_file", 12) == 0) {
            strtok(buffer, " ");
            string file(strtok(NULL, " "));
            peer_db[file].push_back(source);

            string segments = "";
            for (const auto& seg: hash_db[file])
                segments += seg + " ";

            send_message(segments, source, TRACKER_TAG);
            segments.pop_back();

            int size = seed_db[file].size() + peer_db[file].size();
            unordered_map<int, bool> seeds;
            for (const auto& e: seed_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    size--;

                seeds[e] = true;
            }
            for (const auto& e: peer_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    size--;

                seeds[e] = true;
            }

            seeds.clear();
            MPI_Send(&size, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
            for (const auto& e: seed_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    continue;
                MPI_Send(&e, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
                seeds[e] = true;
            }

            for (const auto& e: peer_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    continue;
                MPI_Send(&e, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
                seeds[e] = true;
            }
        } else if (strncmp(buffer, "update", 6) == 0) {
            strtok(buffer, " ");
            string file(strtok(NULL, " "));
            int size = seed_db[file].size() + peer_db[file].size();
            unordered_map<int, bool> seeds;
            for (const auto& e: seed_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    size--;

                seeds[e] = true;
            }
            for (const auto& e: peer_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    size--;

                seeds[e] = true;
            }

            seeds.clear();
            MPI_Send(&size, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
            for (const auto& e: seed_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    continue;
                MPI_Send(&e, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
                seeds[e] = true;
            }

            for (const auto& e: peer_db[file]) {
                if (e == source || seeds.find(e) != seeds.end())
                    continue;
                MPI_Send(&e, 1, MPI_INT, source, TRACKER_TAG, MPI_COMM_WORLD);
                seeds[e] = true;
            }
        } else if (strncmp(buffer, "downloaded", 10) == 0) {
            strtok(buffer, " ");
            string file(strtok(NULL, " "));
            seed_db[file].push_back(source);
        } else if (strncmp(buffer, "done", 4) == 0) {
            done++;
        }

        if (done == numtasks - 1) {
            break;
        }
    }

    string command = "close dummy dummy";
    for (int i = 1; i < numtasks; i++) {
        send_message(command, i, SENDTO_UPLOAD_TAG);
    }
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
    DIE(provided < MPI_THREAD_MULTIPLE, "No multi-threading support for MPI");
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    num_tasks = numtasks;
    if (rank == TRACKER_RANK)
        tracker(numtasks, rank);
    else
        peer(numtasks, rank);

    MPI_Finalize();
}