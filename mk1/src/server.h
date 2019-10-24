#ifndef SERVER_H
#define SERVER_H

#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <tuple>

#include <poll.h>

using namespace jk;

template<class ConHandler, typename... ConHandlerArgs>
class Server
{
public:
	static const int PollTimeout    = 100;
	static const int ListenBacklog  = 128;
	static const int MaxConnections = 1000;
	static const int MaxThreadCount = 20;

	Server(ConHandlerArgs&&... args);
	~Server();

	void startServing(int port, int nr_of_threads);
	void stopServing();

private:
	std::tuple<ConHandlerArgs...> conHandlerArgs;

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
	class Spooler 
	{
	public:

		Spooler();
		~Spooler();

		void start();

		void update();

		void finish();

		void add_connection(int fd);

		size_t connection_count();
		size_t new_connection_count();

	private:
		void operator();

		void break_poll();

		void lock_connections();
		void unlock_connections();

		struct Connection 
		{
			int        fd;
			ConHandler handler;
		};

		std::vector<Connection>    connections;
		std::vector<Connection>    newConnections;
		std::vector<struct pollfd> pollSet;

		int localPipe[2];
		struct pollfd lpPoll;

		bool stopPolling;

		std::thread *curThread;

		std::atomic_flag conLock;
	};
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
	class Listener 
	{
	public:

		Listener();
		~Listener();

		void operator();

		void set_port(int _port);

		void start();

		void finish();

	private:

		int port;

		bool stopListening;

		std::thread *curThread;
	};
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

	std::vector<Spooler> spooler;

	Listener listener;
	
};

#endif // SERVER_H




