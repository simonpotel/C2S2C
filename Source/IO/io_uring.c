/* io_uring.c - Linux backend built on io_uring (readiness through IORING_OP_POLL_ADD).
 * SPDX-License-Identifier: MIT */
#if defined(__linux__)
#include <errno.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "io.h"

/*
 * Io_uring lets the program and the kernel exchange work through two shared
 * queues, so many operations cost a single system call. Here it is used the same way as epoll:
 * we ask the kernel "tell me when this socket is readable/writable" (a POLL_ADD request) and
 * read the answers from the completion queue. A poll request answers once, so after each answer
 * we ask again; that gives the same "level-triggered" behaviour as the other backends (a socket
 * that is still ready is reported again).
 *
 * Each registered socket has a slot with a generation number. The request carries
 * (slot, generation); answers for an older generation (after a change or removal) are ignored.
 * The kernel must be Linux 5.11+ (timeouts on waits); otherwise opening the backend fails.
 */
#define NO_REPLY (~0ull) /* user_data of requests whose answer we do not care about */
#define RING_ENTRIES 4096u
#define MAX_FD_TABLE (1u << 20)

typedef struct reg {
    void* tag;
    int fd;
    uint8_t events;
    uint8_t active;
    uint32_t gen;
} reg;

struct dx_io {
    int ring_fd;
    uint32_t sq_entries;
    void *sq_ptr, *cq_ptr;
    size_t sq_size, cq_size, sqes_size;
    struct io_uring_sqe* sqes;
    uint32_t *sq_head, *sq_tail, *sq_mask, *sq_array;
    uint32_t *cq_head, *cq_tail, *cq_mask;
    struct io_uring_cqe* cqes;
    uint32_t pending; /* requests written but not yet given to the kernel */

    reg* regs;            /* one per registered socket */
    uint32_t* free_slots; /* stack of unused slot numbers */
    uint32_t free_count, nregs;
    int32_t* slot_of_fd; /* fd -> slot, or -1 */
    uint32_t fd_cap;
};

static long uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags,
                        void* arg, size_t argsz)
{
    return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, argsz);
}

static void release_all(dx_io* io, const dx_alloc* a)
{
    if (io->sqes && io->sqes != MAP_FAILED) munmap(io->sqes, io->sqes_size);
    if (io->cq_ptr && io->cq_ptr != io->sq_ptr) munmap(io->cq_ptr, io->cq_size);
    if (io->sq_ptr && io->sq_ptr != MAP_FAILED) munmap(io->sq_ptr, io->sq_size);
    if (io->ring_fd >= 0) close(io->ring_fd);
    a->release(io->regs, a->user);
    a->release(io->free_slots, a->user);
    a->release(io->slot_of_fd, a->user);
    a->release(io, a->user);
}

static dx_io* uring_open(const dx_alloc* a, uint32_t max_fds)
{
    dx_io* io = a->alloc(sizeof *io, a->user);
    if (!io) return NULL;
    memset(io, 0, sizeof *io);
    io->ring_fd = -1;

    struct rlimit rl;
    io->fd_cap = getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < MAX_FD_TABLE
                     ? (uint32_t)rl.rlim_cur
                     : MAX_FD_TABLE;
    io->nregs = max_fds;
    io->regs = a->alloc(sizeof(reg) * max_fds, a->user);
    io->free_slots = a->alloc(sizeof(uint32_t) * max_fds, a->user);
    io->slot_of_fd = a->alloc(sizeof(int32_t) * io->fd_cap, a->user);
    if (!io->regs || !io->free_slots || !io->slot_of_fd) goto fail;
    memset(io->regs, 0, sizeof(reg) * max_fds);
    memset(io->slot_of_fd, 0xFF, sizeof(int32_t) * io->fd_cap); /* all -1 */
    for (uint32_t i = 0; i < max_fds; i++)
        io->free_slots[i] = max_fds - 1 - i;
    io->free_count = max_fds;

    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    io->ring_fd = (int)syscall(__NR_io_uring_setup, RING_ENTRIES, &p);
    if (io->ring_fd < 0 || !(p.features & IORING_FEAT_EXT_ARG))
        goto fail; /* kernel too old or io_uring blocked */

    io->sq_entries = p.sq_entries;
    io->sq_size = p.sq_off.array + p.sq_entries * sizeof(uint32_t);
    io->cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    int single = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
    if (single && io->cq_size > io->sq_size) io->sq_size = io->cq_size;
    io->sq_ptr = mmap(NULL, io->sq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                      io->ring_fd, IORING_OFF_SQ_RING);
    if (io->sq_ptr == MAP_FAILED) goto fail;
    io->cq_ptr = io->sq_ptr;
    if (!single) {
        io->cq_ptr = mmap(NULL, io->cq_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          io->ring_fd, IORING_OFF_CQ_RING);
        if (io->cq_ptr == MAP_FAILED) goto fail;
    }
    io->sqes_size = p.sq_entries * sizeof(struct io_uring_sqe);
    io->sqes = mmap(NULL, io->sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                    io->ring_fd, IORING_OFF_SQES);
    if (io->sqes == MAP_FAILED) goto fail;

    uint8_t* sq = io->sq_ptr;
    uint8_t* cq = io->cq_ptr;
    io->sq_head = (uint32_t*)(void*)(sq + p.sq_off.head);
    io->sq_tail = (uint32_t*)(void*)(sq + p.sq_off.tail);
    io->sq_mask = (uint32_t*)(void*)(sq + p.sq_off.ring_mask);
    io->sq_array = (uint32_t*)(void*)(sq + p.sq_off.array);
    io->cq_head = (uint32_t*)(void*)(cq + p.cq_off.head);
    io->cq_tail = (uint32_t*)(void*)(cq + p.cq_off.tail);
    io->cq_mask = (uint32_t*)(void*)(cq + p.cq_off.ring_mask);
    io->cqes = (struct io_uring_cqe*)(void*)(cq + p.cq_off.cqes);
    return io;

fail:
    release_all(io, a);
    return NULL;
}

static void uring_close(dx_io* io, const dx_alloc* a)
{
    release_all(io, a);
}

/* Gives the queued requests to the kernel. */
static void submit(dx_io* io)
{
    if (io->pending) {
        (void)uring_enter(io->ring_fd, io->pending, 0, 0, NULL, 0);
        io->pending = 0;
    }
}

/* Next free request slot in the submission queue (submitting first if the queue is full). */
static struct io_uring_sqe* next_sqe(dx_io* io)
{
    uint32_t tail = *io->sq_tail;
    if (tail - __atomic_load_n(io->sq_head, __ATOMIC_ACQUIRE) >= io->sq_entries) submit(io);
    uint32_t index = tail & *io->sq_mask;
    struct io_uring_sqe* sqe = &io->sqes[index];
    memset(sqe, 0, sizeof *sqe);
    io->sq_array[index] = index;
    __atomic_store_n(io->sq_tail, tail + 1, __ATOMIC_RELEASE);
    io->pending++;
    return sqe;
}

static uint64_t user_data(uint32_t slot, uint32_t gen)
{
    return ((uint64_t)slot << 32) | gen;
}

/* Asks the kernel to report readiness of one registered socket (once). */
static void arm(dx_io* io, uint32_t slot)
{
    reg* r = &io->regs[slot];
    struct io_uring_sqe* sqe = next_sqe(io);
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = r->fd;
    /* No events requested still reports hang-up and errors, like poll(2) does. */
    sqe->poll_events =
        (uint16_t)(((r->events & DX_IO_R) ? POLLIN : 0) | ((r->events & DX_IO_W) ? POLLOUT : 0));
    sqe->user_data = user_data(slot, r->gen);
}

static void cancel(dx_io* io, uint32_t slot)
{
    struct io_uring_sqe* sqe = next_sqe(io);
    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->addr = user_data(slot, io->regs[slot].gen);
    sqe->user_data = NO_REPLY;
}

static int uring_add(dx_io* io, int fd, void* tag, uint8_t events)
{
    if (fd < 0 || (uint32_t)fd >= io->fd_cap || io->slot_of_fd[fd] >= 0 || io->free_count == 0)
        return DX_ERR_LIMIT;
    uint32_t slot = io->free_slots[--io->free_count];
    reg* r = &io->regs[slot];
    r->tag = tag;
    r->fd = fd;
    r->events = events;
    r->active = 1;
    r->gen++;
    io->slot_of_fd[fd] = (int32_t)slot;
    arm(io, slot);
    return DX_OK;
}

static int uring_mod(dx_io* io, int fd, void* tag, uint8_t events)
{
    if (fd < 0 || (uint32_t)fd >= io->fd_cap || io->slot_of_fd[fd] < 0) return DX_ERR_INVALID_ARG;
    uint32_t slot = (uint32_t)io->slot_of_fd[fd];
    reg* r = &io->regs[slot];
    cancel(io, slot); /* drop the outstanding request, then ask again with the new interest */
    r->gen++;
    r->tag = tag;
    r->events = events;
    arm(io, slot);
    return DX_OK;
}

static int uring_del(dx_io* io, int fd)
{
    if (fd < 0 || (uint32_t)fd >= io->fd_cap || io->slot_of_fd[fd] < 0) return DX_ERR_INVALID_ARG;
    uint32_t slot = (uint32_t)io->slot_of_fd[fd];
    cancel(io, slot);
    io->regs[slot].active = 0;
    io->slot_of_fd[fd] = -1;
    io->free_slots[io->free_count++] = slot;
    return DX_OK;
}

static int uring_wait(dx_io* io, dx_io_event* out, int max, int timeout_ms)
{
    /* Give pending requests to the kernel and, if nothing is complete yet, wait for one. */
    int have = __atomic_load_n(io->cq_tail, __ATOMIC_ACQUIRE) != *io->cq_head;
    unsigned min_complete = (have || timeout_ms == 0) ? 0u : 1u;
    if (timeout_ms > 0 && !have) {
        struct __kernel_timespec ts = {timeout_ms / 1000,
                                       (long long)(timeout_ms % 1000) * 1000000ll};
        struct io_uring_getevents_arg arg;
        memset(&arg, 0, sizeof arg);
        arg.ts = (uint64_t)(uintptr_t)&ts;
        (void)uring_enter(io->ring_fd, io->pending, min_complete,
                          IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
    } else {
        (void)uring_enter(io->ring_fd, io->pending, min_complete,
                          min_complete ? IORING_ENTER_GETEVENTS : 0, NULL, 0);
    }
    io->pending = 0;

    int got = 0;
    uint32_t head = *io->cq_head;
    while (got < max && head != __atomic_load_n(io->cq_tail, __ATOMIC_ACQUIRE)) {
        struct io_uring_cqe cqe = io->cqes[head & *io->cq_mask];
        head++;
        if (cqe.user_data == NO_REPLY) continue;
        uint32_t slot = (uint32_t)(cqe.user_data >> 32), gen = (uint32_t)cqe.user_data;
        if (slot >= io->nregs || !io->regs[slot].active || io->regs[slot].gen != gen)
            continue; /* stale */
        reg* r = &io->regs[slot];
        if (cqe.res == -ECANCELED) continue;
        unsigned revents = cqe.res >= 0 ? (unsigned)cqe.res : (unsigned)POLLERR;
        out[got].tag = r->tag;
        out[got].events =
            (uint8_t)(((revents & POLLIN) ? DX_IO_R : 0) | ((revents & POLLOUT) ? DX_IO_W : 0));
        out[got].error = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (out[got].error) out[got].events |= DX_IO_R; /* a read reveals EOF or the error */
        got++;
        arm(io, slot); /* the request answered once: ask again (level-triggered behaviour) */
    }
    __atomic_store_n(io->cq_head, head, __ATOMIC_RELEASE);
    return got;
}

const dx_io_ops dx_io_uring = {"io_uring", uring_open, uring_close, uring_add,
                               uring_mod,  uring_del,  uring_wait};
#endif
