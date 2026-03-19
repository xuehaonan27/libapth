# Makefile for libapth - userspace thread library
# Author: xuehaonan27

# ==================== Configuration ====================
CC := gcc
AR := ar
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC \
	-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
	-DAPTH_CUR_USING_KEYWORD \
	-DAPTH_HOLD_INITIALIZER_PTHREAD \
	-DAPTH_PREEMPT_SIGNAL \
	-DAPTH_NUMA
LDFLAGS := -pthread
ARFLAGS := rcs

# Directories
SRC_DIR := src
TEST_DIR := test
APPS_DIR := apps
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
LIB_DIR := $(BUILD_DIR)/lib
BIN_DIR := $(BUILD_DIR)/bin

# Library names
LIB_NAME := apth
STATIC_LIB := $(LIB_DIR)/lib$(LIB_NAME).a
SHARED_LIB := $(LIB_DIR)/lib$(LIB_NAME).so

# Include paths
INCLUDES := -I$(SRC_DIR)

# Installation directories
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DESTDIR ?=

# ==================== Source Files ====================
# Find all .c files in src directory
SRC_FILES := $(shell find $(SRC_DIR) -name '*.c' -type f)

# Generate object file paths
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))

# All test sources
ALL_TEST_SOURCES := $(wildcard $(TEST_DIR)/*.c)

# -------------------------------------------------------
# Classify test sources into four categories:
#
#  1. IO_APTH_SRCS   – new I/O tests that USE libapth.
#                      Filename ends with _apth.c.
#                      Built against libapth.a; run with LD_PRELOAD=libapth.so.
#
#  2. IO_PTHREAD_SRCS – tests that use plain pthreads (no apth).
#                      Filename ends with _pthread.c.
#                      Built against -lpthread only; run WITHOUT LD_PRELOAD.
#
#  3. TCP_SRCS       – multi-process TCP server/client test pair.
#                      test_tcp_server.c + test_tcp_client.c.
#                      Built against -lpthread; run via coordinated run-tcp-test.
#
#  4. LEGACY_TEST_SRCS – all other tests (original test suite).
#                      Built against libapth.a; run with LD_PRELOAD=libapth.so.
# -------------------------------------------------------
IO_APTH_SRCS    := $(filter %_apth.c,    $(ALL_TEST_SOURCES))
IO_PTHREAD_SRCS := $(filter %_pthread.c, $(ALL_TEST_SOURCES))
TCP_SERVER_SRC  := $(TEST_DIR)/test_tcp_server.c
TCP_CLIENT_SRC  := $(TEST_DIR)/test_tcp_client.c
TCP_SRCS        := $(TCP_SERVER_SRC) $(TCP_CLIENT_SRC)
# Legacy = everything that is not _apth, _pthread, or tcp
LEGACY_TEST_SRCS := $(filter-out %_apth.c %_pthread.c \
                        $(TCP_SERVER_SRC) $(TCP_CLIENT_SRC), \
                        $(ALL_TEST_SOURCES))

# Corresponding binaries
IO_APTH_BINS    := $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/%, $(IO_APTH_SRCS))
IO_PTHREAD_BINS := $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/%, $(IO_PTHREAD_SRCS))
TCP_SERVER_BIN  := $(BIN_DIR)/test_tcp_server
TCP_CLIENT_BIN  := $(BIN_DIR)/test_tcp_client
LEGACY_TEST_BINS := $(patsubst $(TEST_DIR)/%.c, $(BIN_DIR)/%, $(LEGACY_TEST_SRCS))

ALL_TEST_BINS := $(IO_APTH_BINS) $(IO_PTHREAD_BINS) \
                 $(TCP_SERVER_BIN) $(TCP_CLIENT_BIN) \
                 $(LEGACY_TEST_BINS)

# ==================== Application Files ====================
# Applications in apps/ directory - real-world test applications
APP_C_FILES := $(wildcard $(APPS_DIR)/*.c)
APP_SOURCES := $(filter-out %_pthread.c, $(APP_C_FILES))
APP_PTHREAD_SOURCES := $(filter %_pthread.c, $(APP_C_FILES))
APP_BINS := $(patsubst $(APPS_DIR)/%.c, $(BIN_DIR)/%, $(APP_SOURCES))
APP_PTHREAD_BINS := $(patsubst $(APPS_DIR)/%.c, $(BIN_DIR)/%, $(APP_PTHREAD_SOURCES))

# ==================== Phony targets ====================
.PHONY: all static shared tests apps
.PHONY: run-tests run-io-apth-tests run-io-pthread-tests run-tcp-test run-io-tests
.PHONY: test-% clean distclean help info
.PHONY: install uninstall 

# Default target
all: static shared

# Build static library
static: $(STATIC_LIB)

# Build shared library
shared: $(SHARED_LIB)

# Build ALL test binaries
tests: shared $(ALL_TEST_BINS)

# Build ALL application binaries
apps: shared $(APP_BINS) $(APP_PTHREAD_BINS)

# ==================== Library Building ====================
$(STATIC_LIB): $(OBJ_FILES) | $(LIB_DIR)
	@echo "Creating static library: $@"
	$(AR) $(ARFLAGS) $@ $^
	@echo "Static library created successfully!"

$(SHARED_LIB): $(OBJ_FILES) | $(LIB_DIR)
	@echo "Creating shared library: $@"
	$(CC) -shared -o $@ $^ $(LDFLAGS) -ldl
	@echo "Shared library created successfully!"

# ==================== Object Files ====================
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling: $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ==================== Test Building ====================
# ----- Category 1: apth I/O tests -----
# Link against libapth; the hooks (write, read, …) are provided by the library.
$(IO_APTH_BINS): $(BIN_DIR)/%: $(TEST_DIR)/%.c $(STATIC_LIB) | $(BIN_DIR)
	@echo "Building apth I/O test: $@"
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ \
	    -L$(LIB_DIR) -l$(LIB_NAME) $(LDFLAGS) -ldl
	@echo "Done: $@"

# ----- Category 2: pthread I/O tests -----
# Pure pthreads; do NOT link libapth so the real libc functions are used.
$(IO_PTHREAD_BINS): $(BIN_DIR)/%: $(TEST_DIR)/%.c | $(BIN_DIR)
	@echo "Building pthread I/O test: $@"
	$(CC) $(CFLAGS) $< -o $@ -lpthread
	@echo "Done: $@"

# ----- Category 3: TCP multi-process tests -----
$(TCP_SERVER_BIN) $(TCP_CLIENT_BIN): $(BIN_DIR)/%: $(TEST_DIR)/%.c | $(BIN_DIR)
	@echo "Building TCP test binary: $@"
	$(CC) $(CFLAGS) $< -o $@ -lpthread
	@echo "Done: $@"

# ----- Category 4: Legacy tests -----
# Original test suite: link against libapth, run with LD_PRELOAD.
$(LEGACY_TEST_BINS): $(BIN_DIR)/%: $(TEST_DIR)/%.c $(STATIC_LIB) | $(BIN_DIR)
	@echo "Building legacy test: $@"
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ \
	    -L$(LIB_DIR) -l$(LIB_NAME) $(LDFLAGS)
	@echo "Done: $@"

# ==================== Application Building ====================
# Applications: link against libapth, run with LD_PRELOAD.
$(APP_BINS): $(BIN_DIR)/%: $(APPS_DIR)/%.c $(STATIC_LIB) | $(BIN_DIR)
	@echo "Building application: $@"
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ \
	    -L$(LIB_DIR) -l$(LIB_NAME) $(LDFLAGS) -ldl
	@echo "Done: $@"

$(APP_PTHREAD_BINS): $(BIN_DIR)/%: $(APPS_DIR)/%.c $(STATIC_LIB) | $(BIN_DIR)
	@echo "Building application: $@"
	$(CC) $(CFLAGS) $< -o $@ -lpthread
	@echo "Done: $@"

# ==================== Test Running ====================

# --- Run legacy tests (original suite) with LD_PRELOAD ---
run-tests: tests
	@echo "=========================================="
	@echo "Running legacy tests (with LD_PRELOAD)..."
	@echo "=========================================="
	@for t in $(LEGACY_TEST_BINS); do \
		if [ -f "$$t" ]; then \
			echo "\n>>> [legacy] $$t"; \
			LD_LIBRARY_PATH=$(LIB_DIR):$$LD_LIBRARY_PATH \
			LD_PRELOAD=$(SHARED_LIB) $$t \
			    && echo "  OK" || echo "  FAILED: $$t"; \
		fi; \
	done
	@echo "=========================================="

# --- Run apth I/O tests with LD_PRELOAD ---
run-io-apth-tests: shared $(IO_APTH_BINS)
	@echo "=========================================="
	@echo "Running apth I/O tests (with LD_PRELOAD)..."
	@echo "=========================================="
	@rc=0; \
	for t in $(IO_APTH_BINS); do \
		if [ -f "$$t" ]; then \
			echo "\n>>> [apth] $$t"; \
			LD_LIBRARY_PATH=$(LIB_DIR):$$LD_LIBRARY_PATH \
			LD_PRELOAD=$(SHARED_LIB) $$t \
			    || { echo "  FAILED: $$t"; rc=1; }; \
		fi; \
	done; \
	exit $$rc
	@echo "=========================================="

# --- Run pthread I/O tests WITHOUT LD_PRELOAD ---
run-io-pthread-tests: $(IO_PTHREAD_BINS)
	@echo "=========================================="
	@echo "Running pthread I/O tests (no LD_PRELOAD)..."
	@echo "=========================================="
	@rc=0; \
	for t in $(IO_PTHREAD_BINS); do \
		if [ -f "$$t" ]; then \
			echo "\n>>> [pthread] $$t"; \
			$$t || { echo "  FAILED: $$t"; rc=1; }; \
		fi; \
	done; \
	exit $$rc
	@echo "=========================================="

# --- Run multi-process TCP test ---
# The server prints "READY\n" to stdout when the listen socket is bound.
# A temporary FIFO is used to synchronise the two processes without polling.
run-tcp-test: $(TCP_SERVER_BIN) $(TCP_CLIENT_BIN)
	@echo ">>> Starting multi-process TCP test"
	@FIFO=$$(mktemp -u /tmp/apth_tcp_XXXXXX); \
	mkfifo "$$FIFO"; \
	$(TCP_SERVER_BIN) > "$$FIFO" & \
	SERVER_PID=$$!; \
	read READY_LINE < "$$FIFO"; \
	rm -f "$$FIFO"; \
	$(TCP_CLIENT_BIN); \
	CLIENT_RC=$$?; \
	wait $$SERVER_PID; \
	SERVER_RC=$$?; \
	if [ $$CLIENT_RC -eq 0 ] && [ $$SERVER_RC -eq 0 ]; then \
	    echo "[PASS] run-tcp-test"; \
	else \
	    echo "[FAIL] run-tcp-test (server_rc=$$SERVER_RC client_rc=$$CLIENT_RC)"; \
	    exit 1; \
	fi

# --- Run all I/O tests ---
run-io-tests: run-io-apth-tests run-io-pthread-tests run-tcp-test
	@echo "=========================================="
	@echo "All I/O tests completed."
	@echo "=========================================="

# --- Run a single legacy test by name (usage: make test-test_init) ---
test-%: $(BIN_DIR)/%
	@echo "Running legacy test: $<"
	@LD_LIBRARY_PATH=$(LIB_DIR):$$LD_LIBRARY_PATH \
	    LD_PRELOAD=$(SHARED_LIB) $<

# --- Run a single apth I/O test (usage: make test-io-apth-rw_pipe) ---
test-io-apth-%: $(BIN_DIR)/%_apth
	@echo "Running apth I/O test: $<"
	@LD_LIBRARY_PATH=$(LIB_DIR):$$LD_LIBRARY_PATH \
	    LD_PRELOAD=$(SHARED_LIB) $<

# --- Run a single pthread I/O test (usage: make test-io-pthread-rw_pipe) ---
test-io-pthread-%: $(BIN_DIR)/%_pthread
	@echo "Running pthread I/O test: $<"
	@$<

# ==================== Directory Creation ====================
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(LIB_DIR):
	@mkdir -p $(LIB_DIR)

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# ==================== Cleaning ====================
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	@echo "Clean complete!"

distclean: clean
	@echo "Performing distribution clean..."
	rm -f *~ $(SRC_DIR)/*~ $(TEST_DIR)/*~
	@echo "Distribution clean complete!"

# ==================== Installation ====================
# Headers to install (all .h files directly under src/)
PUBLIC_HEADERS := $(SRC_DIR)/apth.h

install: static shared
	@echo "Installing libraries to $(DESTDIR)$(LIBDIR)"
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
	@echo "Installing headers to $(DESTDIR)$(INCLUDEDIR)"
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 $(PUBLIC_HEADERS) $(DESTDIR)$(INCLUDEDIR)/
	@echo "Installation complete."
	@echo "Note: If you installed the shared library to a system directory,"
	@echo "      you may need to run 'ldconfig' as root."

uninstall:
	@echo "Removing libraries from $(DESTDIR)$(LIBDIR)"
	rm -f $(DESTDIR)$(LIBDIR)/lib$(LIB_NAME).a
	rm -f $(DESTDIR)$(LIBDIR)/lib$(LIB_NAME).so
	@echo "Removing headers from $(DESTDIR)$(INCLUDEDIR)"
	cd $(DESTDIR)$(INCLUDEDIR) && rm -f $(notdir $(PUBLIC_HEADERS))
	@echo "Uninstall complete."

# ==================== Help ====================
help:
	@echo "Libapth Makefile Targets"
	@echo "========================"
	@echo ""
	@echo "Library:"
	@echo "  all              Build both static and shared libraries (default)"
	@echo "  static           Build static library only (libapth.a)"
	@echo "  shared           Build shared library only (libapth.so)"
	@echo ""
	@echo "Tests – build:"
	@echo "  tests            Build ALL test binaries"
	@echo ""
	@echo "Applications:"
	@echo "  apps             Build ALL application binaries"
	@echo ""
	@echo "Tests – run:"
	@echo "  run-tests        Run legacy tests (original suite, with LD_PRELOAD)"
	@echo "  run-io-apth-tests    Run *_apth I/O tests (with LD_PRELOAD)"
	@echo "  run-io-pthread-tests Run *_pthread I/O tests (no LD_PRELOAD)"
	@echo "  run-tcp-test     Run multi-process TCP server+client test"
	@echo "  run-io-tests     Run all of the above I/O tests"
	@echo ""
	@echo "Tests – single:"
	@echo "  test-<name>           Run legacy test  (e.g. make test-test_init)"
	@echo "  test-io-apth-<stem>   Run apth I/O test (e.g. make test-io-apth-rw_pipe)"
	@echo "  test-io-pthread-<stem> Run pthread test (e.g. make test-io-pthread-rw_pipe)"
	@echo ""
	@echo "Misc:"
	@echo "  clean            Remove all build artifacts"
	@echo "  distclean        Remove all build artifacts and backup files"
	@echo "  info             Show source/test file counts"
	@echo "  install          Install libraries and headers to \$${PREFIX} (default /usr/local)"
	@echo "  uninstall        Remove installed files"
	@echo "  help             Display this help message"
	@echo ""
	@echo "Build directories:"
	@echo "  $(OBJ_DIR)  Object files"
	@echo "  $(LIB_DIR)  Library files"
	@echo "  $(BIN_DIR)  Test executables"

# ==================== Info Target ====================
info:
	@echo "Project Information"
	@echo "==================="
	@echo "Library name        : lib$(LIB_NAME)"
	@echo "Source files        : $(words $(SRC_FILES))"
	@echo "apth I/O tests      : $(words $(IO_APTH_SRCS))"
	@echo "pthread I/O tests   : $(words $(IO_PTHREAD_SRCS))"
	@echo "TCP multi-proc tests: $(words $(TCP_SRCS))"
	@echo "Legacy tests        : $(words $(LEGACY_TEST_SRCS))"
	@echo ""
	@echo "apth I/O test binaries:"
	@$(foreach b,$(IO_APTH_BINS),echo "  $(b)";)
	@echo ""
	@echo "pthread I/O test binaries:"
	@$(foreach b,$(IO_PTHREAD_BINS),echo "  $(b)";)
	@echo ""
	@echo "TCP test binaries:"
	@echo "  $(TCP_SERVER_BIN)"
	@echo "  $(TCP_CLIENT_BIN)"
	@echo ""
	@echo "Application binaries:"
	@$(foreach b,$(APP_BINS),echo "  $(b)";)
	@echo "Application pthread binaries:"
	@$(foreach b,$(APP_PTHREAD_SOURCES),echo "  $(b)";)

# ==================== Dependencies ====================
-include $(OBJ_FILES:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(INCLUDES) -MM -MT $(patsubst %.d,%.o,$@) $< > $@
