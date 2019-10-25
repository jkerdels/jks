#ifndef JK_CONNECTIONHANDLER_H
#define JK_CONNECTIONHANDLER_H

#include <vector>
#include <atomic>
#include <inttypes.h>

namespace jk {

class ConnectionHandler
{
	const static int ReadBufferSize  = 4096;
	const static int WriteBufferSize = 4096;

public:
	ConnectionHandler();
	ConnectionHandler(const ConnectionHandler &other);
	virtual ~ConnectionHandler();

	ConnectionHandler& operator=(const ConnectionHandler& other);

	void set_sockets(int _socket, int _spooler);	

	void read_from_socket();
	void write_to_socket();

	bool write_pending() const;
	bool connection_closed() const;

	// when C++20 is available use span
	virtual void process_data(const uint8_t *data, size_t data_size) = 0;
	void send_data(const std::vector<uint8_t> &data);
	void close_connection();


private:
	int socket;
	int spooler;

	void poke_spooler();

	bool conClosed;

	uint8_t readBuffer[ReadBufferSize];
	uint8_t writeBuffer[WriteBufferSize];

	std::vector<uint8_t> sendBuffer[2];
	int curSendBuffer;
	int curSendPos;
	bool writePending;

	std::atomic_flag sendBufLock;

};

}

#endif // JK_CONNECTIONHANDLER_H
