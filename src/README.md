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

Run the checker: sudo ./checker.sh

### Implementation

Following the BitTorrent protocol steps, the following functions were created:

1) Wrapper functions:
send_message(): sends the length pf the message, and then the message converted from string.
receive_message(): receives the length, resets the buffer with length chars, then stores the message
The trick is that the size can be received from any source S, but the message should also come from S,
ensuring the order of the messages.

Two different tags are used in the wrapper functions (tag and tag + 1) to maintain the messages order.

2) Client:
    init_client: reads the data from its configuration file, and sends the owned files
with the hashes to the tracker. It also retains the list of wanted files. Because the tracker
can send from multiple sources, a hashmap retains if the hash of (filename - hash_file) was
previously stored, to avoid duplicates. Each send message will be on prev_tag + 1 to maintain
the correctness of data transmission. Also, the ACK message will be received on the ACK_TAG.

    save_file: using the stored information, creates an output file and prints the hashes

    download_thread_func: The general message model is to send the size, then the char buffer.
Firstly, the swarm and segments are requested from the current file, then for each segment find
all clients available, and choose the one with the minimum requests from the actual client. Make
another request to install the file with that hash to the optimal client. Every 10 segments downloaded,
make an update request to the tracker. After installing, add it in the system, by blocking
and unlocking the global resource. Notify the tracker after all segments are installed
and receive a buffer. Notify the tracker after all files were downloaded to close the download thread.
For communicating with the tracker, tag 0 is sufficient. Now for communicating with the upload_thread,
two tags should be used, one for sending info (TAG_SEND), and the second one for receving info (TAG_RECV)
(BUG that consumed time: This prevents the case when the process P receives on the download_thread what it
should received on the upload_thread). Most of the send are complemented by recv, so no need of a complex
tag system here.

    request_file command: applications for requesting the swarm, and the list of hashes from the
tracker, but also for the update_file command. When retaining the needed segments with the
hashes, a mutex locks the owned_files, because it is also accessed by the update_thread.

    upload_thread_func: receives messages from any source, and finds it using the status variable.
Every message will have the format command + ' ' + filename + ' ' + segment, the parsing being done
via strtok. Now, the command can be check (faster one) or install, but all commands do the same thing,
this is only for making a clearly separation based on time efficiency for the BitTorrent.
In other words, check in the list of owned_files and segments using a mutex (because those resources
are actively used by the download thread), then send OK(1) or ERR(0) to the MPI_SOURCE.

3) Tracker:
    hash_db: each file mapped to its hashes in order (info takem from the clients).
    seed_db: nodes that contain the files fully
    peer_db: nodes that contain the files partially or which do not contain it at all, but requested it
    tracker_registered_hashes: avoid duplicates

    init_tracker: receives the information from the init_client (this was tested), and retain the hashes
ans the seeds for each file. For every host i, the tag resets to 0, but the channel will maintain the
order, because the function f(host, tag) = (host, tag) is bijective. Every recv will be on tag++.

    send_sources: sends a list with seeds and peers for a specific file with no duplicates to a client
that requested that via update_file or request_file

    tracker(): receives a message from any source using the wrapper functions, parses the tokens and
based on the results it executes the commands. The messages communications are well defined, no need of
a complex tag system.

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
