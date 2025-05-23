#include <iostream>   // For standard input and output operations (cin, cout, cerr)
#include <chrono>     // For working with time-based functionalities (timers, timestamps)
#include <condition_variable> // For thread synchronization using condition variables
#include <cstddef>    // For standard definitions like `size_t`, `nullptr`
#include <thread>     // For multithreading support
#include <mutex>      // For thread synchronization using mutex locks
#include <vector>     // For using dynamic arrays (vectors)
#include <list>       // For linked-list container
#include <random>     // For random number generation
#include <sys/types.h> // For various system types (used in networking and sockets)
#include <sys/socket.h> // For socket programming (creating, binding, and managing sockets)
#include <netdb.h>     // For network database operations (e.g., address resolution)
#include <arpa/inet.h> // For converting IP addresses between text and binary form
#include <netinet/in.h> // For defining Internet address structures
#include <stdlib.h>     // For memory management, random numbers, and process control
#include <cstring>      // For string manipulation and memory operations
#include <unistd.h>     // For POSIX operating system API (used for file descriptors, sleep, etc.)
#include <set>         // For using sets (unique collection of elements)
#include <fstream>     // For reading from and writing to files
#include <openssl/sha.h> 
#include <iomanip>     // For formatting output (used in hashing function output)
#include <sstream>     // For string stream manipulation
#include <map>        // For key-value pair storage (dictionary-like structures)
#include <cmath>      // For mathematical functions (e.g., floor, pow)

using namespace std;

#include "BS_thread_pool.hpp"   // For multithreading utilities (thread pooling)
#include "BS_thread_pool_utils.hpp" // Additional utilities for thread pools

// Define constants for message types in peer-to-peer communication
#define REGISTER_REQUEST "REGISTER" // Used when a peer registers with a seed node
#define PEER_LIST_REQUEST "GET_PEERS" // Request to get a list of connected peers
#define DEAD_NODE_MESSAGE "DEAD_NODE" // Used to notify about a non-responsive (dead) peer

#define MAXBUFF_LEN 100  // Maximum buffer length for receiving data

// Random number generator setup using Mersenne Twister engine
std::random_device rd;  // Random device to generate seed
std::mt19937 g(rd());   // Mersenne Twister pseudo-random number generator


struct SeedInfo {
    string ip;
    int port;
};

struct PeerInfo {
    string ip;
    int port;
};

// The following function is a custom-built print function to reduce the number of printing statements in the code. It basically takes in 3 arguments to print the message by checking if we need to print the output as a running error (using cerr) or as a console output to the terminal (using cout) and also checks for addition of a new line.

void PrintPeerMessage(string message,bool isErrorDominant = true,bool addNewLine=true){
    if(isErrorDominant){
        cerr<<message;
        if(addNewLine){
            cerr<<"\n";
        }
        return;
    }
    cout<<message;
    if(addNewLine){
        cout<<"\n";
    }
    return;
}

// These are mutex locks defined to synchronize access to the seeds and peers during multithreading to prevent issues like concurrent use of the same seed by two peers resulting in mix-up of messages and total breakdown of the network.
mutex peer_liv_response_mtx;
mutex peer_mutex;
mutex peer_thread_mutex;
mutex peer_sockets;
mutex deleted_set;

ofstream file;

map<string, int> message_list; 

// We are implementing each peer node using the class created below with access to its initial start-up public and implementated functionalitites private.
class PeerNode 
{
    public:
        PeerNode(int port, const string& config_file){
            this->server_port_ = port;
            this->config_file_ = config_file;
        }
        // This function initializes the peer node for networking and connects it to floor(n/2) + 1 random seed nodes. It implements multithreading to handle peer communication, liveness checks and generating messages.
        void start()
        {
            PrintPeerMessage(to_string(server_port_),false,false);
            PrintPeerMessage(config_file_,false,true);
            server_socket_ = socket(AF_INET, SOCK_STREAM, 0); // Creates a TCP socket by SOCK_STREAM and implements IPv4 protocol by AF_INET
            if (server_socket_ == -1) { //Error catching code
                PrintPeerMessage("Error creating socket");
                return;
            }
            // Configuring sockaddr_in structure
            sockaddr_in address;
            address.sin_family = AF_INET; // IPv4
            address.sin_addr.s_addr = INADDR_ANY; // Accepts connections from any IP.
            address.sin_port = htons(server_port_); // Converts port no. to network byte order

            // Bind function associates the socket with our configured sockaddr_in address.
            if (::bind(server_socket_, (sockaddr*)&address, sizeof(address)) == -1) {
                PrintPeerMessage("Error binding socket");
                return;
            }
            // Listen function initializes the socket to connect to 5 backlog/pending connections before refusing new connections, like making a buffer of 5 connections to process and allowing new connection requests into the buffer once the queue frees up.
            if (listen(server_socket_, 5) == -1) {
                PrintPeerMessage("Error listening on socket");
                return;
            }

            thread accept_thread(&PeerNode::acceptConnections, this); // Starts a thread to handle incoming peer connections asynchronously with acceptConnections responsible for accepting new peers
            seeds_ = read_seed_info(config_file_); // Reads the list of seed nodes available in the network

            connect_to_seeds(); // Establishes TCP connections with randomly selected seed nodes.

            sleep(1); // Delay of 1 second
            receive_peer_lists(); // Requests peer information from connected seed nodes to check for available peers to connect.

            sleep(1);
            connect_to_peers(); // Establishes connections to available peer nodes.

            
            gen_message_thread = move(thread(&PeerNode::generate_messages, this)); // Creates and broadcasts random messages to peers with detach allowing it to run independently from the main program.
            gen_message_thread.detach();

            liveness_thread = move(thread(&PeerNode::check_liveness, this)); // Periodically checks whether peers are still alive and runs independently courtesy of the detach() function.
            liveness_thread.detach();
            accept_thread.join(); // Ensures that the main thread waits for the acceptConnections() thread to finish.
        }

    private:
        // Here initially we have declared some data structures and variables in the private scope to handle the functionalities of our peer node
        vector<SeedInfo> seeds_;

        string config_file_;

        int server_socket_;
        int server_port_;
        vector<int> seed_connected_sockets;
        vector<struct PeerInfo> peer_list;
        vector<int> peer_connected_sockets;

        vector<thread> peer_threads;
        thread gen_message_thread;
        thread liveness_thread;

        vector<int> peer_liv_response;
        vector<int> peer_liveness;
        set <int> deleted_idxs;

        // This function handles the incoming peer connection requests by extracting the IP and port of the peer and after processing it, spawns a new thread to handle the newly connected peer.
        void acceptConnections() {
            while (true) {
                sockaddr_in client_address; // Stores the requesting peer's IP & port.
                socklen_t client_address_size = sizeof(client_address); // Stores the length of the address of the peer requesting connection.
                int client_socket = accept(server_socket_, (sockaddr*)&client_address, &client_address_size); // Accepts a new incoming peer connection.

                if (client_socket == -1) {
                    PrintPeerMessage("Error accepting connection");
                    continue;
                }


                char buffr[100];

                int nb = recv(client_socket, buffr, 100, 0); // Receives IP and port information from the peer and if no information is received, logs the error message and moves to the next connection.
                if (nb <= 0)
                {
                    PrintPeerMessage("Not Able to get the Client Port or IP Address!");
                    continue;
                }

                istringstream ss(buffr); // Parses the info. in the buffer.

                string ip; 
                int port;

                ss >> ip >> port; // Reads and stores the values from the buffer into variables we would use
                std::string msg = "Connection established from Peer : "+to_string(port);
                PrintPeerMessage(msg,false,true);
                file<<msg<<endl;

                lock_guard<mutex> lock(peer_thread_mutex); // Ensures thread safety when modifying peer_list to prevent any dirty read or over-writing issues.
                peer_list.push_back({ip, port}); // Stores peer details.
                peer_connected_sockets.push_back(client_socket); // Stores the socket descriptor for communication.
                peer_liv_response.push_back(0); // Initializes liveness response counter (used for detecting failures).

                // This starts a new thread to handle peer communication and stores the new threads in a vector, the detach function allows the thread to run independently.
                thread t(&PeerNode::peer_handler, this, peer_connected_sockets.size() -1);
                peer_threads.push_back(move(t));
                peer_threads[peer_connected_sockets.size() -1].detach();
            }
        }

        // This function establishes TCP connections with seeds present in the seeds_ vector list. The size of the vector list is floor(n/2) + 1 and the seeds present in it are randomly selected via read_seed_info() function discussed later. The function iterates through each seed's IP address and port, resolves the address, and attempts to create and connect a socket. If successful, it sends a registration request to the seed node and stores the connected socket for future communication. The function also has error handling capabilities and frees up memory and closes failed connections.
        void connect_to_seeds() {
            for(auto& s_info : seeds_)
            {
                int sock_fd;

                struct addrinfo hints, *servinfo, *p;
                int rv;

                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;

                if ((rv = getaddrinfo(s_info.ip.c_str(), to_string(s_info.port).c_str(), &hints, &servinfo)) != 0) {
                    PrintPeerMessage("getaddrinfo: ",true,false);
                    PrintPeerMessage(gai_strerror(rv));
                    return;
                }

                for (p = servinfo; p != NULL; p = p->ai_next)
                {
                    if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
                    {
                        continue;
                    }
                    if (connect(sock_fd, p->ai_addr, p->ai_addrlen) == -1)
                    {
                        close(sock_fd);
                        continue;
                    }

                    break;
                }

                freeaddrinfo(servinfo);

                string mess = REGISTER_REQUEST;
                mess += " 127.0.0.1 ";
                mess += to_string(server_port_);
                if (send(sock_fd, mess.c_str(), mess.size(), 0) == -1)
                {
                    PrintPeerMessage("Not able to register!",true,false);
                    close(sock_fd);
                    continue;
                }
                
                cout<<"Connected to seed "<<s_info.ip <<' '<<s_info.port<<endl<<endl;
                file<<"Connected to seed "<<s_info.ip <<' '<<s_info.port<<endl<<endl;

                seed_connected_sockets.push_back(sock_fd);
                
            }
        }

        // This function asks the connected seed nodes for a list of peers connected to the seed node, processes the response, and selects a few random peers to connect with. It first sends a PEER_LIST_REQUEST message to each seed node using the sockets stored in seed_connected_sockets. If the request is sent successfully, it waits for a response and stores the received peer list in a buffer, if received successfully. The received data contains IP addresses and ports of other peers, which are extracted and stored in a set (set_info) to remove duplicates. Once all responses are processed, the function converts the unique peer information from the set into a vector (temp_info) and then randomly selects between 1 to 4 peers from this list, making sure it doesn't select itself. The chosen peers are added to peer_list, which will later be used for establishing peer-to-peer connections. This approach helps each peer discover and connect with new nodes dynamically while avoiding unnecessary duplicate connections.
        void receive_peer_lists() {
            
            set<string> set_info;
            vector<struct PeerInfo> temp_info;

            for (auto& seed_sock : seed_connected_sockets)
            {
                if (send(seed_sock, PEER_LIST_REQUEST, sizeof(PEER_LIST_REQUEST), 0) == -1)
                {
                    PrintPeerMessage("Not able to send the peer list!");
                    continue;
                }

                char buff[MAXBUFF_LEN];

                int numbytes = recv(seed_sock, buff, MAXBUFF_LEN-1, 0);
                if (numbytes == -1)
                {
                    PrintPeerMessage("Not able to receive the peer list");
                    continue;
                }
                buff[numbytes] = '\0';
                for (int i = 0; buff[i]!='\0';i++)
                {
                    string host, port;
                    for (; buff[i]!=' '; i++)
                    {
                        host += buff[i];
                    }
                    i++;
                    for (; buff[i]!=' '; i++)
                    {
                        port += buff[i];
                    }
                    set_info.insert(host + " " + port);
                    
                }

            }

            for (auto& s : set_info)
            {
                stringstream ss(s);
                string h, p;
                ss >> h >> p;

                temp_info.push_back({h, stoi(p)});
            }

            int num_peers = rand() % 4 + 1;

            if(num_peers > temp_info.size()-1)
                num_peers = temp_info.size()-1;


            set <int> chosen_rands;


            for (int i = 0; i < num_peers; ++i)
            {
                int tmp;

                do{
                    tmp = rand() % temp_info.size();
                } while(chosen_rands.count(tmp) || temp_info[tmp].port == server_port_);


                peer_list.push_back(temp_info[tmp]);

                chosen_rands.insert(tmp);
            }
        }

        // This function connects to the peers listed in peer_list by establishing TCP connections with them. It loops through each peer’s IP address and port, resolves the address using getaddrinfo(), and attempts to create a socket. Once a connection is successfully established, the peer sends its own IP and port to the newly connected peer for identification. The function then stores the connected socket in peer_connected_sockets and initializes the liveness tracking variable in peer_liv_response. Finally, it spawns a new thread to handle communication with the connected peer using peer_handler(), allowing the peer-to-peer connection to function asynchronously.
        void connect_to_peers() {
            for (auto& p_info : peer_list)
            {

                int sock_fd;

                struct addrinfo hints, *servinfo, *p;
                int rv;

                memset(&hints, 0, sizeof hints);
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;

                if ((rv = getaddrinfo(p_info.ip.c_str(), to_string(p_info.port).c_str(), &hints, &servinfo)) != 0) {
                    PrintPeerMessage(gai_strerror(rv),false,true);
                    return;
                }

                for (p = servinfo; p != NULL; p = p->ai_next)
                {
                    if ((sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
                    {
                        PrintPeerMessage("client: socket");
                        continue;
                    }
                    if (connect(sock_fd, p->ai_addr, p->ai_addrlen) == -1)
                    {
                        PrintPeerMessage("Connected");
                        close(sock_fd);
                        continue;
                    }

                    break;
                }

                string ms = "127.0.0.1 " + to_string(server_port_);
                if (send(sock_fd, ms.c_str(), ms.size(), 0) == -1)
                {
                    PrintPeerMessage("Could not send info to peers");
                }
                
                freeaddrinfo(servinfo);

                peer_connected_sockets.push_back(sock_fd);
                peer_liv_response.push_back(0);

                thread t(&PeerNode::peer_handler, this, peer_connected_sockets.size() -1);

                peer_threads.push_back(move(t));
                peer_threads[peer_connected_sockets.size() -1].detach();



            }
        }

        // This function continuously listens for messages from a connected peer and processes them accordingly. It first reads incoming data using recv(), and interprets the received message. If the message is a liveness request, it logs the request and responds with a liveness reply. If it is a liveness reply, it resets the peer’s liveness response counter, indicating that the peer is still active. Else the function assumes that the message is a general broadcast message, then extracts its timestamp, sender IP, and content, and logs it. To prevent propagation of duplicate messages, it computes a simple hash of the message and checks if it has already been received. If it is a new message, the function forwards it to all other connected peers except the original sender. If the connection is lost or closed, the peer’s socket is closed and removed from active communication. This function ensures efficient message handling, peer liveness tracking, and decentralized message propagation within the network.
        void peer_handler(int idx) {
            
            char buff[MAXBUFF_LEN];
            
            int numbytes;
            while ((numbytes = recv(peer_connected_sockets[idx], buff, MAXBUFF_LEN-1, 0)) > 0)
            {
                                
                
                istringstream ss(buff);
                string id1, id2;
                ss >> id1 >> id2;

                const char* id1_ = id1.c_str(); 
                const char* id2_ = id2.c_str(); 

                if(strcmp(id1_, "Liveliness") == 0 and strcmp(id2_, "Request:") == 0)
                {

                    cout<<"Liveness Request Recieved from : "<<peer_list[idx].ip<<" : "<<peer_list[idx].port<<endl; 
                    file<<"LIVELINESS REQUEST RECIENVED FROM : "<<peer_list[idx].ip<<" : "<<peer_list[idx].port<<endl; 

                    string time_stamp, self_ip;
                    string temp;
                    ss >>time_stamp >> temp >> self_ip;

                    liveness_reply(idx, time_stamp, self_ip);


                }
                else if(strcmp(id1_, "Liveliness") == 0 and strcmp(id2_, "Reply:") == 0)
                {
                    if(peer_list[idx].ip.size()!=0){
                        cout<<"Liveness Request Recieved from : "<<peer_list[idx].ip<<" : "<<peer_list[idx].port<<endl;
                        file<<"LIVELINESS REPLY RECIEVED FROM : "<<peer_list[idx].ip<<" : "<<peer_list[idx].port<<endl;

                        string time_stamp, sender_ip, self_ip;
                        string temp;
                        ss >> time_stamp >> temp >> sender_ip >> temp >> self_ip;


                        peer_liv_response[idx] = 0;
                    }
                    
                }
                else
                {
                    string timestamp, ip_, mes, temp;
                    istringstream ss2(buff);

                    ss2 >> timestamp >> temp >> ip_ >> temp >> mes;

                    string message = timestamp + " : " + ip_ + " : " + mes;
                    message = "Message Recieved :" + message;
                    PrintPeerMessage(message,false,true);
                    file<<message<<endl;

                    long long int hash = 0;

                    for (size_t i = 0; i < message.length(); ++i) {
                        hash = (hash * 31) + message[i];
                    }

                    std::stringstream ss;
                    ss << std::hex << std::setw(8) << std::setfill('0') << hash;

                    string hashed = ss.str();

                    if(message_list[hashed])
                    {
                        continue;
                    }
                    message_list[hashed] = 1;
                
                    vector<int> peers_broadcast;

                    cout<<"Broadcasting the message : "<<message<<endl<<endl;
                    file<<"Broadcasting the message : "<<message<<endl<<endl;

                    for(int i = 0; i < peer_connected_sockets.size(); ++i) 
                    {

                        if(i != idx)
                        {
                            lock_guard<mutex> lock(peer_sockets);
                            send(peer_connected_sockets[i], message.c_str(), numbytes, 0);
                        }
                    }
                    
                }
            }

            close(peer_connected_sockets[idx]);
        }

        // This function keeps logging the output of the liveness request with its timestamp and IP. If for some reason it cannot log the liveness reply, it logs it as an error and moves forward.
        void liveness_reply(int idx, string time_stamp, string server_ip)
        {
            
            string liveness_reply = "Liveliness Reply: " + time_stamp + ": " + "127.0.0.1: " + "127.0.0.1";
        
            if (send(peer_connected_sockets[idx], liveness_reply.c_str(), liveness_reply.size(), 0) == -1)
            {
                cerr<<"could not send the liveness reply"<<endl;
                return;
            }
        }

        // This function generates and broadcasts random messages to all connected peers. It runs in a loop, creating 10 messages over time. Each message consists of a timestamp, a fixed IP address (127.0.0.1), and a randomly generated alphanumeric string between 5 to 9 characters long. To avoid duplicate message propagation, a hash value is computed for each message using a simple rolling hash function. The hash is stored in message_list to track previously sent messages. The generated message is then sent to all connected peers, except those marked as deleted (deleted_idxs). A 5-second delay is added between each message to prevent flooding the network. This function helps simulate real-time message exchange among peers in a decentralized network.
        void generate_messages() 
        {
            for(int i=0; i<10; i++)
            {
                
                string characters = "qwertyuiopasdfghjklzxcvbnm1234567890";
                int length = 5+rand()%5;
                string temp_mess;

                for(int k=0; k<length; k++)
                {
                    temp_mess += characters[rand()%36];
                }
  
                
                string message = to_string(clock()) + " : " + "127.0.0.1" + " : " + temp_mess;
                unsigned int hash = 0;
                for (size_t i = 0; i < message.length(); ++i) {
                    hash = (hash * 31) + message[i];
                }

                std::stringstream ss;
                ss << std::hex << std::setw(8) << std::setfill('0') << hash;

                string hashed = ss.str();
                message_list[hashed] = 1;

                for (int i = 0; i < peer_connected_sockets.size(); ++i) {
                    if (deleted_idxs.count(i))
                        continue;
                    lock_guard<mutex> lock(peer_sockets);
                    send(peer_connected_sockets[i], message.c_str(), message.size(), 0);
                }
                sleep(5);
            }
        }

        // This function periodically checks the liveliness of the peer nodes connected to our peer node (every 13 seconds). If a node is listed as dead, it moves to the next node. If a peer fails to respond to a liveliness request 3 times in a row, it is considered to be dead and added to the deleted_idxs list. The peer then sends the status of the dead node to the seed nodes conneced to itself using the dead_node_reply() function allowing them to update their peer lists by removing the dead node. The lock_guard<mutex> ensures safe processing of the dead node by the peers across multiple threads thereby improving reliability. It is discussed further into the function. 
        void check_liveness() 
        {
            while (true) 
            {
                sleep(13);
                string message = "Liveliness Request: " + to_string(clock()) + " : " + "127.0.0.1";

                
                for(int i=0; i<peer_connected_sockets.size(); i++)
                {
                    if (deleted_idxs.count(i))
                        continue;
                    if(peer_liv_response[i] == 3)
                    {
                        PrintPeerMessage("DEAD NOT FOUND, Seed Informed",false,true);
                        file<<"DEAD NOT FOUND, Seed Informed"<<endl;

                        
                        dead_node_reply(peer_list[i].ip, peer_list[i].port);

                        close(peer_connected_sockets[i]);

                        lock_guard<mutex> lock(deleted_set); // Ensures that only one thread can mark a peer as deleted at a time, hence preventing multiple threads from attempting to remove the same peer simultaneously.

                        deleted_idxs.insert(i);

                        continue;

                    }
                    
                    lock_guard<mutex> lock(peer_sockets); // Ensures that only one thread at a time can modify the socket list and send a message, hence preventing data corruption when multiple threads send messages.
                    string msg = "Liveliness Request Sent to " +to_string(peer_connected_sockets[i]);
                    PrintPeerMessage(msg,false,true);
                    file<<msg<<endl;

                    send(peer_connected_sockets[i], message.c_str(), message.size(), 0);
                    peer_liv_response[i] += 1;
                }
            }
        }
        
        // This function handles the communication of dead peer nodes to the seeds associated to the peer. It transmits the information with important information being dead node IP, dead node port and Timestamp at which the peer node was found to be dead. If it cannot transmit the info, it logs it as an error and moves on.
        void dead_node_reply(string node_ip, int node_port)
        {
            string msg = "Dead Node: " + node_ip + " : " + to_string(node_port) + " : " + to_string(clock()) + " : " + "127.0.0.1";        
            for (auto& seed_socks : seed_connected_sockets)
            {   
                if (send(seed_socks, msg.c_str(), msg.size(), 0) == -1)
                {
                    PrintPeerMessage("Couldn't send Dead Node msg to seed");
                }
            }
        }

        // This function reads seed node details from a file, processes the data, and picks a random set of seeds for peer discovery. It first tries to open the file and logs an error if it fails. Each line in the file should have an IP address and a port number, which are extracted using stringstream. If a line is incorrect, an error is logged, and the function skips that line. After collecting all valid seeds, the list is shuffled randomly, and about half of the seeds (rounded up) are chosen for the final list. These selected seeds are returned and used for peer connections. This approach helps each peer connect to a random set of seeds, making the network more balanced and reliable.
        vector<SeedInfo> read_seed_info(const string& config_file) 
        {
            vector<SeedInfo> seeds;
            ifstream file;
            file.open(config_file);

            if (!file.is_open()) {
                cerr << "Error opening config file: " << config_file << endl;
                return seeds;
            }

            string line;
            while (getline(file, line)) {
                istringstream ss(line);
                string ip, port_str;
                int port;

                if (!(ss >> ip >> port_str)) 
                {
                    cerr << "Error parsing config line: " << line << endl;
                    continue;
                }

                if (!(stringstream(port_str) >> port)) {
                    cerr << "Error converting port to integer: " << port_str << endl;
                    continue;
                }

                seeds.push_back({ip, port});
            }

            file.close();
            int n = seeds.size();
            string msg = "Number of seeds : " + to_string(n);
            PrintPeerMessage(msg,false,true);
            int arr[n];
            for(int i = 0; i < n; ++i)
                arr[i] = i;

            shuffle(arr, arr+n,g);
            
            int m = floor(((float)n)/2) + 1;

            vector<SeedInfo> final_seeds;

            for(int i=0; i<m; i++)
            {
                final_seeds.push_back(seeds[arr[i]]);
            }

            return final_seeds;
        }

    };

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        PrintPeerMessage("Invalid usage: peer port");
        return 0;
    }

    char* port = argv[1];

    int prt = atoi(port);
    PeerNode node(prt, "config.txt");
    file.open("peer_" + to_string(prt)+".txt");
    node.start();
    return 0;
}