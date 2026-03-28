# Redis AOF Persistence: io_uring vs Traditional System Calls

A benchmark that isolates the I/O path of Redis's Append-Only File (AOF) persistence mechanism and demonstrates the performance improvement achieved by replacing traditional `write()` + `fsync()` system calls with Linux `io_uring` ring buffers.

---

## How to Run

### Prerequisites

- Linux kernel 5.6 or later
- GCC
- liburing-dev

Install liburing on Ubuntu/Debian:

```
sudo apt install liburing-dev
```

### One-liner: build and run everything

```
make clean && make build && make run && make demo
```

This will:
1. Clean any previous builds
2. Compile the benchmark and demo binaries
3. Run the full benchmark suite (5 test configurations), print a comparison table, and write results to `results.json`
4. Run the live demo with real-time progress bars showing per-operation latency

### Individual commands

```
make build                        # compile only
make run                          # full benchmark (table + results.json)
make demo                         # live demo with progress bars
./build/benchmark --ops=5000      # custom operation count
./build/demo --ops=200 --size=512 # custom demo parameters
```

### Viewing the dashboard

After running `make run`, open the dashboard in a browser:

```
xdg-open dashboard.html
```

The dashboard reads `results.json` and renders bar charts and line charts comparing throughput, latency, total time, and percentage improvement.

---

## What This Project Is About

Redis is an in-memory key-value store. When you run `SET foo bar`, the operation happens instantly in RAM. But RAM is volatile -- if the server loses power, all data is lost.

To prevent this, Redis writes every command to a file on disk called the Append-Only File (AOF). This is implemented in Redis's source file `aof.c`, in the function `flushAppendOnlyFile()`. The AOF serves as a write-ahead log: if Redis crashes, it can replay the file on startup and reconstruct the entire dataset.

This project does not implement a key-value store. It isolates and benchmarks the specific I/O pattern that `flushAppendOnlyFile()` uses to persist commands to disk, and compares the traditional implementation against an io_uring-based alternative.

---

## What Exactly Is Being Written to Disk

Each benchmark operation simulates one Redis command being appended to the AOF.

In real Redis, a command like `SET foo bar` is serialized in RESP (Redis Serialization Protocol) format:

```
*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
```

In our benchmark, each operation writes a buffer of N bytes (default 256) to a temporary file in `/tmp/`. The buffer is filled with the character `A` and terminated with a newline. The content does not matter -- what matters is the I/O path: the act of pushing bytes to the kernel and forcing the kernel to flush them to the physical storage device.

The temporary file is deleted immediately after the benchmark completes. We are not measuring what is written to disk, but how fast the writing happens.

---

## How Each File Works

### src/aof.h

Defines the `BenchResult` struct that both implementations return:

```c
typedef struct {
    int      num_ops;          // number of append operations performed
    size_t   op_size;          // size of each append payload in bytes
    double   total_time_ms;    // wall-clock time in milliseconds
    double   ops_per_sec;      // throughput: num_ops / total_time_sec
    double   avg_latency_us;   // average per-op latency in microseconds
} BenchResult;
```

Both implementations expose the same function signature, taking the number of operations and payload size, and returning a `BenchResult`.

### src/aof_traditional.c

This replicates what Redis does in `flushAppendOnlyFile()` when `appendfsync` is set to `always`. For each operation:

```c
for (int i = 0; i < num_ops; i++) {
    write(fd, buf, op_size);   // system call 1: userspace -> kernel page cache
    fsync(fd);                 // system call 2: kernel page cache -> physical disk
}
```

`write()` copies the buffer from userspace into the kernel's page cache. At this point the data exists only in RAM managed by the kernel. `fsync()` forces the kernel to flush the page cache for this file descriptor to the physical storage device and blocks until the device confirms the data is stored.

For 1000 operations, this makes 2000 system calls. Each system call involves a context switch from user mode to kernel mode and back, which costs roughly 1-5 microseconds of overhead on top of the actual I/O time.

### src/aof_iouring.c

This is the improved implementation using io_uring. Instead of making two blocking system calls per operation, it batches operations into a shared ring buffer:

```c
while (submitted < num_ops) {
    // Prepare a batch of 32 write+fsync pairs
    for (int i = 0; i < batch; i++) {
        sqe = io_uring_get_sqe(&ring);              // get slot in ring buffer (no syscall)
        io_uring_prep_write(sqe, fd, buf, ...);      // fill in write request (no syscall)
        sqe->flags |= IOSQE_IO_LINK;                // chain: fsync must wait for write

        sqe = io_uring_get_sqe(&ring);              // get another slot (no syscall)
        io_uring_prep_fsync(sqe, fd, 0);             // fill in fsync request (no syscall)
    }

    io_uring_submit(&ring);    // ONE system call: submit all 64 requests to kernel

    // Reap completions
    for (int i = 0; i < batch * 2; i++) {
        io_uring_wait_cqe(&ring, &cqe);             // read result from completion queue
        io_uring_cqe_seen(&ring, cqe);
    }
}
```

For 1000 operations with a batch size of 32, this makes roughly 63 system calls instead of 2000.

### src/benchmark.c

The main driver program. It runs both implementations across multiple test configurations (varying operation count and payload size), measures wall-clock time using `clock_gettime(CLOCK_MONOTONIC)`, computes throughput and latency, prints a formatted comparison table to the terminal, and writes all results as JSON to `results.json`.

### src/demo_live.c

A separate binary for live demonstrations. It runs the same workload but shows a real-time progress bar with per-operation latency as operations execute. The traditional mode visibly takes longer than the io_uring mode, making the improvement immediately apparent to an observer.

### dashboard.html

A single-file HTML page that loads `results.json` and renders four charts using Chart.js: throughput comparison, latency comparison, total time comparison, and per-configuration improvement percentage.

---

## How io_uring Reduces System Calls

### The traditional approach: 2 system calls per operation

```
Operation 1:
  User mode:  write(fd, buf, 256)  -->  [context switch to kernel]
  Kernel:     copies 256 bytes to page cache
  Kernel:     [context switch back to user]  -->  returns
  User mode:  fsync(fd)  -->  [context switch to kernel]
  Kernel:     flushes page cache to SSD, waits for confirmation
  Kernel:     [context switch back to user]  -->  returns

Operation 2:
  (same two context switches again)

...

1000 operations = 2000 system calls = 4000 context switches
```

Each context switch has a fixed cost: the CPU must save all user-mode registers, switch the page table to the kernel's address space, execute the kernel code, switch back, and restore registers. On modern hardware this takes 1-5 microseconds per switch.

### The io_uring approach: 2 system calls per batch of 32 operations

io_uring works by sharing two ring buffers between userspace and the kernel:

- The Submission Queue (SQ): userspace writes I/O requests here
- The Completion Queue (CQ): the kernel writes results here

Both queues live in memory that is mapped into both userspace and kernel space. This means filling in an I/O request (calling `io_uring_prep_write` or `io_uring_prep_fsync`) does not require a system call. It is just writing to a struct in shared memory.

```
Batch of 32 operations:

  User mode:  io_uring_prep_write(sqe, ...)   // writes to shared memory, no syscall
              io_uring_prep_fsync(sqe, ...)   // writes to shared memory, no syscall
              io_uring_prep_write(sqe, ...)   // writes to shared memory, no syscall
              io_uring_prep_fsync(sqe, ...)   // writes to shared memory, no syscall
              ... (x32 pairs = 64 SQEs filled, still zero syscalls)

              io_uring_submit(&ring)          // ONE system call: tells kernel "go"
                Kernel processes all 64 requests, potentially optimizing order

              io_uring_wait_cqe(&ring, ...)   // ONE system call: "give me results"
                Kernel returns all 64 completion statuses

1000 operations / 32 per batch = ~31 batches = ~62 system calls = ~124 context switches
```

The `IOSQE_IO_LINK` flag ensures that within each pair, the fsync only starts after the corresponding write completes, maintaining the same durability guarantee as the traditional approach.

### System call comparison

```
                    Traditional          io_uring (batch=32)
Operations          1000                 1000
Syscalls per op     2                    ~0.062
Total syscalls      2000                 ~62
Context switches    4000                 ~124
Reduction           --                   ~97%
```

---

## Why io_uring Is Not Used Everywhere

Despite the performance advantage, io_uring has significant limitations that prevent widespread adoption:

### 1. Linux only

io_uring is a Linux kernel feature. It does not exist on macOS, Windows, FreeBSD, or any other operating system. Redis runs on all of these platforms. Adopting io_uring would require maintaining two completely separate I/O paths and testing both. Most projects avoid this complexity.

### 2. Requires recent kernels

io_uring was introduced in Linux 5.1 (2019) and only became stable and fully featured around 5.6-5.10. Many production environments, especially enterprise Linux distributions like RHEL/CentOS, run older kernels. A feature that only works on recent kernels cannot be relied upon as the primary I/O mechanism.

### 3. Security vulnerabilities

io_uring has had a disproportionate number of CVEs (security vulnerabilities) since its introduction. The interface is complex and exposes a large attack surface in the kernel. As a result:

- Google disabled io_uring on all production servers and ChromeOS in 2023
- Android has io_uring disabled by default
- Several container runtimes (Docker, gVisor) restrict or block io_uring

For security-sensitive deployments, the performance benefit does not justify the risk.

### 4. Complexity

The traditional `write()` + `fsync()` pattern is four lines of obvious, debuggable code. The io_uring equivalent involves ring buffer initialization, SQE preparation, flag management (IOSQE_IO_LINK for ordering), batch size tuning, CQE reaping, and error handling through completion queue entries rather than return values. Bugs in io_uring usage can silently corrupt data or lose writes without any visible error at the call site.

### 5. Diminishing returns in practice

Our benchmark isolates the I/O path and measures it in a tight loop. In real Redis, the main thread also handles:

- Network I/O (accepting connections, reading commands, sending replies)
- Command parsing (RESP protocol deserialization)
- Data structure operations (hash table lookups, sorted set insertions)
- Memory allocation and management
- Replication, pub/sub, Lua scripting

The AOF write is a fraction of the total work per command. A 3-9x improvement in the I/O path might translate to only a 10-30% improvement in end-to-end throughput, depending on the workload.

### 6. Redis already mitigates the problem

In the default configuration (`appendfsync=everysec`), Redis offloads the expensive `fsync()` call to a background thread and only does it once per second rather than per command. This already eliminates most of the blocking overhead. io_uring provides the biggest improvement for `appendfsync=always`, which is the least commonly used mode because of its performance cost.

### 7. Kernel memory overhead

Each io_uring instance allocates kernel memory for the submission and completion queues. With high queue depths or many io_uring instances, this adds up. In memory-constrained environments, this overhead may not be acceptable.

---

## Project Structure

```
miniProject/
  Makefile                build system (make build / make run / make demo)
  dashboard.html          Chart.js visualization dashboard
  results.json            generated benchmark data (after make run)
  README.md               this file
  src/
    aof.h                 shared BenchResult type and function declarations
    aof_traditional.c     write() + fsync() implementation
    aof_iouring.c         io_uring batched implementation
    benchmark.c           main driver: runs both, prints table, writes JSON
    demo_live.c           live demo with progress bars for presentations
  build/
    benchmark             compiled benchmark binary (after make build)
    demo                  compiled demo binary (after make demo)
```