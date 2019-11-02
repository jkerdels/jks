#include <cstring>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "chunkhandler.h"

jk::ChunkHandler::ChunkHandler() :
	socket(),
	spooler(),
	conClosed(false),
	readBuffer(),
	writeBuffer(),
	sendBuffer(),
	curSendBuffer(0),
	curSendPos(0),
	writePending(false),
	sendBufLock(ATOMIC_FLAG_INIT),
	start_ids(),
	chunk_ids(),
	chunk_handlers(),
	chunk_max_sizes(),
	cur_start_id(0),
	cur_chunk_id(0),
	cur_chunk_idx(0),
	cur_chunk_data(),
	cur_state(0),
	cur_data_bytes_remaining(0)
{
	std::memset(start_ids,       0, MaxNrOfStartIDs * sizeof(start_ids[0]));
	std::memset(chunk_ids,       0, MaxNrOfChunks   * sizeof(chunk_ids[0]));
	std::memset(chunk_handlers,  0, MaxNrOfChunks   * sizeof(chunk_handlers[0]));
	std::memset(chunk_max_sizes, 0, MaxNrOfChunks   * sizeof(chunk_max_sizes[0]));	
}

jk::ChunkHandler::ChunkHandler(const ChunkHandler &other) :
	socket(other.socket),
	spooler(other.spooler),
	conClosed(other.conClosed),
	readBuffer(),
	writeBuffer(),
	sendBuffer(),
	curSendBuffer(other.curSendBuffer),
	curSendPos(other.curSendPos),
	writePending(other.writePending),
	sendBufLock(ATOMIC_FLAG_INIT),
	start_ids(),
	chunk_ids(),
	chunk_handlers(),
	chunk_max_sizes(),
	cur_start_id(other.cur_start_id),
	cur_chunk_id(other.cur_chunk_id),
	cur_chunk_idx(other.cur_chunk_idx),
	cur_chunk_data(other.cur_chunk_data),
	cur_state(other.cur_state),
	cur_data_bytes_remaining(other.cur_data_bytes_remaining)
{
	std::memcpy(readBuffer,other.readBuffer,ReadBufferSize);
	std::memcpy(writeBuffer,other.writeBuffer,WriteBufferSize);
	sendBuffer[0].assign(other.sendBuffer[0].begin(),
		                 other.sendBuffer[0].end());
	sendBuffer[1].assign(other.sendBuffer[1].begin(),
		                 other.sendBuffer[1].end());
	std::memcpy(start_ids,      other.start_ids,      MaxNrOfStartIDs * sizeof(start_ids[0]));
	std::memcpy(chunk_ids,      other.chunk_ids,      MaxNrOfChunks *   sizeof(chunk_ids[0]));
	std::memcpy(chunk_handlers, other.chunk_handlers, MaxNrOfChunks *   sizeof(chunk_handlers[0]));
	std::memcpy(chunk_max_sizes,other.chunk_max_sizes,MaxNrOfChunks *   sizeof(chunk_max_sizes[0]));
}

jk::ChunkHandler::~ChunkHandler()
{
}

jk::ChunkHandler& jk::ChunkHandler::operator=(const ChunkHandler& other)
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

	std::memcpy(start_ids,      other.start_ids,      MaxNrOfStartIDs * sizeof(start_ids[0]));
	std::memcpy(chunk_ids,      other.chunk_ids,      MaxNrOfChunks *   sizeof(chunk_ids[0]));
	std::memcpy(chunk_handlers, other.chunk_handlers, MaxNrOfChunks *   sizeof(chunk_handlers[0]));
	std::memcpy(chunk_max_sizes,other.chunk_max_sizes,MaxNrOfChunks *   sizeof(chunk_max_sizes[0]));

	cur_start_id             = other.cur_start_id;
	cur_chunk_id             = other.cur_chunk_id;
	cur_chunk_idx            = other.cur_chunk_idx;
	cur_chunk_data           = other.cur_chunk_data;
	cur_state                = other.cur_state;
	cur_data_bytes_remaining = other.cur_data_bytes_remaining;

	return *this;
}

void jk::ChunkHandler::set_sockets(int _socket, int _spooler)
{
	socket  = _socket;
	spooler = _spooler;
}

void jk::ChunkHandler::read_from_socket()
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

void jk::ChunkHandler::write_to_socket()
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

bool jk::ChunkHandler::write_pending() const
{
	return writePending;
}

bool jk::ChunkHandler::connection_closed() const
{
	return conClosed;
}

void jk::ChunkHandler::send_data(const std::vector<uint8_t> &data)
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

void jk::ChunkHandler::close_connection()
{
	conClosed = true;
}

void jk::ChunkHandler::poke_spooler()
{
	uint8_t data = 1;
	ssize_t result = write(spooler,&data,(size_t)1);	
}

void jk::ChunkHandler::process_data(const uint8_t *data, size_t data_size)
{
	// chunk state machine
}

void jk::ChunkHandler::add_start_id(uint32_t id)
{
	if (id == 0) {
		// id 0 not allowed
		return;
	}

	bool found_slot = false;
	for (int i = 0; i < MaxNrOfStartIDs-1; ++i) {
		if (start_ids[i] == 0) {
			found_slot = true;
			start_ids[i] = id;
			break;
		} 
	}
	if (!found_slot) {
		// handle error
	}
}

void jk::ChunkHandler::add_start_id(const char *id)
{
	add_start_id(chunk_from_str(id));
}

void jk::ChunkHandler::remove_start_id(uint32_t id)
{
	if (id == 0) return;

	int i = 0;
	while ((start_ids[i]) && (start_ids[i] != id)) {
		++i;
	}

	if (start_ids[i]) {
		do {
			start_ids[i] = start_ids[i+1];
		} while (start_ids[i++]);
	}
}

void jk::ChunkHandler::remove_start_id(const char *id)
{
	remove_start_id(chunk_from_str(id));
}

void jk::ChunkHandler::add_chunk_handler(uint32_t start_id,
	                   uint32_t chunk_id, 
	                   std::function<void(const std::vector<uint8_t> &data)> handler, 
	                   uint32_t max_size)
{}

void jk::ChunkHandler::add_chunk_handler(const char *start_id,
	                   const char *chunk_id, 
	                   std::function<void(const std::vector<uint8_t> &data)> handler, 
	                   uint32_t max_size)
{}

void jk::ChunkHandler::remove_chunk_handler(uint32_t start_id,
	                      uint32_t chunk_id)
{}

void jk::ChunkHandler::remove_chunk_handler(const char *start_id,
	                      const char *chunk_id)
{}

uint32_t jk::ChunkHandler::chunk_from_str(const char *id)
{
	uint32_t result = 0;

	if (id == nullptr) return 0;

	int i = 0;
	uint32_t cc = 0;
	while((cc = static_cast<uint32_t>(id[i])) && (i < 4)) {
		result |= cc << (i++ * 8);
	}
	return result;
}


