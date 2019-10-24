#include <algorithm>

#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "server.h"

template<class ConHandler, typename... ConHandlerArgs>
jk::Server::Server(ConHandlerArgs&&... args) :
	conHandlerArgs(std::forward<ConHandlerArgs>(args)...),
	spooler(),
	listener()
{

}

template<class ConHandler, typename... ConHandlerArgs>
jk::Server::~Server()
{
	stopServing();
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::startServing(int port, int nr_of_threads)
{
	if (port < 1) {
		port = 1;
	}
	if (nr_of_threads < 2) {
		nr_of_threads = 2;
	}
	if (nr_of_threads > MaxThreadCount) {
		nr_of_threads = MaxThreadCount;
	}

	spooler.resize(nr_of_threads-1);
	for (auto &s : spooler) {
		s.start();
	}

	listener.set_port(port);
	listener.start();
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::stopServing()
{
	listener.finish();

	for (auto &s : spooler) {
		s.finish();
	}
}


//------------------------------------------------------------------------
//------------------------------------------------------------------------
template<class ConHandler, typename... ConHandlerArgs>
jk::Server::Spooler::Spooler() :
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

template<class ConHandler, typename... ConHandlerArgs>
jk::Server::Spooler::~Spooler()
{
	finish();
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::start()
{
	finish();

	stopPolling = false;
	curThread = new std::thread(*this);
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::update()
{
	break_poll();
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::finish()
{
	if (curThread == nullptr) {
		return;
	}

	stopPolling = true;
	break_poll();

	curThread->join();

	delete curThread;
	curThread = nullptr;	
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::add_connection(int fd)
{
	lock_connections();
	// don't accept connections when finished or not started
	if (stopPolling == true) {
		close(fd);
		unlock_connections();
		return;
	}
	Connection newCon;
	newCon.fd = fd;
	if constexpr (std::tuple_size<std::tuple<ConHandlerArgs...>>::value > 0) {
		std::apply([&](ConHandlerArgs&&... args){ 
			newCon.handler.init(std::forward<ConHandlerArgs>(args)...); 
		}, conHandlerArgs);
	}
	newConnections.push_back(newCon);
	unlock_connections();
	update();
}

template<class ConHandler, typename... ConHandlerArgs>
size_t jk::Server::Spooler::connection_count()
{
	size_t result = 0;
	lock_connections();
	result = connections.size();
	unlock_connections();
	return result;
}

template<class ConHandler, typename... ConHandlerArgs>
size_t jk::Server::Spooler::new_connection_count()
{
	size_t result = 0;
	lock_connections();
	result = newConnections.size();
	unlock_connections();
	return result;
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::operator()
{
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

		poll(pollSet.data(),pollSet.size(),Server<ConHandler>::PollTimeout);

		for (auto ps_entry : pollSet) {
			
			if (ps_entry.revents == 0) continue;			

			// find handler
			auto con_it = std::lower_bound(
						  	connections.begin(),connections.end(),
						  	ps_entry.fd, 
						  	[](const Connection &a, const Connection &b,) -> bool {
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
				con_it->handler.read();
			}
			if (ps_entry.revents & POLLOUT) {
				con_it->handler.write();
			}
			if (con_it->handler.connection_closed() == true) {
				close(ps_entry.fd);
			}
		}

		// remove closed connections from vector connections
		lock_connections();
		connections.erase(
			std::remove_if(connections.begin(),connections.end(),
						   []](const Connection &a) -> bool {
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

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::break_poll()
{
	// write something to local pipe to make poll return
	uint8_t data = 1;
	write(localPipe[1],&data,1);	
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::lock_connections()
{
	while(conLock.test_and_set());
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Spooler::unlock_connections()
{
	conLock.clear();
}


//------------------------------------------------------------------------
//------------------------------------------------------------------------
template<class ConHandler, typename... ConHandlerArgs>
jk::Server::Listener::Listener() :
	port(-1),
	stopListening(false),
	curThread(nullptr)
{

}

template<class ConHandler, typename... ConHandlerArgs>
jk::Server::Listener::~Listener()
{
	finish();
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Listener::operator()
{
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
            auto min_spool = std::min_element(spooler.begin(),spooler.end(),
            	[](const Spooler &a, const Spooler &b) -> bool {
            		return a.connection_count() < b.connection_count();
            	});

            // add if minsize < MaxConnections
            if (min_spool->connection_count() < MaxConnections) {
            	min_spool->add_connection(newConnection);
            	if (min_spool->new_connection_count() > ListenBacklog/2) {
            		min_spool->update();
            	}
            } else {
                close(newConnection);
            }
        }
    }

    // close server socket
    close(serverSocket);

}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Listener::set_port(int _port)
{
	port = _port;
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Listener::start()
{
	finish();

	stopListening = false;
	curThread = new std::thread(*this);
}

template<class ConHandler, typename... ConHandlerArgs>
void jk::Server::Listener::finish()
{
	if (curThread == nullptr) {
		return;
	}

	stopListening = true;

	curThread->join();

	delete curThread;
	curThread = nullptr;
}

