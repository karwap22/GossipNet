README 

Description - 
The project aims to stimulate the Gossip Protocol over a peer to peer network to broadcast messages and check the liveliness of the connected pair.
The project consists of two parts-
    => Seed - The seed are the nodes in the network, which is binded to a user-defined IP Address and port, 
    mentioned in config_file. We have used multithreading in the network so that it can handle various connected
    peers. The Seed also contains a Peer List, which is an actively maintained list containing 
    all the active Peers connected to the seed. When a new Peer is joined randomly to any of the seed, 
    it is added to the list and also provided this list.
    =>Peer - The Peer are the nodes with their respective ports. As soon as they become part of the network 
    they are randomly joined to floor((n/2) + 1) seed nodes, and in turn further connected to peers of them.
    After connecting every peer generates a message and broadcasts this message to all the peers every 5 
    seconds. When a message is received it checks for its hash and checks if it is already present or not, and 
    if not broadcasts it back to all the peers. After every 13 seconds it checks for the liveliness request, 
    and send a dead node message if not replied three times. 

File Structure - 
Following files are present in the folder - 
    1. Header Files - BS_thread_pool.hpp && BS_thread_pool_utils.hpp
    2. Seed.cpp - Code for Seeds
    3. Peer.cpp - Code for Peers
    4. Config.txt - Containing all the seed nodes ip and ports.
    5. README - This readme file. 


Running the code - 
After unzipping the folder , follow the below steps -
 
    1. Make Sure that the openssl library is installed. As we did the project in MacOS we used Homebrew for it as evident in below terminal commands.
    2. For running Seed Node - 
        a. Open terminal at the current directory
        b. Type the command  - “ g++ -w seed.cpp -o seed -I$(brew --prefix openssl)/include -L$(brew --prefix openssl)/lib -lssl -lcrypto -std=c++17 && ./seed {SEED_PORT} ”
    3. For running the Peer Node - 
        a. Open terminal at the current directory
        b. Type the command  - “ g++ -w peer.cpp -o peer -I$(brew --prefix openssl)/include -L$(brew --prefix openssl)/lib -lssl -lcrypto -std=c++17 && ./peer {PEER_PORT} ”

