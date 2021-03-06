
#ifdef __cplusplus
extern "C" {
#endif


#ifndef NBB_H
#define NBB_H

// This doesn't work below XOPEN_SOURCE...doh
#include <time.h>

// Necessary for sys/ipc.h
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>           
#include <poll.h>
#include <unistd.h>
#include <signal.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>

#include <sys/stat.h>
#include <semaphore.h>

#include "constants.h"

#ifdef DEBUG
  #define PRINTF(...) printf(__VA_ARGS__)
#else
  #define PRINTF(...)
#endif

// BUFFER_SIZE is limited to ~32,767 since it has to be represented by an unsigned short / 2
#define BUFFER_SIZE (256 * 4)
#define NUM_ITEMS 500000

// This should be dynamic in the future
#define PAGE_SIZE (4096 * 4) // This should probably be found programatically

// For connected_node struct
// This applies to service name, client name, and so on...
// We hardcode the name size so that we don't have to malloc() in signal handler
#define MAX_NAME_SIZE (128)

// Betting that it's some kind of malloc/mem allocation error...
//
// This crashes erratically
//#define NUM_ITEMS   523909
// This crashes always
//#define NUM_ITEMS   524000

enum { 
  OK = 0,
  BUFFER_FULL, 
  BUFFER_FULL_CONSUMER_READING, 
  BUFFER_EMPTY,
  BUFFER_EMPTY_PRODUCER_INSERTING,
  SHM_ERROR
};

// Callback mechanisms for nbb events
// Hardcode for now. We can generalize the function prototype later.


// New connection event
typedef void (*cb_new_conn_func)(int slot_id, void *arg);
void nbb_set_cb_new_connection(const char* owner, cb_new_conn_func func, void* arg);

// New data event (available to read)
typedef void (*cb_new_data_func)(int slot_id);
void nbb_set_cb_new_data(const char* owner, cb_new_data_func func);

// Allow process to change owner for channel slot
void nbb_set_owner(int slot_id, const char *owner);

struct connected_node {
  int pid;
  char name[MAX_NAME_SIZE];
};

// Simple channel abstraction
struct channel {
	struct buffer *read;
	unsigned char* read_data;
  int read_id;
  int read_count;

	struct buffer *write;
	unsigned char* write_data;
  int write_id;
  int write_count;

  char* owner;
  cb_new_conn_func new_conn;
  cb_new_data_func new_data;
  void* arg;

  int in_use;
};

// Store offset within data region and size of message
struct channel_item {
	unsigned short offset;
	unsigned short size;
};

// This is for a unidirectional buffer
struct buffer {
	// NBB counters
	unsigned short ack_counter;
	unsigned short last_ack_counter;
	unsigned short update_counter;
	unsigned short last_update_counter;
	unsigned short recycle_counter;

	// Offset to data region from buffer*
	// It's probably good to put this on a page boundary
	unsigned short data_offset;
	unsigned short data_size;

	// Array of objs within data region
	struct channel_item items[BUFFER_SIZE];
};

typedef struct delay_buffer
{
  char* content;
  int len;          // Available data to read
  int capacity;     // Allocated memory for |content|. |capacity| >= |len|
  int read_count;
} delay_buffer_t;

// Initialize the service 
int nbb_init_service(int num_channels, const char* name);

// Client tries to connect to a certain service
int nbb_connect_service(const char* client_name, const char* service_name);

// Communicate with the nameserver
int nbb_nameserver_connect(const char* request, char** ret, int* ret_len);

// Open & close channels
int nbb_open_channel(const char* owner, int shm_read_id, int shm_write_id, int is_ipc_create);
int nbb_close_channel(int channel_id);

// Sending a message from client to server
int nbb_send(const char* service_name, const char* msg, size_t msg_len);

// Finds a free channel slot
// Returns the index of the free slot, if it is full, returns -1
int nbb_free_channel_slot();

// Data is available from client, called via interrupt
void nbb_recv_data(int signum);

// Flush stuffs in shm to intermediate buffer to allow finer granularity
void nbb_flush_shm(int slot, char* array_to_flush, int size);

// Read a specified number of bytes from the shm
int nbb_read_bytes(int slot, char* buf, int size);

// Write number of bytes to slot slot_id
int nbb_write_bytes(int slot_id, const char* msg, size_t msg_len);

// Simple utility functions that should be self-explanatory
int nbb_bytes_available(int slot);
int nbb_bytes_read(int slot);
int nbb_bytes_written(int slot);
 
// Insert/read item from the NBB
int nbb_insert_item(int channel_id, const void* ptr_to_item, size_t size);
int nbb_read_item(int channel_id, void** ptr_to_item, size_t* size);

// Called by event dispatcher to make the sockets check the NBB for new events
typedef int(*handle_events_func)(void);
extern volatile handle_events_func handler_func;

int nbb_handle_events();
int nbb_set_handle_events(handle_events_func);

void nbb_print_timestamp(char* str);


#endif // NBB_H

#ifdef __cplusplus
}
#endif
