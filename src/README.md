Copyright 2024 ~ Toma-Ioan Dumitrescu

Faculty of Automatic Control and Computer Science
Group: 331CA

BitTorrent Protocol

### Description

The C++ project reperesents an efficient BitTorrent protocol where clients (seeds
or peers) send messages in parallel to the tracker and other clients for the initialization,
and for requesting files. The tracker sends the updated nodes swarm for each file request
of the tracker, and also notifies when all nodes finish. The communication is realised via
Message Passing Interface.

### Implementation

Following the BitTorrent protocol steps, the following functions were created:

1) Client:
    init_client: reads the data from its configuration file, and sends the owned files
with the hashes to the tracker. It also retains the list of wanted files.

    request_file: applications for requesting the swarm, and the list of hashes from the
tracker, but also for the update_file command. When retaining the needed segments with the
hashes, a mutex locks the owned_files, because it is also accessed by the update_thread.

    save_file: using the stored information, creates an output file and prints the hashes

    download_thread_func: retain a global tag that increments every message. The general
message model is to send the size, then the char buffer. Firstly, the swarm and segments
are requested from the current file, then for each segment find all clients available, and
choose the one with the minimum requests from the actual client. Make another request to
install the file with that hash to the optimal client. Every 10 segments downloaded, make
an update request to the tracker. After installing, add it in the system, by blocking
and unlocking the global resource. Notify the tracker after all segments are installed
and receive a buffer. Notify the tracker after all files were downloaded to close the download thread.

    upload_thread_func: receive messages from any source, and find it using the status variable.
Every message will have the format command + ' ' + filename + ' ' + segment, the parsing being done
via strtok. Now, the command can be check (faster one) or install, but all commands to the same thing,
this is only for making a clearly separation based on time efficiency for the BitTorrent simulation.
In other words, check in the list of owned_files and segments using a mutex (because those resources
are actively used by the download thread), then send OK or ERR to the MPI_SOURCE.

2) Tracker:
    hash_db: each file mapped to its hashes in order (info takem from the clients).
    seed_db: nodes that contain the files fully
    peer_db: nodes that contain the files partially or which do not contain it at all, but requested it

    init_tracker: receive the information from the init_client (this was tested), and retain the hashes
ans the seeds for each file

    send_sources: sends a list with seeds and peers for a specific file with no duplicates to a client
that requested that via update_file or request_file

    tracker(): for every client that is not done, receive using MPI_Recv an integer size and a char buffer
that has the command and data separated by a space. The command can be request_file that sends the list
of hashes and the sources (result of merging peers and seeds lists for a specific file), update_file: just
send the sources, notify: mark that client as seed for the specific file (also in the buffer), notify_all:
mark the client as done so it is skipped in the loop, and the download_thread of that client is closed after
sending it a message using MPI_Send. This loop helps balancing the requests from clients. After all clients
are done (verified efficiently using the number of clients marked as done and compared to numtasks - numtrackers
= numtasks - 1), a broadcast that overwrites the global variable stop is sent so that the upload_thread of
every client closes.

    tag management: because sending all messages with the same tag (0 for example) will lead to undefined
behavior (maybe the messages do not arrive in order or the threads call the MPI sending function in such
a manner that it leads to an error of processing), consider an interval of tags [0, 10000] and TAG_LIMIT
a divider for each client, so that client i has tags in [i * TAG_LIMIT, (i + 1) * TAG_LIMIT), then an array
retained by the tracker where every element j represents the expected tag of client j (thus initialized
with j * TAG_LIMIT). Th manner of sending messages in the download_thread function and init_tracker makes
it sufficient to increase the expected_tag[j] value for every MPI_Recv and MPI_Send. This works only for
the download_thread_func, because interacting with the upload_thread_func is more complex, hardly to determine
the expected tag. Here all the tags of the messages are 0, but this works because of how upload_thread_func
manages the received messages: for any message, it identifies the source, and it knows that it received
the size of the buffer; the second MPI_Recv won't be for any source, but for the previous source (to maintain
the order of the message and the desired logic).

### Efficiency

It is mainly represented by varying the hosts for the segments, and this is done individually for every
pair (client, segment). The check request is way faster than install command (although because of the
simulation they are doing the same thing). Every individual INSTALL request is counted, and every time
the one with the minimum requests (from the actual client) is chosen as optimal. It does not matter the
number of requests from all clients to the current host, because after a request, another host will be
triggered, so the varying is maximum possible. This way of varying the hosts does not function only in the
case when all peers download the same file and generate requests for the same segment at the same time:
the peers will download from host 1 the first segment, then from host 2 the second segment etc. While this
is the only inefficient case, it is the most improbable, firstly because of the threads having different
performances, and secondly because of the order of the files desired by a specific client that does not
necessarily coincide with the one desired by another client.

### Debugging
Currently, the program needts to be debugged for minor errors. The debugging method will typically
consist in prints on some files: ofstream fout("debug.txt") ... fout.close().

### Bibliography
https://en.wikipedia.org/wiki/BitTorrent
https://skerritt.blog/bit-torrent/
https://en.wikipedia.org/wiki/BitTorrent_tracker
