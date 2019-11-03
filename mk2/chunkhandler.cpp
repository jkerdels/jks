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
	cur_chunk_size(0),
	cur_chunk_data(),
	cur_state(StartIDSearch),
	tmp_cnt(0)
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
	cur_chunk_size(other.cur_chunk_size),
	cur_chunk_data(other.cur_chunk_data),
	cur_state(other.cur_state),
	tmp_cnt(other.tmp_cnt)
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
	cur_chunk_size           = other.cur_chunk_size;
	cur_chunk_data           = other.cur_chunk_data;
	cur_state                = other.cur_state;
	tmp_cnt                  = other.tmp_cnt;

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
	uint8_t *dp = const_cast<uint8_t*>(data);
	uint8_t *ep = dp + data_size;
	while (dp < ep) {

		uint8_t cur_byte = *(dp++);

		switch (cur_state) {

			case StartIDSearch : {
				cur_start_id = (cur_start_id << 8) | (uint32_t)cur_byte;
				int i = 0;
				while ((start_ids[i]) && (start_ids[i] != cur_start_id)) ++i;
				if (start_ids[i]) {
					tmp_cnt   = 0;
					cur_state = ReadChunkID;
				}
			} break;

			case ReadChunkID : {
				cur_chunk_id = (cur_chunk_id << 8) | (uint32_t)cur_byte;
				if (++tmp_cnt == 4) {
					 uint64_t cid = ((uint64_t)cur_start_id << 32) | (uint64_t)cur_chunk_id;
					 int cur_chunk_idx = 0;
					 while ((chunk_ids[cur_chunk_idx]) && (chunk_ids[cur_chunk_idx] != cid))
					 	++cur_chunk_idx;
					 if (chunk_ids[cur_chunk_idx]) {
					 	tmp_cnt   = 0;
					 	cur_state = ReadChunkSize;
					 } else {
					 	// unknown chunk id
					 	cur_start_id = 0;
					 	cur_chunk_id = 0;
					 	cur_state = StartIDSearch;
					 	// try to backtrack 4 bytes if possible
					 	dp -= (dp - data > 4) ? 4 : dp - data;
					 }
				} 
			} break;

			case ReadChunkSize : {
				cur_chunk_size = (cur_chunk_size << 8) | (uint32_t)cur_byte;
				if (++tmp_cnt == 4) {
					if ((cur_chunk_size > chunk_max_sizes[cur_chunk_idx]) && 
						(chunk_max_sizes[cur_chunk_idx]))
					{
						// chunk is too big
					 	cur_start_id   = 0;
					 	cur_chunk_id   = 0;
					 	cur_chunk_size = 0;
					 	cur_state = StartIDSearch;
					} else {
						cur_chunk_data.clear();
						cur_chunk_data.reserve(cur_chunk_size);
						cur_state = ReadData;
					}
				}
			} break;

			case ReadData : {
				cur_chunk_data.push_back(cur_byte);
				if (cur_chunk_data.size() == cur_chunk_size) {
					chunk_handlers[cur_chunk_idx](cur_chunk_data);					
				 	cur_start_id   = 0;
				 	cur_chunk_id   = 0;
				 	cur_chunk_size = 0;
				 	cur_state = StartIDSearch;
				}
			} break;

			default : {
				// unknown state
			}
		}
	}
}

void jk::ChunkHandler::add_start_id(uint32_t id)
{
	if (id == 0) {
		// id 0 not allowed
		return;
	}

	int i = 0;
	while (start_ids[i]) ++i;

	if (i == MaxNrOfStartIDs-1) {
		// not enough space for another start id
		return;
	}
	start_ids[i] = id;
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
{
	if ((start_id == 0) || (chunk_id == 0) || (handler == nullptr)) return;

	int i = 0;
	while(chunk_ids[i]) ++i;

	if (i == MaxNrOfChunks-1) {
		// not enough space for another chunk
		return;
	}

	uint64_t chunk_idx = ((uint64_t)start_id << 32) | (uint64_t)chunk_id;
	chunk_ids[i]       = chunk_idx;
	chunk_max_sizes[i] = max_size;
	chunk_handlers[i]  = handler;

}

void jk::ChunkHandler::add_chunk_handler(const char *start_id,
	                   const char *chunk_id, 
	                   std::function<void(const std::vector<uint8_t> &data)> handler, 
	                   uint32_t max_size)
{
	add_chunk_handler(chunk_from_str(start_id),chunk_from_str(chunk_id),handler,max_size);
}

void jk::ChunkHandler::remove_chunk_handler(uint32_t start_id,
	                      uint32_t chunk_id)
{
	uint64_t chunk_idx = ((uint64_t)start_id << 32) | (uint64_t)chunk_id;
	int i = 0;
	while((chunk_ids[i]) && (chunk_ids[i] != chunk_idx)) ++i;

	if (chunk_ids[i]) {
		do {
			chunk_ids[i]       = chunk_ids[i+1];
			chunk_max_sizes[i] = chunk_max_sizes[i+1];
			chunk_handlers[i]  = chunk_handlers[i+1];
		} while (chunk_ids[i++]);
	}
}

void jk::ChunkHandler::remove_chunk_handler(const char *start_id,
	                      const char *chunk_id)
{
	remove_chunk_handler(chunk_from_str(start_id),chunk_from_str(chunk_id));
}

uint32_t jk::ChunkHandler::chunk_from_str(const char *id)
{
	uint32_t result = 0;

	if (id == nullptr) return 0;

	int i = 0;
	uint32_t cc = 0;
	while((cc = (uint32_t)id[i]) && (i < 4)) {
		result |= cc << (i++ * 8);
	}
	return result;
}


