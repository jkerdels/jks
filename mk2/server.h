#ifndef JK_SERVER_H
#define JK_SERVER_H

#include <cstring>

#include <vector>
#include <unordered_map>
#include <thread>
#include <functional>
#include <atomic>
#include <algorithm>

#include <chrono>
using namespace std::chrono_literals;

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <poll.h>

namespace jk {

template<class ConnectionHandler>
class Server
{
	static const int PollTimeout    = 100;
	static const int ListenBacklog  = 128;
	static const int MaxConnections = 1000;

public:
	Server() :
		worker(),
		port(0),
		stopListening(true),
		ct(nullptr)
	{}

	~Server() 
	{
		stop_serving();
	}

	void start_serving(int port, int nrOfThreads = 2) {
		stop_serving();

		if (port < 1) port = 1;
		if (nrOfThreads < 2) nrOfThreads = 2;

		this->port = port;

		worker.resize(nrOfThreads-1);
		for (auto &w : worker) {
			w.start();
		}

		stopListening = false;
		ct = new std::thread(std::ref(*this));
	}

	void stop_serving() {
		if (stopListening == true) {
			return;
		}

		stopListening = true;
		if (ct != nullptr) {
			ct->join();
			delete ct;
			ct = nullptr;
		}

		for (auto &w : worker) {
			w.stop();
		}
	}

private:

	struct Worker {
		Worker() :
			connections(),
			localPipe(0),
			t(nullptr),
			stopWorking(false),
			lpPoll(),
			conLock(ATOMIC_FLAG_INIT),
			newCons(0)
		{
			std::printf("Worker()\n");
			int tmp[2];
			int rv = pipe2(tmp,O_NONBLOCK);
			if (rv < 0) {
				// TODO: handle error of pipe
			}
			lpPoll.fd      = tmp[0];
			lpPoll.events  = POLLIN | POLLPRI;
			lpPoll.revents = 0;
			localPipe      = tmp[1];			
		}

		Worker(const Worker &other) :
			connections(other.connections),
			localPipe(other.localPipe),
			t(other.t),
			stopWorking(other.stopWorking),
			lpPoll(other.lpPoll),
			conLock(ATOMIC_FLAG_INIT),
			newCons(other.newCons)
		{
			std::printf("Worker(const Worker&)\n");
		}

		Worker(Worker &&other) :
			connections(),
			localPipe(0),
			t(nullptr),
			stopWorking(false),
			lpPoll(),
			conLock(ATOMIC_FLAG_INIT),
			newCons(0)
		{
			std::printf("Worker(Worker&&)\n");
			std::swap(connections,other.connections);
			std::swap(localPipe,other.localPipe);
			std::swap(t,other.t);
			std::swap(stopWorking,other.stopWorking);
			std::swap(lpPoll,other.lpPoll);
			std::swap(newCons,other.newCons);
		}

		~Worker() 
		{
			std::printf("~Worker()\n");
			stop();
		}

		Worker& operator=(const Worker &other) 
		{
			std::printf("operator=(const Worker&)\n");
			connections  = other.connections;
			localPipe    = other.localPipe;
			t            = other.t;
			stopWorking = other.stopWorking;
			lpPoll       = other.lpPoll;
			newCons      = other.newCons;
			return *this;				
		}

		void add_connection(int fd) {
			lock_connections();
			if (stopWorking == true) {
				close(fd);
				unlock_connections();
				return;
			}
			auto new_con = connections.emplace(std::make_pair(fd,ConnectionHandler()));
			new_con.first->second.set_sockets(fd,localPipe);
			newCons++;
			conLock.clear();
			break_poll();
		}

		size_t connection_count() {
			size_t result = 0;
			lock_connections();
			result = connections.size();
			unlock_connections();
			return result;			
		}

		int new_con_count() {
			return newCons;
		}

		void start() {
			if (t == nullptr) {
				t = new std::thread(std::ref(*this));
			}
		}

		void stop() {
			stopWorking = true;
			if (t != nullptr) {
				t->join();
				delete t;
				t = nullptr;
			}
		}

		void break_poll() {
			// write something to local pipe to make poll return
			uint8_t data = 1;
			ssize_t result = write(localPipe,&data,1);	
		}

		void lock_connections() {
			while(conLock.test_and_set());
		}

		void unlock_connections() {
			conLock.clear();
		}

		void operator()() {
			std::vector<pollfd> pollSet;
			while (stopWorking == false) {

				// rebuilt pollSet
				lock_connections();
				newCons = 0;
				size_t con_size = connections.size();

				if (con_size == 0) {
					unlock_connections();
					std::this_thread::sleep_for(50ms);
					continue;
				}

				pollSet.resize(con_size+1);
				size_t pollIdx = 0;
				for (auto &con_it : connections) {
					pollSet[pollIdx].fd      = con_it.first;
					pollSet[pollIdx].events  = POLLIN | POLLPRI;
					pollSet[pollIdx].revents = 0;
					if (con_it.second.write_pending() == true) {
						pollSet[pollIdx].events |= POLLOUT;
					}
					pollIdx++;
				}
				pollSet[con_size] = lpPoll;
				unlock_connections();

				poll(pollSet.data(),pollSet.size(),PollTimeout);

				lock_connections();
				for (auto &ps_entry : pollSet) {
					
					if (ps_entry.revents == 0) continue;			

					// find handler
					auto con_it = connections.find(ps_entry.fd);

					if (con_it == connections.end()) {
						if (ps_entry.fd == lpPoll.fd) {
							// empty local pipe
							uint64_t tmp;
							while(read(lpPoll.fd,&tmp,8) == 8);
						} else {
							// should not happen, TODO report error
						}				
						continue;
					}

					if ((ps_entry.revents & POLLIN) || (ps_entry.revents & POLLPRI)) {
						con_it->second.read_from_socket();
					}
					if (ps_entry.revents & POLLOUT) {
						con_it->second.write_to_socket();
					}
					if (con_it->second.connection_closed() == true) {
						close(ps_entry.fd);
					}
				}

				// remove closed connections from vector connections
			    for(auto it = connections.begin(); it != connections.end(); ) {
			        if(it->second.connection_closed() == true) {
			            it = connections.erase(it);
			        } else {
			            ++it;
			        }
			    }
				unlock_connections();
			}

			// close all remaining connections
			lock_connections();
			for (auto &c : connections) {
				close(c.first);
			}
			connections.clear();
			unlock_connections();

		}

		std::unordered_map<int,ConnectionHandler> connections;
		int localPipe;
		std::thread *t;
		bool stopWorking;
		pollfd lpPoll;
		std::atomic_flag conLock;
		int newCons;
	};

public:
	void operator()() {
		// set up server socket
	    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
	    if (serverSocket < 0) {
	    	// TODO: handle error case
	        return;
	    }

	    // So that we can re-bind to it without TIME_WAIT problems
	    int reuse_addr = 1;
		setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse_addr,
			sizeof(reuse_addr));

	    // make socket non-blocking
		int opts = fcntl(serverSocket,F_GETFL);
		if (opts < 0) {
	    	// TODO: handle error case
	        close(serverSocket);
	        return;
		}
		opts = (opts | O_NONBLOCK);
		if (fcntl(serverSocket,F_SETFL,opts) < 0) {
	    	// TODO: handle error case
	        close(serverSocket);
	        return;
		}

	    // prepare to bind to port
	    struct sockaddr_in serv_addr;
	    std::memset(&serv_addr,0,sizeof(serv_addr));

	    serv_addr.sin_family = AF_INET;
	    serv_addr.sin_port   = htons(port);
	    serv_addr.sin_addr.s_addr = INADDR_ANY;
	    int retVal = bind(serverSocket, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	    if (retVal < 0) {
	    	// TODO: handle error case
	        close(serverSocket);
	        return;
	    }

	    // listen on port
	    retVal = listen(serverSocket,ListenBacklog);
	    if (retVal < 0) {
	    	// TODO: handle error case
	        close(serverSocket);
	        return;
	    }

	    // go into select loop
	    while (stopListening == false) {
	        // wait for incoming data and fill the job queue
	        pollfd pollInfo;

	        pollInfo.fd      = serverSocket;
	        pollInfo.events  = POLLIN | POLLPRI;
	        pollInfo.revents = 0;

	        int readsocks = poll(&pollInfo,1,PollTimeout);

	        if (readsocks < 0) {
		    	// TODO: handle error case
	            continue;
			}

	        // check if there is a new connection
	        if (pollInfo.revents > 0) {
	            errno = 0;
	            int newConnection = accept(serverSocket, NULL, NULL);
	            if (newConnection < 0) {
			    	// TODO: handle error case
	                continue;
	            }
	            // make socket non-blocking
	            opts = fcntl(newConnection,F_GETFL);
	            if (opts < 0) {
			    	// TODO: handle error case
	                continue;
	            }
	            opts = (opts | O_NONBLOCK);
	            if (fcntl(newConnection,F_SETFL,opts) < 0) {
			    	// TODO: handle error case
	                continue;
	            }
	            // add connection to a worker
	            // search for worker with lowest number of connections
	            auto min_worker = std::min_element(worker.begin(),
	            								   worker.end(),
	            	[](Worker &a, Worker &b) -> bool {
	            		return a.connection_count() < b.connection_count();
	            	});

	            // add if minsize < MaxConnections
	            if (min_worker->connection_count() < MaxConnections) {
	            	min_worker->add_connection(newConnection);
	            	if (min_worker->new_con_count() > ListenBacklog/2) {
	            		min_worker->break_poll();
	            	}
	            } else {
	                close(newConnection);
	            }
	        }
	    }

	    // close server socket
	    close(serverSocket);			
	}

private:

	std::vector<Worker> worker;
	int port;
	bool stopListening;
	std::thread *ct;	

};


}	


#endif // SERVER_H

