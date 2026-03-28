# Redis AOF io_uring Benchmark
#
# Build:  make build
# Run:    make run
# Clean:  make clean

CC       = gcc
CFLAGS   = -O2 -Wall -Wextra
LDFLAGS  = -luring
SRC_DIR  = src
BUILD_DIR = build
TARGET   = $(BUILD_DIR)/benchmark

SRCS = $(SRC_DIR)/aof_traditional.c \
       $(SRC_DIR)/aof_iouring.c \
       $(SRC_DIR)/benchmark.c

DEMO_SRC = $(SRC_DIR)/demo_live.c

.PHONY: all build run demo clean

all: build

build: $(TARGET)

$(TARGET): $(SRCS) $(SRC_DIR)/aof.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo "✅ Build complete: $(TARGET)"

$(BUILD_DIR)/demo: $(DEMO_SRC) $(SRC_DIR)/aof.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(DEMO_SRC) $(LDFLAGS)
	@echo "✅ Demo build complete: $(BUILD_DIR)/demo"

run: build
	@echo ""
	@./$(TARGET)

demo: $(BUILD_DIR)/demo
	@echo ""
	@./$(BUILD_DIR)/demo

clean:
	rm -rf $(BUILD_DIR) results.json
	@echo "🧹 Cleaned."
