#include "nbb.h"
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>

// list of channel pointers (to shared memory)
struct channel channel_list[SERVICE_MAX_CHANNELS] = {};
struct connected_node connected_nodes[SERVICE_MAX_CHANNELS] = {};
delay_buffer_t delay_buffers[SERVICE_MAX_CHANNELS];

sem_t *sem_id;   // POSIX semaphore

#define PID_MAX_STRLEN 5 // Assume maximum pid value of 16-bit
#define CHANNEL_MAX_STRLEN 5

// When a client nbb_connect_service()s to a service, this message is
// sent to the service to note the new incoming connection.
#define NEW_CONN_NOTIFY_MSG "**Q_Q**"
#define NEW_CONN_NOTIFY_MSG_LEN (sizeof(NEW_CONN_NOTIFY_MSG) - 1)

// Re-entrant version of nbb_read_item for use only in signal handler
// 16KB should be enough for 1 read at a time in the signal handler
#define signal_handler_buffer_size (1<<14)
static char signal_handler_buffer[signal_handler_buffer_size];
static int nbb_read_item_r(int channel_id, void* item, size_t* size, size_t max_size);


int nbb_nameserver_connect(const char* request, char** ret, int* ret_len)
{
  int nameserver_pid = 0;
  FILE* pFile;
  int retval;
  char* recv;
  size_t recv_len;

  // Sanity check to isolate errors faster
  assert(request != NULL && ret != NULL && ret_len != NULL);
  *ret = NULL;
  *ret_len = 0;

  // Should be reversed since what's written by service is read by nameserver
  if(nbb_open_channel(NULL, NAMESERVER_WRITE, NAMESERVER_READ, !IPC_CREAT)) {
    return -1;
  }

  pFile = fopen(NAMESERVER_PID_FILE, "r+");
  fscanf(pFile,"%d",&nameserver_pid);
  fclose(pFile);

  nbb_insert_item(NAMESERVER_SLOT, request, strlen(request));
  kill(nameserver_pid, NBB_SIGNAL);

  // Poll until we get something
  do{
    retval = nbb_read_item(NAMESERVER_SLOT, (void**)&recv, &recv_len);
  } while (retval == BUFFER_EMPTY || retval == BUFFER_EMPTY_PRODUCER_INSERTING);

  // Set return values
  *ret = recv;
  *ret_len = recv_len;

  // No errors, we're happy
  return 0;
}

int init_nameserver()
{
  FILE* pFile;

  // XXX: Initial semaphore value? Use 1 for now...
  sem_id = sem_open(SEM_KEY, O_CREAT, 0666, 1);
  if(sem_id == SEM_FAILED) {
    PRINTF("! Unable to obtain semaphore\n");
    return -1;
  }

  if(nbb_open_channel(NULL, NAMESERVER_READ, NAMESERVER_WRITE, IPC_CREAT)) {
    PRINTF("! Unable to open channel\n");
    return -1;
  }

  pFile = fopen(NAMESERVER_PID_FILE,"w+");
  fprintf(pFile,"%d",(int)getpid());
  fclose(pFile);

  return 0;
}

int nbb_init_service(int num_channels, const char* name)
{
  char request[MAX_MSG_LEN] = {};
  char num_channel[CHANNEL_MAX_STRLEN];
  char pid[PID_MAX_STRLEN + 1];

  assert(num_channels > 0 && name != NULL);

  sem_id = sem_open(SEM_KEY, 0);
  if(sem_id == SEM_FAILED) {
    PRINTF("! nbb_init_service(): Unable to obtain semaphore\n");
    return -1;
  }

  // BEGIN CRITICAL SECTION
  //TODO: sem_wait(sem_id);

  sprintf(num_channel, "%d", num_channels);
  sprintf(pid, "%d", getpid());

  strcpy(request, NBB_SERVICE);
  strcat(request, " ");
  strcat(request, name);
  strcat(request, " ");
  strcat(request, num_channel);
  strcat(request, " ");
  strcat(request, pid);

  PRINTF("request: %s, len: %zu\n", request, strlen(request));

  char* recv;
  int recv_len;

  if(nbb_nameserver_connect(request, &recv, &recv_len)) {
    PRINTF("! nbb_init_service(): Could not connect to nameserver\n");
    sem_post(sem_id);
    return -1;
  }

  if(!strcmp(recv, NAMESERVER_CHANNEL_FULL)) {
    PRINTF("! nbb_init_service(): Reserving channel unsuccessful\n");
  
    sem_post(sem_id);
    return -1;
  }
  else {
    PRINTF("** Acquired the following channels: %.*s\n", recv_len, recv);

    int i;
    int channel;
    char* tmp;

    tmp = strtok(recv, " ");
    for(i = 1;i <= num_channels;i++) {
      channel = atoi(tmp);
      if(nbb_open_channel(name, channel, channel + READ_WRITE_CONV, IPC_CREAT) == -1) {
        PRINTF("! nbb_init_service(): Failed to open the %d-th channel\n", i);
        sem_post(sem_id);
        free(recv);
        return -1;
      }
      tmp = strtok(NULL, " ");
    }

    signal(NBB_SIGNAL, nbb_recv_data);

    sem_post(sem_id);
    free(recv);
    return 0;
  }
  // END CRITICAL SECTION
}

// Called by clients connecting to a server
// Needs to map shm buffers into client's address space
int nbb_connect_service(const char* client_name, const char* service_name)
{
  char request[MAX_MSG_LEN];
  int ret_code;

  assert(service_name != NULL);

  sem_id = sem_open(SEM_KEY, 0);
  if(sem_id == SEM_FAILED) {
    PRINTF("! nbb_connect_service(): Unable to obtain semaphore\n");
    return -1;
  }
  // BEGIN CRITICAL SECTION
  sem_wait(sem_id);

  strcpy(request, NBB_CLIENT);
  strcat(request, " ");
  strcat(request, service_name);

  char* recv;
  int recv_len;
  if(nbb_nameserver_connect(request, &recv, &recv_len)) {
    PRINTF("! nbb_connect_service(): Could not connect to nameserver!\n");
    return -1;
  }

  if(!recv) {
    ret_code = -1;
  }

  else if(!strcmp(recv, UNKNOWN_SERVICE)) {
    PRINTF("! nbb_connect_service(): Invalid service: %s\n", service_name);
    ret_code = -1;
  }

  else if(!strcmp(recv, SERVICE_BUSY)) {
    PRINTF("! nbb_connect_service(): Service %s too busy, not enough channel\n", service_name); 
    ret_code = -1;
  }

  else {
    char* tmp;
    int slot;
    int channel_id;
    int service_pid;
    char msg[MAX_MSG_LEN];
    char pid[PID_MAX_STRLEN + 1];

    tmp = strtok(recv, " ");
    channel_id = atoi(tmp);
    tmp = strtok(NULL, " ");
    service_pid = atoi(tmp);

    slot = nbb_open_channel(client_name, channel_id + READ_WRITE_CONV, channel_id, !IPC_CREAT);

    //connected_nodes[slot].name = (char*)malloc(sizeof(char)*MAX_MSG_LEN);
    assert(strlen(service_name) + 1 <= MAX_NAME_SIZE);
    strcpy(connected_nodes[slot].name, service_name);
    connected_nodes[slot].pid = service_pid;

    ret_code = slot;

    sprintf(pid, "%d", getpid());
    strcpy(msg, NEW_CONN_NOTIFY_MSG) ;
    strcat(msg, " ");
    strcat(msg, pid);
    strcat(msg, " ");
    strcat(msg, client_name);

    // Have to set up the signal handler before we connect to the service
    // Ran into this condition under heavy load
    signal(NBB_SIGNAL, nbb_recv_data);
    // Notify service of the new connection
    if (nbb_send(service_name, msg, strlen(msg))) {
      PRINTF("! nbb_connect_service(): Can't notify service '%s' of new connection\n", service_name);
      ret_code = -1;
    } else {
      PRINTF("** Connecting to service successful, channel: %d service pid: %d\n", channel_id, service_pid);
    }
  }

  // END CRITICAL SECTION
  sem_post(sem_id);

	return ret_code;
}

void nbb_set_cb_new_connection(const char* owner, cb_new_conn_func func, void* arg)
{
  int i;

  // |arg| can be NULL
  assert(owner != NULL && func != NULL);

  for(i = 1;i < SERVICE_MAX_CHANNELS;i++) {
    if(!channel_list[i].in_use) {
      continue;
    }

    if(channel_list[i].owner && !strcmp(owner, channel_list[i].owner)) {
      channel_list[i].new_conn = func;
      channel_list[i].arg = arg;
    }
  }
}

void nbb_set_cb_new_data(const char* owner, cb_new_data_func func)
{
  int i;

  assert(owner != NULL && func != NULL);

  for(i = 1;i < SERVICE_MAX_CHANNELS;i++) {
    if(!channel_list[i].in_use) {
      continue;
    }

    if(channel_list[i].owner && !strcmp(owner, channel_list[i].owner)) {
      channel_list[i].new_data = func;
    }
  }
}

void nbb_set_owner(int slot_id, const char *owner)
{
  assert(slot_id >= 0 && slot_id < SERVICE_MAX_CHANNELS && "Invalid slot id");
  assert(owner != NULL && "Invalid owner");

  if (channel_list[slot_id].owner != NULL) {
    free(channel_list[slot_id].owner);
  }

  channel_list[slot_id].owner = (char *) malloc(sizeof(char) * (strlen(owner) + 1));
  assert(channel_list[slot_id].owner != NULL && "malloc failed");

  strcpy(channel_list[slot_id].owner, owner);
  PRINTF("***nbb_change_owner***: Changed owner for slot %d to '%s'\n", slot_id, owner);
}

int nbb_write_bytes(int slot_id, const char* msg, size_t msg_len)
{
  assert(msg != NULL);

  if (msg_len == 0) {
    PRINTF("! nbb_send(): nothing to send (0 length passed in)\n");
    return 0;
  }

  assert(slot_id >= 0 && slot_id < SERVICE_MAX_CHANNELS && "Process not found");

  nbb_insert_item(slot_id, msg, msg_len);
  kill(connected_nodes[slot_id].pid, NBB_SIGNAL);

  return 0;
}

int nbb_send(const char* destination, const char* msg, size_t msg_len)
{
  int i;

  // Since i = 0 is already reserved for nameserver
  for(i = 1; i < SERVICE_MAX_CHANNELS;i++) {
    if(channel_list[i].in_use && connected_nodes[i].name &&
       !strcmp(destination, connected_nodes[i].name)) {
      break;
    }
  }

  return nbb_write_bytes(i, msg, msg_len);
}

/* Called when the service gets new client data */
void nbb_recv_data(int signum)
{
  int i;
  char* recv = signal_handler_buffer;
  size_t recv_max_len = signal_handler_buffer_size;
  size_t recv_len = 0;
  int retval = -1;
  int is_new_conn_msg = 0;

  // Attempt to debug Qt. XXX: Remove when done.
  PRINTF("***NBB***: Inside signal handler\n");

  // Since i = 0 is already reserved for nameserver
  for(i = 1;channel_list[i].in_use && i < SERVICE_MAX_CHANNELS;i++) {
    // Use re-entrant version of nbb_read_item
    retval = nbb_read_item_r(i, (void*) recv, &recv_len, recv_max_len);

    if(retval == OK) {
      if (memcmp(recv, NEW_CONN_NOTIFY_MSG, NEW_CONN_NOTIFY_MSG_LEN) == 0) {
        // We have enough buffer for this null byte
        recv[recv_len] = '\0';

        char* tmp = NULL;

        strtok(recv, " ");
        tmp = strtok(NULL, " ");
        connected_nodes[i].pid = atoi(tmp);
        tmp = strtok(NULL, " ");
        //connected_nodes[i].name = (char*)malloc(strlen(tmp) * sizeof(char));
        assert(strlen(tmp) + 1 <= MAX_NAME_SIZE);
        strcpy(connected_nodes[i].name, tmp);

        PRINTF("***NBB***: New connection on slot %d from client_name: %s with pid: %d\n", i, connected_nodes[i].name, connected_nodes[i].pid);

        is_new_conn_msg = 1;
      }

      if (!is_new_conn_msg) {
        nbb_flush_shm(i, recv, recv_len);
      }

      /*
      PRINTF("** Received %zu bytes: ", recv_len);
      int z;
      for(z=0; z<recv_len; z++) {
        PRINTF("%02x", recv[z]);
      }
      PRINTF(" from shm id %d slot %d\n", (int) channel_list[i].read_id, i);
      */

      // Notify of new connection on slot i
      if (is_new_conn_msg && channel_list[i].new_conn != NULL) {
        channel_list[i].new_conn(i, channel_list[i].arg);
      }

      // We only have new data when the msg is "real data" (not our initial msg).
      // Notify event of new available data on slot i
      if (!is_new_conn_msg && channel_list[i].new_data != NULL) {
        channel_list[i].new_data(i);
      }

      recv_len = 0;
    }

  }

  signal(NBB_SIGNAL, nbb_recv_data);
}

int nbb_open_channel(const char* owner, int shm_read_id, int shm_write_id, int is_ipc_create)
{
	int shmid;
	unsigned char * shm;
  int free_slot;

  if(shm_read_id == NAMESERVER_WRITE && shm_write_id == NAMESERVER_READ) {
    free_slot = 0;
  }
  else {
    free_slot = nbb_free_channel_slot();
  }

  if(free_slot == -1) {
	  PRINTF("! nbb_open_channel(): no free_slot\n");
    return -1;
  }

	// Allocate 4 pages, 1 meta + 1 data for each buffer
	// Read buffer
	// note that we use SERVICE_TEST_WRITE, not READ, since the service's
	// read is the client's write
	if((shmid = shmget(shm_read_id, PAGE_SIZE*2, is_ipc_create | 0666)) < 0) {
		PRINTF("shmget");
		return -1;
	}
	if((shm = (unsigned char *) shmat(shmid, NULL, 0)) == (unsigned char*) -1) {
		PRINTF("shmat");
		return -1;
	}
	// Make sure the memory is zero'd out
	memset(shm, 0, PAGE_SIZE*2);

	channel_list[free_slot].read = (struct buffer*) shm;
	channel_list[free_slot].read->data_size = PAGE_SIZE;
	channel_list[free_slot].read->data_offset = PAGE_SIZE;
	channel_list[free_slot].read_data = (unsigned char*) shm+PAGE_SIZE;
  channel_list[free_slot].read_id = shm_read_id;
  channel_list[free_slot].read_count = 0;

	// Write buffer. Same note as above about swapping read/write
	shmid = -1;
	shm = (unsigned char*) -1;
	if((shmid = shmget(shm_write_id, PAGE_SIZE*2, is_ipc_create | 0666)) < 0) {
		PRINTF("shmget");
		return -1;
	}
	if((shm = (unsigned char *) shmat(shmid, NULL, 0)) == (unsigned char*) -1) {
		PRINTF("shmat");
		return -1;
	}
	// Make sure the memory is zero'd out
	memset(shm, 0, PAGE_SIZE*2);

	channel_list[free_slot].write = (struct buffer*) (shm);
	channel_list[free_slot].write->data_size = PAGE_SIZE;
	channel_list[free_slot].write->data_offset = PAGE_SIZE;
	channel_list[free_slot].write_data = (unsigned char*) shm+PAGE_SIZE;
  channel_list[free_slot].write_id = shm_write_id;
  channel_list[free_slot].write_count = 0;

  channel_list[free_slot].in_use = 1;

  if(owner) {
    channel_list[free_slot].owner = (char*) malloc(strlen(owner) + 1);
    strcpy(channel_list[free_slot].owner, owner);
  }

  memset(&delay_buffers[free_slot], 0, sizeof(struct delay_buffer));
  delay_buffers[free_slot].content = (char*) calloc(1<<20, sizeof(char));
  assert(delay_buffers[free_slot].content);
  delay_buffers[free_slot].capacity = 1<<20;

  return free_slot;
}

//TODO: Most probably buggy
int nbb_close_channel(int index)
{
  assert(index >= 0 && index < SERVICE_MAX_CHANNELS);

  shmdt((char*)channel_list[index].read);
  if(shmctl(channel_list[index].read_id, IPC_RMID, 0) == -1) {
    return -1;
  }

  shmdt((char*)channel_list[index].write);
  if(shmctl(channel_list[index].write_id, IPC_RMID, 0) == -1) {
    return -1;
  }

  channel_list[index].in_use = 0;
  return 0;
}

int nbb_free_channel_slot()
{
  int i;

  for(i = 0;i < SERVICE_MAX_CHANNELS;i++) {
    if(!channel_list[i].in_use) {
      return i;
    }
  }

  return -1;
}

/* Reads as many bytes up to size as are available
 * Return value is the number of bytes read.
 */
int nbb_read_bytes(int slot, char* buf, int size)
{
  assert(slot >= 0 && buf != NULL && size >= 0);

  delay_buffer_t* delay_buffer = &(delay_buffers[slot]);
  assert(delay_buffer->capacity >= delay_buffer->len);

  // Attempt to read 0 bytes or buffer has nothing to read
  if (size == 0 || delay_buffer->content == NULL || delay_buffer->len == 0) {
    return 0;
  }

  assert(delay_buffer->content != NULL && delay_buffer->len > 0);

  // Read minimum of the requested length and available data
  if(size > delay_buffer->len) {
    size = delay_buffer->len;
  }

  // Read |size| bytes into |buf| and update statistics
  memcpy(buf, delay_buffer->content, size);
  channel_list[slot].read_count += size;

  // Move remaining data (if any) into the front of buffer
  int new_len = delay_buffer->len - size;
  if (new_len > 0) {
    memmove(delay_buffer->content, delay_buffer->content + size, new_len);
  }
  delay_buffer->len = new_len;

  return size;
}

int nbb_bytes_available(int slot)
{
  assert(slot >= 0 && slot < SERVICE_MAX_CHANNELS);
  return delay_buffers[slot].len;
}

int nbb_bytes_read(int slot)
{
  assert(slot >= 0 && slot < SERVICE_MAX_CHANNELS);
  return channel_list[slot].read_count;
}

int nbb_bytes_written(int slot)
{
  assert(slot >= 0 && slot < SERVICE_MAX_CHANNELS);
  return channel_list[slot].write_count;
}

void nbb_flush_shm(int slot, char* array_to_flush, int size)
{
  assert(slot >= 0 && slot < SERVICE_MAX_CHANNELS);
  assert(array_to_flush != NULL && size >= 0);

  if (size == 0)
    return;

  delay_buffer_t* buffer = &(delay_buffers[slot]);
  int new_size = buffer->len + size;

  // Grow the buffer if exceeding current capacity
#if 0
  if (new_size > buffer->capacity) {
    // Initial capacity (2 * MAX_MSG_LEN)
    if (buffer->capacity == 0) {
      buffer->capacity = MAX_MSG_LEN;
    }
    int new_buffer_capacity = 2 * buffer->capacity < new_size ?
        new_size :
        2 * buffer->capacity;

    buffer->content = (char *) realloc(buffer->content, new_buffer_capacity);
    buffer->capacity = new_buffer_capacity;
  }

#endif

  // Append new data to the end (or beginning if it's the first flush)
  memcpy(buffer->content + buffer->len, array_to_flush, size);
  buffer->len = new_size;
}

int nbb_insert_item(int channel_id, const void* ptr_to_item, size_t size)
{
  assert(channel_id >= 0 && channel_id < SERVICE_MAX_CHANNELS);
  assert(ptr_to_item != NULL && size >= 0);

	struct buffer *buf = channel_list[channel_id].write;
	unsigned char *data_buf = channel_list[channel_id].write_data;

  unsigned short temp_ac = buf->ack_counter;

  assert(channel_id >= 0 && channel_id < SERVICE_MAX_CHANNELS);
  assert(ptr_to_item != NULL && size >= 0);

  if (buf->last_update_counter - temp_ac == 2 * BUFFER_SIZE) {
    return BUFFER_FULL;
  }

  if (buf->last_update_counter - temp_ac == (2 * BUFFER_SIZE) - 1) {
    return BUFFER_FULL_CONSUMER_READING;
  }

  // Check if there is space in the data region for new item
  // This works by checking how far the previous item extends,
  // and then if our size will fit in the remaining space
  //
  // If it doesn't fit at the end, check at the head of the list too.
  struct channel_item* prev_item =
				&(buf->items[(((buf->last_update_counter/2)-1)%BUFFER_SIZE)]);

	int item_offset;

	if(buf->last_update_counter == 0) {
		item_offset = 0;
	}
	else if((prev_item->offset+prev_item->size+size) <  buf->data_size) {
		item_offset = prev_item->offset + prev_item->size;
	}
	// Check if there's space at the head of the list for our item instead
	// This is done by checking the offset of the
	// oldest unread item (at the ack counter)
	else if(buf->items[((buf->last_ack_counter)/2)%BUFFER_SIZE].offset > size) {
		item_offset = 0;
	}
	// Couldn't fit at the end or the beginning. Sad.
	else {
		PRINTF("else...\n");
		PRINTF("poff: %d psize: %d size: %zu bsize: %d\n", prev_item->offset,
					prev_item->size, size, buf->data_size);
		return BUFFER_FULL;
	}

	// Update our new item in items[], say that we're writing
  buf->update_counter = buf->last_update_counter + 1;

	// Copy the item into the buffer's shm data region at offset
	memcpy(data_buf+item_offset, ptr_to_item, size);

	// Set the offset based on our above calculations
	buf->items[((buf->last_update_counter/2)%BUFFER_SIZE)].offset = item_offset;
	buf->items[((buf->last_update_counter/2)%BUFFER_SIZE)].size = size;

  // Done writing
  buf->update_counter = buf->last_update_counter + 2;

  buf->last_update_counter = buf->update_counter;

  if(memcmp(NEW_CONN_NOTIFY_MSG, ptr_to_item, sizeof(NEW_CONN_NOTIFY_MSG))) {
    channel_list[channel_id].write_count += (size - 1); // Excluding '\0'
  }

  return OK;
}

int nbb_read_item(int channel_id, void** ptr_to_item, size_t* size)
{
	struct buffer *buf = channel_list[channel_id].read;
	unsigned char *data_buf = channel_list[channel_id].read_data;
  unsigned short temp_uc = buf->update_counter;

  assert(channel_id >= 0 && channel_id < SERVICE_MAX_CHANNELS);
  assert(ptr_to_item != NULL && size != NULL);

  *ptr_to_item = NULL;
  *size = 0;

  if(channel_id < 0) {
    PRINTF("! nbb_read_item(): invalid channel_id %d\n", channel_id);
    return -1;
  }

  if(size < 0) {
    PRINTF("! nbb_read_item(): invalid size %lu\n", *size);
    return -1;
  }

  if (temp_uc == buf->last_ack_counter) {
    return BUFFER_EMPTY;
  }

  if ((temp_uc - buf->last_ack_counter) == 1) {
    return BUFFER_EMPTY_PRODUCER_INSERTING;
  }

  buf->ack_counter = buf->last_ack_counter + 1;

  // Copy out the value to malloc'd mem in our address space
  struct channel_item* tmp =
				&(buf->items[((buf->last_ack_counter / 2) % BUFFER_SIZE)]);
  *ptr_to_item = malloc(tmp->size);
  memcpy(*ptr_to_item, data_buf+tmp->offset, tmp->size);
	*size = tmp->size;

  buf->ack_counter = buf->last_ack_counter + 2;
  buf->last_ack_counter = buf->ack_counter;

  return OK;
}

// nbb_read_item_r is the re-entrant version of nbb_read_item.
// This version has no malloc() and uses global data for ptr_to_item;
// therefore, it must be used only in the signal handler
static int nbb_read_item_r(int channel_id, void* item, size_t* size, size_t max_size)
{
	struct buffer *buf = channel_list[channel_id].read;
	unsigned char *data_buf = channel_list[channel_id].read_data;
  unsigned short temp_uc = buf->update_counter;

  assert(channel_id >= 0 && channel_id < SERVICE_MAX_CHANNELS);
  assert(item != NULL && size != NULL && max_size > 0);

  *size = 0;

  if(channel_id < 0) {
    printf("! nbb_read_item(): invalid channel_id %d\n", channel_id);
    return -1;
  }

  if(size < 0) {
    printf("! nbb_read_item(): invalid size %lu\n", *size);
    return -1;
  }

  if (temp_uc == buf->last_ack_counter) {
    return BUFFER_EMPTY; 
  }

  if ((temp_uc - buf->last_ack_counter) == 1) {
    return BUFFER_EMPTY_PRODUCER_INSERTING;
  }

  buf->ack_counter = buf->last_ack_counter + 1;

  // Copy out the value to malloc'd mem in our address space
  struct channel_item* tmp = 
				&(buf->items[((buf->last_ack_counter / 2) % BUFFER_SIZE)]);

  // Copy to this item
  // Make sure we have enough buffer size for the message and null byte
  assert(tmp->size < max_size);
  memcpy(item, data_buf+tmp->offset, tmp->size);
	*size = tmp->size;

  buf->ack_counter = buf->last_ack_counter + 2;
  buf->last_ack_counter = buf->ack_counter;

  return OK;
}

volatile handle_events_func handler_func;

int nbb_set_handle_events(handle_events_func newfunc) {
    handler_func = newfunc;
    PRINTF("==== nbb_set_handle_events %p %p\n", &handler_func, handler_func);

    return 0;
}

int nbb_handle_events() {
    if(handler_func != NULL) {
        PRINTF("==== nbb_handle_events calling %p %p\n", &handler_func, handler_func);
        return handler_func();
    } else {
        PRINTF("==== nbb_handle_events called, but no function set!\n");
    }
    return 0;
}

void nbb_print_timestamp(char* str) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned long long nsec = ts.tv_nsec + (1000000000*ts.tv_sec);
    fprintf(stderr, "Timestamp from %s: %lld", str, nsec);
}
