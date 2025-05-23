#include <iostream>      // Provides standard input and output functionalities (cout, cerr, etc.)
#include <fstream>       // Enables file handling operations (ifstream, ofstream)
#include <vector>        // Implements dynamic arrays (std::vector) for storing peer lists
#include <map>          // Implements key-value pairs (std::map) for managing message history
#include <string>       // Supports string manipulation functions (std::string, std::stringstream)
#include <sstream>      // Provides string stream functionalities for parsing messages
#include <thread>       // Supports multi-threading operations for handling multiple clients
#include <mutex>        // Implements thread synchronization to avoid race conditions
#include <list>         // Implements doubly linked lists (std::list) for storing connected peers
#include <random>       // Generates random numbers (used in peer selection)
#include <sys/types.h>  // Defines data types for sockets (used for compatibility)
#include <sys/socket.h> // Provides socket programming functions (socket(), bind(), listen(), accept())
#include <netdb.h>      // Defines network database functions (getaddrinfo(), freeaddrinfo())
#include <arpa/inet.h>  // Includes functions for IP address conversion (inet_addr(), inet_ntoa())
#include <netinet/in.h> // Defines constants and structures for the internet domain (sockaddr_in)
#include <stdlib.h>     // Provides general-purpose functions (exit(), atoi())
#include <cstring>      // Supports string manipulation and memory functions (memset(), strcmp())
#include <unistd.h>     // Provides system calls for file descriptors and process control (close(), read(), write())
#include <set>         // Implements sets (std::set) for storing unique peer addresses

// Defines constants for message types used in peer-to-seed communication
#define REGISTER_REQUEST "REGISTER"
#define PEER_LIST_REQUEST "GET_PEERS"
#define DEAD_NODE_MESSAGE "Dead"

using namespace std;

mutex seed_mutex; // Mutex for thread safety while modifying shared resources (peer list, file handling)

ofstream file; // File stream object for logging peer activity and messages

// The following function is a custom-built print function to reduce the number of printing statements in the code. It basically takes in 3 arguments to print the message by checking if we need to print the output as a running error (using cerr) or as a console output to the terminal (using cout) and also checks for addition of a new line.
void PrintSeedMessage(string message,bool isErrorDominant = true,bool addNewLine=true){
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


struct PeerInfo {
    string ip;
    int port;
};

class PeerList {
public:
    list<struct PeerInfo> peers_;
    PeerList() {
        peers_.clear();
    }
    ~PeerList() {}

    // This function adds a new peer whose connection request has been processed and allowed to connect to the peer list of the given seed node. It stores the IP address and port no. of the peer.
    void addPeer(const string& ip, int port) {
        peers_.push_back({ip, port});
    }
    // This function removes the peer that has been declared dead by multiple other peers from its peer list.
    void removePeer(const string& ip, int port) {
        for (auto it = peers_.begin(); it != peers_.end(); ++it) {
            if (it->ip == ip && it->port == port) {
                peers_.erase(it);
                return;
            }
        }
    }

    // This function checks if the peer requesting connection or deletion is present in its peer list to facilitate further processing.
    bool hasPeer(const string& ip, int port) const {
        for (const auto& peer : peers_) {
            if (peer.ip == ip && peer.port == port) {
                return true;
            }
        }
        return false;
    }
    // Returns the size of the peer list.
    size_t size() const {
        return peers_.size();
    }

    
}peers_;

class SeedNode {

public:
    SeedNode(int port) : port_(port) {}

    // This function initializes and starts the seed node, allowing it to accept connections from peers. It first creates a TCP socket using socket(), and if this fails, it logs an error and exits. Then, it sets up the server address structure, specifying AF_INET (IPv4), a fixed local IP (127.0.0.1), and the given port number. The function binds the socket to this address using bind(), and if the binding fails, it logs an error and exits. Next, it sets the socket to listen for incoming connections with a queue size of 5, handling errors if it fails. Once the seed node is successfully set up, it logs that it is listening on the specified port and starts a separate thread (accept_thread) to handle incoming peer connections using acceptConnections(). This thread is detached, meaning it runs independently in the background. Finally, the function enters an infinite loop (while(true);), keeping the seed node active indefinitely.
    void start() {

        server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket_ == -1) {
            PrintSeedMessage("Error while creating a socket");
            return;
        }

        struct sockaddr_in address;

        memset(&address, 0, sizeof(address));

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = inet_addr("127.0.0.1");
        address.sin_port = htons(port_);

        if (::bind(server_socket_, (sockaddr*)&address, sizeof(address))<0) {
            PrintSeedMessage("Error while binding the socket");
            return;
        }
        if (listen(server_socket_, 5) == -1) {
            PrintSeedMessage("Error while listening on the socket!");
            return;
        }
        PrintSeedMessage("Seed node listening on port: ",false,false);
        PrintSeedMessage(to_string(port_),false,true);
        file << "Seed node listening on port " << port_ << endl;

        thread accept_thread(&SeedNode::acceptConnections, this);
        accept_thread.detach();

        while(true);
    
    }

private:
    int server_socket_;
    int port_;

    // This function handles the incoming peer connection requests by extracting the IP and port of the peer and after processing it, spawns a new thread to handle the newly connected peer. It stores the sockaddr_in configured address and size of the addresses using it to process and accept the connections.
    void acceptConnections() {
        while (true) {
            sockaddr_in client_address;
            socklen_t client_address_size = sizeof(client_address);
            int client_socket = accept(server_socket_, (sockaddr*)&client_address, &client_address_size);

            if (client_socket == -1) {
                PrintSeedMessage("Error accepting connection",true,true);
                continue;
            }

            thread client_thread(&SeedNode::handleClient, this, client_socket);
            client_thread.detach();
        }
    }
    // This function manages communication with a connected peer by continuously listening for incoming messages. It reads data into a buffer using recv(), and if the client disconnects or an error occurs, it closes the socket and exits. When a message is successfully received, it is converted into a string and passed to processMessage() for further handling. This ensures smooth client interaction along with handling disconnections and errors.
    void handleClient(int client_socket) {
        while (true) {
            char buffer[1024];
            int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);

            if (bytes_received == 0) {
                close(client_socket);
                return;
            } 
            else if (bytes_received == -1) {
                PrintSeedMessage("Error receiving data from client",true,true);
                close(client_socket);
                return;
            }

            string message(buffer, bytes_received);
            processMessage(client_socket, message);
        }
    }

    // This function processes messages from connected peers and decides how to respond based on the command received. It first extracts the command from the message and checks what kind of request it is. If a peer wants to register, it extracts the IP and port and adds the peer to the network. If a peer requests the list of active peers, the function logs the request and sends the list back. If the message reports a dead node, it extracts the details, logs the failure, and removes the inactive peer from the list. If the command is unknown, it logs an error. This function helps the seed node manage peer connections, share network information, and track node failures efficiently.
    void processMessage(int client_socket, const string& message) {
        stringstream ss(message);
        string command;
        ss >> command;
        

        const char* com = command.c_str(); 

        if (strcmp(com, REGISTER_REQUEST) == 0) 
        {
            PeerInfo peer;
            ss >> peer.ip >> peer.port;
            registerPeer(client_socket, peer);
        } 
        else if (strcmp(com, PEER_LIST_REQUEST) == 0) 
        {
            PrintSeedMessage("Peer has requested the peer list",false,true);
            file<<"Peer has requested the peer list"<<endl;
            sendPeerList(client_socket);
        } 
        else if (strncmp(com, DEAD_NODE_MESSAGE, 4) == 0) {
            string dead_ip, dead_port, temp, timestamp, node_ip;
            ss >> temp>>dead_ip >>temp >> dead_port >> temp >> timestamp >> temp >> node_ip;
            string msg = ("Message Recieved : Dead Node: " + dead_ip + " : " + dead_port + " : " + timestamp + " : " + node_ip);
            PrintSeedMessage(msg,false,true);
            file<<msg<<endl;  
            removeDeadPeer(dead_ip, stoi(dead_port));
        } 
        else {
            PrintSeedMessage("Received unknown command from client: " + message,false,true);
            file << "Received unknown command from client: "<< message << endl;
        }
    }

    // This function registers a newly processed peer by adding its IP address and port no. to the seed's peerlist. It does so by first locking the mutex to ensure that no other thread modifies the list at th same time. After this, it unlocks the mutex and allows for other operations such as logging the registered entry into the terminal and into the file. This function helps safely manage new peer registrations while ensuring data consistency in a multi-threaded environment.
    void registerPeer(int client_socket, const PeerInfo& peer) {
        seed_mutex.lock();

        peers_.addPeer(peer.ip, peer.port);
        seed_mutex.unlock();
        std::string msg = "New Peer registered: " + peer.ip + ": ";
        PrintSeedMessage(msg,false,false);
        cout<<peer.port<<endl;
        file << msg <<peer.port << endl;
    }

    // This function sends the list of peers registered with the seed node to the requesting peer node. It first locks the mutex to prevent any thread from modifying the peerlist while it is being accessed by the function. It iterates through the entire list and formats all the peer IP addresses and port no. into a single string. Then it unlocks the mutex and sends the string message to the requesting peer, logging this request into the terminal and file. This function ensures thread-safe access to peer data while allowing new peers to discover existing ones in the network.
    void sendPeerList(int client_socket) {

        seed_mutex.lock();

        string peer_list = "";


        int i = 0;
        for (const auto& peer : peers_.peers_) {
            peer_list += peer.ip + " " + to_string(peer.port) + " ";
            
            i++;
        }

        seed_mutex.unlock();

        
        send(client_socket, peer_list.c_str() , peer_list.size(), 0);
        PrintSeedMessage("Peer List Sended to ",false,false);
        cout<<client_socket<<endl;
    }
    // This function helps remove dead peer nodes from the network by deleting it from the seed's peerlist. It does so by first locking the mutex, then logging the removal of given dead node into the terminal and file. Finally it uses the _removePeer() function to delete the node and ends the task by unlocking the mutex.
    void removeDeadPeer(const string& dead_ip, int dead_port) {
        seed_mutex.lock();
        std::string msg = "Removing "+dead_ip +":"+to_string(dead_port)+" from the peer list";
        PrintSeedMessage(msg,false,true);
        file<<msg<<endl;
        peers_.removePeer(dead_ip, dead_port);
        seed_mutex.unlock();

    }


};

int main(int argc, char* argv[])
{
    if (argc < 2){   
        PrintSeedMessage("Invalid usage: seed port",true,true);
        return 0;
    }
    char* port = argv[1];

    int prt = atoi(port);

    SeedNode node(prt);

    file.open("seed_" + to_string(prt)+".txt");
    node.start();

    return 0;
}



