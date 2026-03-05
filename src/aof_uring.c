/* 
 * Copyright (c) 2024, Redis Ltd.
 * All rights reserved.
 *
 * This file implements io_uring support for AOF operations.
 */

#include "server.h"
#include "aof_uring.h"

#ifdef USE_IO_URING

#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Structure to track each write operation */
typedef struct aofWriteReq {
    struct iovec iov;           /* Scatter/gather array for writing */
    char data[];                 /* Flexible array for the actual data */
} aofWriteReq;

/* io_uring context */
static struct io_uring ring;
static int uring_enabled = 0;
static int pending_ops = 0;
static long long total_submissions = 0;
static long long total_completions = 0;

/* Initialize io_uring for AOF operations */
int initAOFUring(void) {
    /* Try to initialize io_uring with 1024 queue depth */
    int ret = io_uring_queue_init(1024, &ring, 0);
    
    if (ret < 0) {
        serverLog(LL_WARNING, "Failed to initialize io_uring: %s", strerror(-ret));
        uring_enabled = 0;
        return C_ERR;
    }
    
    uring_enabled = 1;
    pending_ops = 0;
    total_submissions = 0;
    total_completions = 0;
    
    serverLog(LL_NOTICE, "io_uring initialized successfully for AOF (queue depth: 1024)");
    return C_OK;
}

/* Asynchronous write operation */
int aofWriteUring(int fd, const char *buf, size_t len, off_t offset) {
    if (!uring_enabled) {
        return -1;  /* io_uring not available, fall back to traditional write */
    }
    
    /* Allocate request structure with space for the data */
    aofWriteReq *req = malloc(sizeof(*req) + len);
    if (!req) {
        serverLog(LL_WARNING, "Failed to allocate memory for io_uring write request");
        return -1;
    }
    
    /* Copy the data and set up the iovec */
    memcpy(req->data, buf, len);
    req->iov.iov_base = req->data;
    req->iov.iov_len = len;
    
    /* Get a submission queue entry */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        free(req);
        serverLog(LL_WARNING, "No free submission queue entry available");
        return -1;
    }
    
    /* Prepare the write operation */
    io_uring_prep_writev(sqe, fd, &req->iov, 1, offset);
    io_uring_sqe_set_data(sqe, req);  /* Store pointer to our request for completion */
    
    pending_ops++;
    total_submissions++;
    
    /* If we have many pending, submit them now */
    if (pending_ops >= 64) {
        io_uring_submit(&ring);
    }
    
    /* Return success - the actual write result will be handled in completion */
    return len;
}

/* Asynchronous fsync operation */
int aofFsyncUring(int fd) {
    if (!uring_enabled) {
        return -1;  /* io_uring not available, fall back to traditional fsync */
    }
    
    /* Get a submission queue entry */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        serverLog(LL_WARNING, "No free submission queue entry for fsync");
        return -1;
    }
    
    /* Store file descriptor for completion */
    int *fd_ptr = malloc(sizeof(int));
    if (!fd_ptr) {
        return -1;
    }
    *fd_ptr = fd;
    
    /* Prepare the fsync operation */
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_sqe_set_data(sqe, fd_ptr);
    
    pending_ops++;
    total_submissions++;
    
    return 0;
}

/* Process completed io_uring operations */
void processAOFUringCompletions(void) {
    if (!uring_enabled || pending_ops == 0) {
        return;
    }
    
    struct io_uring_cqe *cqe;
    unsigned head;
    int processed = 0;
    
    /* Submit any pending operations first */
    io_uring_submit(&ring);
    
    /* Process all available completions */
    io_uring_for_each_cqe(&ring, head, cqe) {
        void *data = io_uring_cqe_get_data(cqe);
        int res = cqe->res;  /* Result of the operation */
        
        if (data) {
            if (res < 0) {
                /* Operation failed */
                serverLog(LL_WARNING, "io_uring operation failed: %s (error code: %d)", 
                          strerror(-res), res);
                /* Update error stats */
                server.aof_last_write_errno = -res;
            } else if (res > 0) {
                /* Successful write - update Redis stats */
                server.aof_current_size += res;
                server.aof_last_incr_size += res;
                total_completions++;
            } else {
                /* Successful fsync or other operation */
                total_completions++;
            }
            
            /* Free the request data */
            free(data);
        }
        
        pending_ops--;
        processed++;
        io_uring_cqe_seen(&ring, cqe);
    }
    
    if (processed > 0) {
        serverLog(LL_DEBUG, "Processed %d io_uring completions", processed);
    }
    
    /* Log stats every 1000 operations */
    if (total_completions % 1000 == 0 && total_completions > 0) {
        serverLog(LL_VERBOSE, "io_uring stats: submitted=%lld, completed=%lld, pending=%d",
                  total_submissions, total_completions, pending_ops);
    }
}

/* Clean up io_uring resources */
void freeAOFUring(void) {
    if (uring_enabled) {
        /* Wait for any pending operations to complete (with timeout) */
        int timeout_ms = 5000;
        struct io_uring_cqe *cqe;
        
        serverLog(LL_NOTICE, "Cleaning up io_uring, waiting for %d pending operations", pending_ops);
        
        /* Try to process remaining completions */
        while (pending_ops > 0 && timeout_ms > 0) {
            struct __kernel_timespec ts = {
                .tv_sec = 0,
                .tv_nsec = 1000000  /* 1ms */
            };
            
            int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
            if (ret == 0) {
                void *data = io_uring_cqe_get_data(cqe);
                if (data) free(data);
                io_uring_cqe_seen(&ring, cqe);
                pending_ops--;
            }
            timeout_ms--;
        }
        
        /* Exit the io_uring instance */
        io_uring_queue_exit(&ring);
        uring_enabled = 0;
        
        serverLog(LL_NOTICE, "io_uring cleaned up. Final stats: submitted=%lld, completed=%lld",
                  total_submissions, total_completions);
    }
}

#endif /* USE_IO_URING */