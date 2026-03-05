#ifndef AOF_URING_H
#define AOF_URING_H

#ifdef USE_IO_URING
#include <liburing.h>
#include "server.h"

/* Initialize io_uring for AOF operations
 * Returns C_OK on success, C_ERR on failure */
int initAOFUring(void);

/* Asynchronous write - replaces traditional write()
 * Parameters:
 *   fd     - file descriptor to write to
 *   buf    - buffer to write
 *   len    - length of buffer
 *   offset - offset in file to write at
 * Returns:
 *   len    - on success (submitted asynchronously)
 *   -1     - on failure (fall back to traditional write)
 */
int aofWriteUring(int fd, const char *buf, size_t len, off_t offset);

/* Asynchronous fsync - replaces redis_fsync()
 * Parameters:
 *   fd - file descriptor to fsync
 * Returns:
 *   0 - on success (submitted asynchronously)
 *   -1 - on failure (fall back to traditional fsync)
 */
int aofFsyncUring(int fd);

/* Process completed io_uring operations
 * Should be called regularly from serverCron() */
void processAOFUringCompletions(void);

/* Clean up io_uring resources on server shutdown */
void freeAOFUring(void);

#else /* USE_IO_URING */

/* Stub functions for when io_uring is disabled at compile time */
static inline int initAOFUring(void) { return C_ERR; }
static inline void processAOFUringCompletions(void) {}
static inline void freeAOFUring(void) {}
static inline int aofWriteUring(int fd, const char *buf, size_t len, off_t offset) { return -1; }
static inline int aofFsyncUring(int fd) { return -1; }

#endif /* USE_IO_URING */
#endif /* AOF_URING_H */