
#include <thread>

#include "server.h"
#include "echohandler.h"

#include <signal.h>

using namespace jk;

bool stopServing;

void signal_callback_handler(int signum)
{
    if (signum == SIGTERM)
        stopServing = true;
}

int main(int argc, char *argv[]) {

    stopServing = false;
    signal(SIGTERM, signal_callback_handler);

	Server<EchoHandler> server;

	server.start_serving(8080,4);

    while (!stopServing) {
        std::this_thread::yield();
    }

    server.stop_serving();

	return 0;
}


