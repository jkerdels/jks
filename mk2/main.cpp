#include <cstdio>
#include <thread>

#include <chrono>
using namespace std::chrono_literals;

#include "server.h"
#include "echohandler.h"

#include <signal.h>

using namespace jk;

bool stopServing;

void signal_callback_handler(int signum)
{
    if (signum == SIGTERM) {
        std::printf("received SIGTERM\n");
        stopServing = true;
    }
    if (signum == SIGINT) {
        std::printf("received SIGINT\n");
        stopServing = true;
    }
}

int main(int argc, char *argv[]) {

    stopServing = false;
    signal(SIGTERM, signal_callback_handler);
    signal(SIGINT, signal_callback_handler);

	Server<EchoHandler> server;

    server.set_ch_params(10,nullptr);

	server.start_serving(8080,2);

    while (!stopServing) {
        std::this_thread::sleep_for(50ms);
    }

    server.stop_serving();

	return 0;
}


