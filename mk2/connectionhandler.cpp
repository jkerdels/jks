#include <cstring>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "connectionhandler.h"

jk::ConnectionHandler::ConnectionHandler() :
	socket(),
	spooler(),
	conClosed(false),
	readBuffer(),
	writeBuffer(),
	sendBuffer(),
	curSendBuffer(0),
	curSendPos(0),
	writePending(false),
	sendBufLock(ATOMIC_FLAG_INIT)
{
	
}

jk::ConnectionHandler::ConnectionHandler(const ConnectionHandler &other) :
	socket(other.socket),
	spooler(other.spooler),
	conClosed(other.conClosed),
	readBuffer(),
	writeBuffer(),
	sendBuffer(),
	curSendBuffer(other.curSendBuffer),
	curSendPos(other.curSendPos),
	writePending(other.writePending),
	sendBufLock(ATOMIC_FLAG_INIT)
{
	std::memcpy(readBuffer,other.readBuffer,ReadBufferSize);
	std::memcpy(writeBuffer,other.writeBuffer,WriteBufferSize);
	sendBuffer[0].assign(other.sendBuffer[0].begin(),
		                 other.sendBuffer[0].end());
	sendBuffer[1].assign(other.sendBuffer[1].begin(),
		                 other.sendBuffer[1].end());
}

jk::ConnectionHandler::~ConnectionHandler()
{
}

jk::ConnectionHandler& jk::ConnectionHandler::operator=(const ConnectionHandler& other)
{
	socket        = other.socket;
	spooler       = other.spooler;
	conClosed     = other.conClosed;
	std::memcpy(readBuffer,other.readBuffer,ReadBufferSize);
	std::memcpy(writeBuffer,other.writeBuffer,WriteBufferSize);
	sendBuffer[0].assign(other.sendBuffer[0].begin(),
		                 other.sendBuffer[0].end());
	sendBuffer[1].assign(other.sendBuffer[1].begin(),
		                 other.sendBuffer[1].end());
	curSendBuffer = other.curSendBuffer;
	curSendPos    = other.curSendPos;
	writePending  = other.writePending;

	return *this;
}

void jk::ConnectionHandler::set_sockets(int _socket, int _spooler)
{
	socket  = _socket;
	spooler = _spooler;
}

void jk::ConnectionHandler::read_from_socket()
{
	if (conClosed == true) return;

    std::memset(readBuffer,0,ReadBufferSize);
    errno = 0;
    ssize_t bytes_read = recv(socket,readBuffer,ReadBufferSize,0);
	int con_error_type = errno;

	if (bytes_read > 0) {
		process_data(readBuffer,(size_t)bytes_read);
		return;
	}

	// error handling
	if (bytes_read == 0) {
		// client has closed the connection
		conClosed = true;
		return;
	}

	if ((con_error_type == EAGAIN) ||
		(con_error_type == EWOULDBLOCK)) 
	{
		// just try again		
		return;
	}

	// bytes_read are negative -> an error occurred
	switch (con_error_type) {
		case EBADF : {
			// The argument sockfd is an invalid descriptor.
		} break;

		case ECONNREFUSED : {
			// A remote host refused to allow the network connection (typically because it is not running the requested service). 
		} break;

		case EFAULT : {
			// The receive buffer pointer(s) point outside the process's address space.
		} break;

		case EINTR : {
			// The receive was interrupted by delivery of a signal before any data were available; see signal(7).
		} break;

		case EINVAL : {
			// Invalid argument passed.
		} break;

		case ENOMEM : {
			// Could not allocate memory for recvmsg().
		} break;

		case ENOTCONN : {
			// The socket is associated with a connection-oriented protocol and has not been connected (see connect(2) and accept(2)). 
		} break;

		case ENOTSOCK : {
			// The argument sockfd does not refer to a socket. 
		} break;

		default : {
			// unkown recv error (which can happen, i.e., is allowed by posix)
		}
	}

	// close the connection
	conClosed = true;
}

void jk::ConnectionHandler::write_to_socket()
{
	while(sendBufLock.test_and_set());
	int availableBytes = (int)(sendBuffer[curSendBuffer].size()) - curSendPos;
	if (availableBytes > 0) {
		int bytesToWrite = availableBytes < WriteBufferSize ? 
		                   	 availableBytes : WriteBufferSize;
		errno = 0;
		int bytesWritten = send(socket,
			                 sendBuffer[curSendBuffer].data()+curSendPos,
			                 bytesToWrite,MSG_NOSIGNAL);
		int con_error_type = errno;
		if (bytesWritten < 0) {
			// error sending
			if ((con_error_type != EAGAIN) &&
				(con_error_type != EWOULDBLOCK)) 
			{
				// don't try again, just close the connection
				conClosed = true;
			}
			sendBufLock.clear();
			return;
		}
		if (bytesWritten == 0) {
			// client closed connection, we do the same
			conClosed = true;
			sendBufLock.clear();
			return;
		}
		// we did write something
		availableBytes -= bytesWritten;
		curSendPos += bytesWritten;
		if (availableBytes == 0) {
			// we wrote the whole buffer!
			curSendPos = 0;
			sendBuffer[curSendBuffer].clear();
			writePending = false;
		}
	}
	int otherBuffer = (curSendBuffer + 1) & 1;
	size_t otherBufSize = sendBuffer[otherBuffer].size();
	if ((availableBytes == 0) && (otherBufSize > 0)) {
		// we are done sending the first buffer and the second
		// has data waiting. So switch the buffers, reset sendpos,
		// and make sure write Pending is still set
		curSendBuffer = otherBuffer;
		curSendPos    = 0;
		writePending  = true;
	}
	sendBufLock.clear();
}

bool jk::ConnectionHandler::write_pending() const
{
	return writePending;
}

bool jk::ConnectionHandler::connection_closed() const
{
	return conClosed;
}

void jk::ConnectionHandler::send_data(const std::vector<uint8_t> &data)
{
	while(sendBufLock.test_and_set());
	if (writePending == false) {
		writePending = true;
		sendBuffer[curSendBuffer].assign(data.begin(),data.end());		
		poke_spooler();
	} else {
		int otherBuffer = (curSendBuffer + 1) & 1;
		sendBuffer[otherBuffer].insert(sendBuffer[otherBuffer].end(),data.begin(),data.end());
	}
	sendBufLock.clear();
}

void jk::ConnectionHandler::close_connection()
{
	conClosed = true;
}

void jk::ConnectionHandler::poke_spooler()
{
	uint8_t data = 1;
	ssize_t result = write(spooler,&data,(size_t)1);	
}

