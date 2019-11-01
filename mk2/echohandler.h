#ifndef JK_ECHOHANDLER_H
#define JK_ECHOHANDLER_H

#include <cstdio>
#include <vector>
#include <inttypes.h>

#include "connectionhandler.h"

namespace jk {

class EchoHandler : public ConnectionHandler
{
public:
	EchoHandler() :
		ConnectionHandler(),
		some_int(0),
		some_pointer(nullptr)
	{}

	void process_data(const uint8_t *data, size_t data_size) override {
		std::vector<uint8_t> tmp;
		tmp.assign(data,data+data_size);
		send_data(tmp);
		//std::printf("some_int: %d\n", some_int);
	}

	void set_params(int a, void *p) {
		some_int = a;
		some_pointer = p;
	}

private:
	int some_int;
	void *some_pointer;

};

}

#endif // JK_ECHOHANDLER_H
