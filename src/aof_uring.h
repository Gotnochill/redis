#ifndef AOF_URING_H
#define AOF_URING_H

#ifdef USE_IO_URING
#include <liburing.h>
#include "server.h"

/* io_uring context for AOF */
typedef struct aofUringCtx {
    struct io_uring ring;
    int enabled;
    int pending_ops;
    long long submissions;
    long long completions;
    long long failed_ops;
} aofUringCtx;

extern aofUringCtx server_aof_uring;

/* Initialize io_uring for AOF */
int initAOFUring(void);

/* Async write operations */
int aofWriteUring(int fd, const char *buf, size_t len, off_t offset);

/* Async fsync */
int aofFsyncUring(int fd);

/* Process completions - call from serverCron */
void processAOFUringCompletions(void);

/* Cleanup */
void freeAOFUring(void);

#else

/* Stubs for when io_uring is disabled */
static inline int initAOFUring(void) { return C_ERR; }
static inline void processAOFUringCompletions(void) {}
static inline void freeAOFUring(void) {}
static inline int aofWriteUring(int fd, const char *buf, size_t len, off_t offset) { return -1; }
static inline int aofFsyncUring(int fd) { return -1; }

#endif /* USE_IO_URING */
#endif /* AOF_URING_H */