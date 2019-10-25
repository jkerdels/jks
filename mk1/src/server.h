#ifndef JK_SERVER_H
#define JK_SERVER_H

#include <cstring>
#include <algorithm>

#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <tuple>

#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <poll.h>

#define PollTimeout    100
#define ListenBacklog  128
#define MaxConnections 1000
#define MaxThreadCount 20


namespace jk {

template<class ConHandler>
struct Connection 
{
	Connection() :
		fd(),
		handler()
	{}

	Connection(const Connection<ConHandler> &other) :
		fd(other.fd),
		handler(other.handler)
	{}

	Connection<ConHandler>& operator=(Connection<ConHandler> &other)
	{
		fd = other.fd;
		handler = other.handler;
		return *this;
	}

	int        fd;
	ConHandler handler;
};


template<class ConHandler>
class Spooler 
{
public:

	Spooler():
		connections(),
		newConnections(),
		pollSet(),
		localPipe(),
		lpPoll(),
		stopPolling(true),
		curThread(nullptr),
		conLock(ATOMIC_FLAG_INIT)
	{
		int rv = pipe2(localPipe,O_NONBLOCK);
		if (rv < 0) {
			// TODO: handle error of pipe
		}
		lpPoll.fd      = localPipe[0];
		lpPoll.events  = POLLIN | POLLPRI;
		lpPoll.revents = 0;
	}

	Spooler(const Spooler<ConHandler> &other) :
		connections(),
		newConnections(),
		pollSet(),
		localPipe(),
		lpPoll(other.lpPoll),
		stopPolling(other.stopPolling),
		curThread(other.curThread),
		conLock(ATOMIC_FLAG_INIT)	
	{
		connections.assign(other.connections.begin(),
			               other.connections.end());
		newConnections.assign(other.newConnections.begin(),
			                  other.newConnections.end());
		pollSet.assign(other.pollSet.begin(),
					   other.pollSet.end());
		localPipe[0] = other.localPipe[0];
		localPipe[1] = other.localPipe[1];
	}

	~Spooler() {
		finish();
	}

	Spooler<ConHandler>& operator=(Spooler<ConHandler> &other)
	{
		connections.assign(other.connections.begin(),
			               other.connections.end());
		newConnections.assign(other.newConnections.begin(),
			                  other.newConnections.end());
		pollSet.assign(other.pollSet.begin(),
					   other.pollSet.end());
		localPipe[0] = other.localPipe[0];
		localPipe[1] = other.localPipe[1];
		lpPoll = other.lpPoll;
		stopPolling = other.stopPolling;
		curThread = other.curThread;
		return *this;
	}

	void start() {
		finish();

		stopPolling = false;
		curThread = new std::thread(*this);			
	}

	void break_poll() {
		// write something to local pipe to make poll return
		uint8_t data = 1;
		ssize_t result = write(localPipe[1],&data,1);	
	}

	void finish() {
		if (curThread == nullptr) {
			return;
		}

		stopPolling = true;
		break_poll();

		curThread->join();

		delete curThread;
		curThread = nullptr;				
	}

	void add_connection(int fd) {
		lock_connections();
		// don't accept connections when finished or not started
		if (stopPolling == true) {
			close(fd);
			unlock_connections();
			return;
		}
		Connection<ConHandler> newCon;
		newCon.fd = fd;
		/*
		if constexpr (std::tuple_size<std::tuple<ConHandlerArgs...>>::value > 0) {
			std::apply([&](ConHandlerArgs&&... args){ 
				newCon.handler.init(std::forward<ConHandlerArgs>(args)...); 
			}, conHandlerArgs);
		}
		*/
		newConnections.push_back(newCon);
		unlock_connections();
		break_poll();			
	}

	size_t connection_count() {
		size_t result = 0;
		lock_connections();
		result = connections.size();
		unlock_connections();
		return result;			
	}

	size_t new_connection_count() {
		size_t result = 0;
		lock_connections();
		result = newConnections.size();
		unlock_connections();
		return result;			
	}

	void operator()() {
		while (stopPolling == false) {
			// add new connections
			lock_connections();
			connections.insert(connections.end(),newConnections.begin(),newConnections.end());
			newConnections.clear();
			unlock_connections();

			// rebuilt pollSet
			size_t con_size = connections.size();

			if (con_size == 0) {
				std::this_thread::yield();
				continue;
			}

			pollSet.resize(con_size+1);
			for (size_t i = 0; i < con_size; ++i) {
				pollSet[i].fd      = connections[i].fd;
				pollSet[i].events  = POLLIN | POLLPRI;
				pollSet[i].revents = 0;
				if (connections[i].handler.write_pending() == true) {
					pollSet[i].events |= POLLOUT;
				}
			}
			pollSet[con_size] = lpPoll;

			poll(pollSet.data(),pollSet.size(),PollTimeout);

			for (auto ps_entry : pollSet) {
				
				if (ps_entry.revents == 0) continue;			

				// find handler
				auto con_it = std::lower_bound(
							  	connections.begin(),connections.end(),
							  	ps_entry.fd, 
							  	[](const Connection<ConHandler> &a, const Connection<ConHandler> &b) -> bool {
							  		return a.fd < b.fd;
							  });

				if ((con_it == connections.end()) || (con_it->fd != ps_entry.fd)) {
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
					con_it->handler.read_from_socket();
				}
				if (ps_entry.revents & POLLOUT) {
					con_it->handler.write_to_socket();
				}
				if (con_it->handler.connection_closed() == true) {
					close(ps_entry.fd);
				}
			}

			// remove closed connections from vector connections
			lock_connections();
			connections.erase(
				std::remove_if(connections.begin(),connections.end(),
							   [](auto &a) -> bool {
								  return a.handler.connection_closed();
							  }),
				connections.end()
			);
			unlock_connections();
		}

		// close all remaining connections
		lock_connections();
		for (auto &c : connections) {
			close(c.fd);
		}
		connections.clear();

		for (auto &c : newConnections) {
			close(c.fd);
		}
		newConnections.clear();
		unlock_connections();
	}
private:

	void lock_connections() {
		while(conLock.test_and_set());
	}

	void unlock_connections() {
		conLock.clear();
	}

	std::vector<Connection<ConHandler>> connections;
	std::vector<Connection<ConHandler>> newConnections;
	std::vector<struct pollfd> pollSet;

	int localPipe[2];
	struct pollfd lpPoll;

	bool stopPolling;

	std::thread *curThread;

	std::atomic_flag conLock;
};

template<class ConHandler>
class Server;

template<class ConHandler>
class Listener 
{
public:

	Listener(Server<ConHandler> *_parent) :
		parent(_parent),
		port(-1),
		stopListening(false),
		curThread(nullptr)
	{}

	~Listener() {
		finish();
	}

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
	    memset(&serv_addr,0,sizeof(serv_addr));

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
	        struct pollfd pollInfo;

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
	            // add connection to a spooler
	            // search for spooler with lowest number of connections
	            auto min_spool = std::min_element(parent->spooler.begin(),
	            								  parent->spooler.end(),
	            	[](Spooler<ConHandler> &a, Spooler<ConHandler> &b) -> bool {
	            		return a.connection_count() < b.connection_count();
	            	});

	            // add if minsize < MaxConnections
	            if (min_spool->connection_count() < MaxConnections) {
	            	min_spool->add_connection(newConnection);
	            	if (min_spool->new_connection_count() > ListenBacklog/2) {
	            		min_spool->break_poll();
	            	}
	            } else {
	                close(newConnection);
	            }
	        }
	    }

	    // close server socket
	    close(serverSocket);			
	}

	void set_port(int _port) {
		port = _port;
	}

	void start() {
		finish();

		stopListening = false;
		curThread = new std::thread(*this);			
	}

	void finish() {
		if (curThread == nullptr) {
			return;
		}

		stopListening = true;

		curThread->join();

		delete curThread;
		curThread = nullptr;			
	}

private:
	Server<ConHandler> *parent;

	int port;

	bool stopListening;

	std::thread *curThread;
};



template<class ConHandler>
class Server
{
	friend class Listener<ConHandler>;
public:

	Server() : 
		spooler(),
		listener(this)
	{}
	~Server() 
	{
		stop_serving();
	}

	//void set_handler_arguments(ConHandlerArgs&&... args);

	void start_serving(int port, int nr_of_threads) {
		if (port < 1) port = 1;

		if (nr_of_threads < 2) nr_of_threads = 2;

		if (nr_of_threads > MaxThreadCount) nr_of_threads = MaxThreadCount;

		spooler.resize(nr_of_threads-1);
		for (size_t i = 0; i < spooler.size(); ++i) {
			spooler[i].start();
		}
		/*
		for (auto &s : spooler) {
			s.start();
		}
		*/

		listener.set_port(port);
		listener.start();		
	}

	void stop_serving()	{
		listener.finish();

		for (auto &s : spooler) {
			s.finish();
		}		
	}

private:
	//std::tuple<ConHandlerArgs...> conHandlerArgs;
	std::vector<Spooler<ConHandler>> spooler;

	Listener<ConHandler> listener;
	
};

}

#endif // SERVER_H




