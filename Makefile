# ==============================================================================
# Makefile - Online Caro Game in C (POSIX C11)
# ==============================================================================

CC ?= gcc
CFLAGS := -Wall -Wextra -std=c11 -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS :=

BIN_DIR   := bin
BUILD_DIR := build

ifeq ($(OS),Windows_NT)
    SERVER_BIN := $(BIN_DIR)/caro_server.exe
    CLIENT_BIN := $(BIN_DIR)/caro_client.exe
    TEST_BIN   := $(BIN_DIR)/test_protocol.exe
    LDFLAGS    += -lws2_32
    RUN_TEST   = $(subst /,\,$(TEST_BIN))
define MAKE_DIRS
	@if not exist "$(subst /,\,$(BIN_DIR))" mkdir "$(subst /,\,$(BIN_DIR))"
	@if not exist "$(subst /,\,$(BUILD_DIR))" mkdir "$(subst /,\,$(BUILD_DIR))"
	@if not exist "$(subst /,\,$(BUILD_DIR)/server)" mkdir "$(subst /,\,$(BUILD_DIR)/server)"
	@if not exist "$(subst /,\,$(BUILD_DIR)/client)" mkdir "$(subst /,\,$(BUILD_DIR)/client)"
	@if not exist "$(subst /,\,$(BUILD_DIR)/common)" mkdir "$(subst /,\,$(BUILD_DIR)/common)"
	@if not exist "$(subst /,\,$(BUILD_DIR)/tests)" mkdir "$(subst /,\,$(BUILD_DIR)/tests)"
endef
define DO_CLEAN
	@if exist $(subst /,\,$(BUILD_DIR)) rmdir /S /Q $(subst /,\,$(BUILD_DIR))
	@if exist $(subst /,\,$(BIN_DIR)) rmdir /S /Q $(subst /,\,$(BIN_DIR))
endef
else
    SERVER_BIN := $(BIN_DIR)/caro_server
    CLIENT_BIN := $(BIN_DIR)/caro_client
    TEST_BIN   := $(BIN_DIR)/test_protocol
    RUN_TEST   = ./$(TEST_BIN)
define MAKE_DIRS
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)/server $(BUILD_DIR)/client $(BUILD_DIR)/common $(BUILD_DIR)/tests
endef
define DO_CLEAN
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
endef
endif

# Common object files
COMMON_OBJS := $(BUILD_DIR)/common/protocol.o

# Server object files
SERVER_OBJS := $(BUILD_DIR)/server/main.o \
               $(BUILD_DIR)/server/server.o \
               $(COMMON_OBJS)

# Client object files
CLIENT_OBJS := $(BUILD_DIR)/client/main.o \
               $(COMMON_OBJS)

# Test object files
TEST_OBJS   := $(BUILD_DIR)/tests/test_protocol.o \
               $(COMMON_OBJS)

.PHONY: all server client test clean dirs

all: dirs server client test

dirs:
	$(MAKE_DIRS)

server: dirs $(SERVER_BIN)

client: dirs $(CLIENT_BIN)

test: dirs $(TEST_BIN)
	@echo [TEST] Executing $(TEST_BIN)...
	@$(RUN_TEST)

$(SERVER_BIN): $(SERVER_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(CLIENT_BIN): $(CLIENT_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(TEST_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.c | dirs
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.c | dirs
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo [CLEAN] Removing build and binary directories...
	$(DO_CLEAN)
	@echo [OK] Clean completed.
