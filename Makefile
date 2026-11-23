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
    TEST_GAME_BIN := $(BIN_DIR)/test_game.exe
    TEST_STORAGE_BIN := $(BIN_DIR)/test_storage.exe
    TEST_AUTH_BIN := $(BIN_DIR)/test_auth.exe
    LDFLAGS    += -lws2_32
    RUN_TEST   = $(subst /,\,$(TEST_BIN))
    RUN_TEST_GAME = $(subst /,\,$(TEST_GAME_BIN))
    RUN_TEST_STORAGE = $(subst /,\,$(TEST_STORAGE_BIN))
    RUN_TEST_AUTH = $(subst /,\,$(TEST_AUTH_BIN))
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
    TEST_GAME_BIN := $(BIN_DIR)/test_game
    TEST_STORAGE_BIN := $(BIN_DIR)/test_storage
    TEST_AUTH_BIN := $(BIN_DIR)/test_auth
    RUN_TEST   = ./$(TEST_BIN)
    RUN_TEST_GAME = ./$(TEST_GAME_BIN)
    RUN_TEST_STORAGE = ./$(TEST_STORAGE_BIN)
    RUN_TEST_AUTH = ./$(TEST_AUTH_BIN)
define MAKE_DIRS
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)/server $(BUILD_DIR)/client $(BUILD_DIR)/common $(BUILD_DIR)/tests
endef
define DO_CLEAN
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
endef
endif

# Common object files
COMMON_OBJS := $(BUILD_DIR)/common/protocol.o \
               $(BUILD_DIR)/common/sha256.o

# Server object files
SERVER_OBJS := $(BUILD_DIR)/server/main.o \
               $(BUILD_DIR)/server/server.o \
               $(BUILD_DIR)/server/game.o \
               $(BUILD_DIR)/server/storage.o \
               $(BUILD_DIR)/server/auth.o \
               $(COMMON_OBJS)

# Client object files
CLIENT_OBJS := $(BUILD_DIR)/client/main.o \
               $(COMMON_OBJS)

# Test object files
TEST_OBJS   := $(BUILD_DIR)/tests/test_protocol.o \
               $(COMMON_OBJS)
TEST_GAME_OBJS := $(BUILD_DIR)/tests/test_game.o \
                  $(BUILD_DIR)/server/game.o
TEST_STORAGE_OBJS := $(BUILD_DIR)/tests/test_storage.o \
                     $(BUILD_DIR)/server/storage.o
TEST_AUTH_OBJS := $(BUILD_DIR)/tests/test_auth.o \
                  $(BUILD_DIR)/server/auth.o \
                  $(BUILD_DIR)/common/sha256.o \
                  $(BUILD_DIR)/server/storage.o

.PHONY: all server client test test_game test_storage test_auth clean dirs

all: dirs server client test test_auth

dirs:
	$(MAKE_DIRS)

server: dirs $(SERVER_BIN)

client: dirs $(CLIENT_BIN)

test: dirs $(TEST_BIN) $(TEST_GAME_BIN) $(TEST_STORAGE_BIN) $(TEST_AUTH_BIN)
	@echo [TEST] Executing $(TEST_BIN)...
	@$(RUN_TEST)
	@echo [TEST] Executing $(TEST_GAME_BIN)...
	@$(RUN_TEST_GAME)
	@echo [TEST] Executing $(TEST_STORAGE_BIN)...
	@$(RUN_TEST_STORAGE)
	@echo [TEST] Executing $(TEST_AUTH_BIN)...
	@$(RUN_TEST_AUTH)

test_game: dirs $(TEST_GAME_BIN)
	@echo [TEST] Executing $(TEST_GAME_BIN)...
	@$(RUN_TEST_GAME)

test_storage: dirs $(TEST_STORAGE_BIN)
	@echo [TEST] Executing $(TEST_STORAGE_BIN)...
	@$(RUN_TEST_STORAGE)

test_auth: dirs $(TEST_AUTH_BIN)
	@echo [TEST] Executing $(TEST_AUTH_BIN)...
	@$(RUN_TEST_AUTH)

$(SERVER_BIN): $(SERVER_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lsqlite3 -lpthread

$(CLIENT_BIN): $(CLIENT_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(TEST_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_GAME_BIN): $(TEST_GAME_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_STORAGE_BIN): $(TEST_STORAGE_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lsqlite3

$(TEST_AUTH_BIN): $(TEST_AUTH_OBJS)
	@echo [LD] $@
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lsqlite3 -lpthread

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
