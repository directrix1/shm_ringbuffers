#include "shm_ringbuffers.h"
#include <fcntl.h> /* For O_* constants */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants, and fstat */
#include <unistd.h>

unsigned int get_aligned_size(unsigned int in_size)
{
    in_size /= 4096;
    return 4096 * (in_size + 1);
}

// ====================
// Subscriber functions
// ====================

/*
 * srb_subscriber_new
 *
 * params:
 *   shm_path - shared memory path
 *
 * returns:
 *   the SRBHandle that references the shared memory ring buffers. This is usually followed up with srb_get_rings call.
 */
SRBHandle srb_subscriber_new(const char* shm_path)
{
    unsigned int head_size = sizeof(struct ShmRingBuffersHead);
    unsigned int rb_size = sizeof(struct ShmRingBufferShared);

    // Create shared memory object
    int shmfd = shm_open(shm_path, O_RDWR, 0);
    if (shmfd <= 0) {
        fprintf(stderr, "Error opening shm object (%s): %d\n", shm_path, shmfd);
        return NULL;
    }
    struct stat shm_stat;
    fstat(shmfd, &shm_stat);
    unsigned int total_size = shm_stat.st_size;
    if (total_size < get_aligned_size(head_size + rb_size)) {
        // Failed sanity check.
        fprintf(stderr, "Failed sanity check opening shared memory!\n");
        close(shmfd);
        return NULL;
    }
    uint8_t* m = (uint8_t*)mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    struct ShmRingBuffersHead* head = (struct ShmRingBuffersHead*)m;
    if (head->state != SRB_RUNNING) {
        fprintf(stderr, "Ring buffer producer not in running state!\n");
        munmap(m, total_size);
        close(shmfd);
        return NULL;
    }

    // Create the memory mapped structure
    SRBHandle handle = malloc(sizeof(struct ShmRingBuffersLocal));
    handle->is_producer = 0;
    handle->shm_fd = shmfd;
    handle->mem_map = m;
    handle->shm_path = strdup(shm_path);
    handle->shm_size = total_size;
    handle->ring_buffers_head = head;
    struct ShmRingBuffer* ringbuffers = handle->ringbuffers = malloc(
        sizeof(struct ShmRingBuffer)
        * head->num_ringbuffers);
    unsigned int rbs_offset = head_size;
    unsigned int descriptions_offset = rbs_offset + head->num_ringbuffers * rb_size;
    unsigned int descriptions_size = 0;

    char* description = (char*)(m + descriptions_offset);
    struct ShmRingBufferShared* rb = (struct ShmRingBufferShared*)(m + rbs_offset);
    for (unsigned int i = 0; i < head->num_ringbuffers; i++) {
        unsigned int description_length = strlen(description) + 1;
        ringbuffers[i].shared = rb;
        ringbuffers[i].description = description;
        if (i < (head->num_ringbuffers - 1)) {
            rb++;
            description += description_length;
            descriptions_size += description_length;
        }
    }

    unsigned int buffers_offset = get_aligned_size(descriptions_offset + descriptions_size);
    uint8_t* buffers = m + buffers_offset;
    for (unsigned int i = 0; i < head->num_ringbuffers; i++) {
        ringbuffers[i].buffers = buffers;
        if (i < (head->num_ringbuffers - 1)) {
            buffers += ringbuffers[i].shared->num_buffers * ringbuffers[i].shared->buffer_size;
        }
    }

    return handle;
}

/*
 * srb_subscriber_get_most_recent_buffer_id
 *
 * params:
 *   ring_buffer - the ring buffer to get the most recent buffer id
 *
 * returns:
 *   the most recent buffer id, that is not currently set as the write_ring_pos, or 0 if no valid buffers exist
 */
unsigned int srb_subscriber_get_most_recent_buffer_id(struct ShmRingBuffer* ring_buffer)
{
    return ring_buffer->shared->write_ring_pos - 1;
}

/*
 * srb_subscriber_get_most_recent_buffer
 *
 * params:
 *   ring_buffer - the ring buffer to get the most recent buffer
 *
 * returns:
 *   the most recent buffer that is not currently set as the write_ring_pos, or NULL if no valid buffers exist
 */
uint8_t* srb_subscriber_get_most_recent_buffer(struct ShmRingBuffer* ring_buffer)
{
    unsigned int b = ring_buffer->shared->write_ring_pos - 1;
    if (b == 0) {
        return NULL;
    }
    b = b % ring_buffer->shared->num_buffers;
    return ring_buffer->buffers + (b * ring_buffer->shared->buffer_size);
}

/*
 * srb_subscriber_get_state
 *
 * params:
 *   ring_buffers_handle - the handle to the ring buffer's shared memory
 *
 * returns:
 *   the run state of the producer
 */
enum EShmRingBuffersState srb_subscriber_get_state(SRBHandle ring_buffers_handle)
{
    return ring_buffers_handle->ring_buffers_head->state;
}

// ==================
// Producer functions
// ==================

/*
 * srb_producer_new
 *
 * params:
 *   shm_path - shared memory path
 *   num_defs - the number of ringbuffers you are defining
 *   ring_buffer_defs - array of {struct ShmRingBuffer}s which will be created in the mmap based on the supplied members:
 *        buffer_size, num_buffers, and description. (these can be freed if wanted after this call)
 *
 * returns:
 *   the SRBHandle that references the shared memory ring buffers. This is usually followed up with srb_get_rings call.
 */
SRBHandle srb_producer_new(const char* shm_path, unsigned int num_defs, struct ShmRingBufferDef* ring_buffer_defs)
{
    // Ascertain sizes of everything
    int head_size = sizeof(struct ShmRingBuffersHead);
    int rb_size = sizeof(struct ShmRingBuffer);
    int descriptions_offset = head_size + rb_size * num_defs;
    int descriptions_size = 0;
    int buffers_size = 0;
    for (unsigned int i = 0; i < num_defs; i++) {
        if (ring_buffer_defs[i].description) {
            descriptions_size += strlen(ring_buffer_defs[i].description);
        }
        descriptions_size++;
        buffers_size += ring_buffer_defs[i].num_buffers * ring_buffer_defs[i].buffer_size;
    }
    int buffers_offset = get_aligned_size(descriptions_offset + descriptions_size);
    int total_size = buffers_offset + buffers_size;

    // Create shared memory object
    int shmfd = shm_open(shm_path, O_CREAT | O_RDWR, S_IRWXU);
    if (shmfd <= 0) {
        fprintf(stderr, "Error creating shm object (%s): %d\n", shm_path, shmfd);
        return NULL;
    }
    if (ftruncate(shmfd, total_size) < 0) {
        fprintf(stderr, "Error truncating shm object (%s) at size: %d\n", shm_path, total_size);
        close(shmfd);
        shm_unlink(shm_path);
    }
    uint8_t* m = (uint8_t*)mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);

    // Create the memory mapped structure
    SRBHandle handle = malloc(sizeof(struct ShmRingBuffersLocal));
    handle->is_producer = 1;
    handle->shm_fd = shmfd;
    handle->mem_map = m;
    handle->shm_path = strdup(shm_path);
    handle->shm_size = total_size;
    struct ShmRingBuffersHead* head = handle->ring_buffers_head = (struct ShmRingBuffersHead*)m;
    head->state = SRB_STOPPED;
    head->num_ringbuffers = num_defs;
    struct ShmRingBuffer* ringbuffers = handle->ringbuffers = malloc(sizeof(struct ShmRingBuffer) * num_defs);

    char* description = (char*)(m + descriptions_offset);
    uint8_t* buffer = m + buffers_offset;
    struct ShmRingBufferShared* ringbuffer = (struct ShmRingBufferShared*)(m + head_size);
    for (unsigned int i = 0; i < num_defs; i++) {
        struct ShmRingBuffer* dest = ringbuffers + i;
        dest->shared = ringbuffer;
        struct ShmRingBufferDef* src = ring_buffer_defs + i;
        dest->shared->num_buffers = src->num_buffers;
        dest->shared->buffer_size = src->buffer_size;
        dest->shared->write_ring_pos = 1;
        dest->description = description;
        if (src->description) {
            strcpy(src->description, description);
        } else {
            description[0] = 0; // zero length string for null src description
        }
        dest->buffers = buffer;

        if (i < (num_defs - 1)) {
            description += strlen(description) + 1;
            buffer += dest->shared->num_buffers * dest->shared->buffer_size;
            ringbuffer++;
        }
    }
    head->state = SRB_RUNNING;

    return handle;
}

/*
 * srb_producer_first_write_buffer
 *
 * params:
 *   ring_buffer - the ring buffer to get the first shared buffer from
 *
 * return:
 *   pointer to the first shared buffer
 */
uint8_t* srb_producer_first_write_buffer(struct ShmRingBuffer* ring_buffer)
{
    unsigned int b = ring_buffer->shared->write_ring_pos % ring_buffer->shared->num_buffers;
    return ring_buffer->buffers + (b * ring_buffer->shared->buffer_size);
}

/*
 * srb_producer_next_write_buffer
 *   this function returns the next shared write buffer.
 *
 * params:
 *   ring_buffer - the ring buffer to get the next shared buffer from
 *
 * return:
 *   pointer to the next shared buffer
 */
uint8_t* srb_producer_next_write_buffer(struct ShmRingBuffer* ring_buffer)
{
    unsigned int b = ++(ring_buffer->shared->write_ring_pos) % ring_buffer->shared->num_buffers;
    return ring_buffer->buffers + (b * ring_buffer->shared->buffer_size);
}

/*
 * srb_producer_signal_stopping
 *   sets shared state to SRB_STOPPING
 *
 * params:
 *   ring_buffers_handle - the handle to the ring buffer's shared memory'
 *
 */
void srb_producer_signal_stopping(SRBHandle ring_buffers_handle)
{
    SRBHandle handle = ring_buffers_handle;
    if (handle->is_producer) {
        handle->ring_buffers_head->state = SRB_STOPPING;
    }
}

// =================================================
// Common functions to producer and subscriber sides
// =================================================

/*
 * srb_get_rings
 *
 * params:
 *   ring_buffers_handle - the handle to the ring buffer's shared memory
 *   ring_buffers - will be set to the memory mapped ring_buffers
 *
 * returns:
 *   the number of ring_buffers
 */
unsigned int srb_get_rings(SRBHandle ring_buffers_handle, struct ShmRingBuffer** ring_buffers)
{
    *ring_buffers = ring_buffers_handle->ringbuffers;
    return ring_buffers_handle->ring_buffers_head->num_ringbuffers;
}

/*
 * srb_close
 *   unmaps all ring buffers and closes the shared memory, if producer first signals SRB_STOPPED
 *
 * params:
 *   ring_buffers_handle - the handle to the ring buffer's shared memory
 */
void srb_close(SRBHandle ring_buffers_handle)
{
    SRBHandle handle = ring_buffers_handle;
    if (handle->is_producer) {
        handle->ring_buffers_head->state = SRB_STOPPED;
    }
    munmap((void*)handle->mem_map, handle->shm_size);
    close(handle->shm_fd);
    if (handle->is_producer) {
        shm_unlink(handle->shm_path);
    }
    free((void*)(handle->ringbuffers));
    free((void*)(handle->shm_path));
    free(handle);
}
