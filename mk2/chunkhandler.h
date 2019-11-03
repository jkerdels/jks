#ifndef JK_CHUNKHANDLER_H
#define JK_CHUNKHANDLER_H

#include <vector>
#include <atomic>
#include <inttypes.h>
#include <functional>
#include <unordered_map>

namespace jk {

class ChunkHandler
{
	const static int ReadBufferSize  = 4096;
	const static int WriteBufferSize = 4096;
	const static int MaxNrOfStartIDs = 10;
	const static int MaxNrOfChunks   = 100;

	const static int StartIDSearch = 0;
	const static int ReadChunkID   = 10;
	const static int ReadChunkSize = 20;
	const static int ReadData      = 30;

public:
	ChunkHandler();
	ChunkHandler(const ChunkHandler &other);
	~ChunkHandler();

	ChunkHandler& operator=(const ChunkHandler& other);

	void set_sockets(int _socket, int _spooler);	

	void read_from_socket();
	void write_to_socket();

	bool write_pending() const;
	bool connection_closed() const;

	// when C++20 is available use span
	void process_data(const uint8_t *data, size_t data_size);
	void send_data(const std::vector<uint8_t> &data);
	void close_connection();

	// chunk service routines
	void add_start_id(uint32_t id);
	void add_start_id(const char *id);

	void remove_start_id(uint32_t id);
	void remove_start_id(const char *id);

	void add_chunk_handler(uint32_t start_id,
		                   uint32_t chunk_id, 
		                   std::function<void(const std::vector<uint8_t> &data)> handler, 
		                   uint32_t max_size = 0);

	void add_chunk_handler(const char *start_id,
		                   const char *chunk_id, 
		                   std::function<void(const std::vector<uint8_t> &data)> handler, 
		                   uint32_t max_size = 0);

	void remove_chunk_handler(uint32_t start_id,
		                      uint32_t chunk_id);

	void remove_chunk_handler(const char *start_id,
		                      const char *chunk_id);

private:
	uint32_t chunk_from_str(const char *id);

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

	// chunk foo
	uint32_t start_ids[MaxNrOfStartIDs];

	uint64_t chunk_ids[MaxNrOfChunks];
	uint32_t chunk_max_sizes[MaxNrOfChunks];
	std::function<void(const std::vector<uint8_t>&)> chunk_handlers[MaxNrOfChunks];

	uint32_t cur_start_id;
	uint32_t cur_chunk_id;
	int      cur_chunk_idx;
	uint32_t cur_chunk_size;

	std::vector<uint8_t> cur_chunk_data;

	uint32_t cur_state;
	uint32_t tmp_cnt;
};

}

#endif // JK_CHUNKHANDLER_H
