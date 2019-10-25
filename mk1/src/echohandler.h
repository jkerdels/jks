#ifndef JK_ECHOHANDLER_H
#define JK_ECHOHANDLER_H

#include <vector>
#include <inttypes.h>

#include "connectionhandler.h"

namespace jk {

class EchoHandler : public ConnectionHandler
{
public:
	EchoHandler(int _socket, int _spooler) :
		ConnectionHandler(_socket,_spooler)
	{}

	void process_data(const uint8_t *data, size_t data_size) override {
		std::vector<uint8_t> tmp;
		tmp.assign(data,data+data_size);
		send_data(tmp);
	}

};

}

#endif // JK_ECHOHANDLER_H
