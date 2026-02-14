# Makefile for libapth - userspace thread library
# Author: xuehaonan27

# ==================== Configuration ====================
CC := gcc
AR := ar
CFLAGS := -Wall -Wextra -std=gnu11 -g -O2 -fPIC -DAPTH_DEBUG
LDFLAGS := -pthread
ARFLAGS := rcs

# Directories
SRC_DIR := src
TEST_DIR := test
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

# ==================== Source Files ====================
# Find all .c files in src directory (excluding test directory)
SRC_FILES := $(shell find $(SRC_DIR) -name '*.c' -type f)

# Generate object file paths
OBJ_FILES := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC_FILES))

# Test files
TEST_SOURCES := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BIN_DIR)/%,$(TEST_SOURCES))

# ==================== Targets ====================
.PHONY: all static shared tests clean distclean help

# Default target
all: static shared

# Build static library
static: $(STATIC_LIB)

# Build shared library
shared: $(SHARED_LIB)

# Build all tests
tests: $(TEST_BINS)

# ==================== Library Building ====================
# Create static library
$(STATIC_LIB): $(OBJ_FILES) | $(LIB_DIR)
	@echo "Creating static library: $@"
	$(AR) $(ARFLAGS) $@ $^
	@echo "Static library created successfully!"

# Create shared library
$(SHARED_LIB): $(OBJ_FILES) | $(LIB_DIR)
	@echo "Creating shared library: $@"
	$(CC) -shared -o $@ $^ $(LDFLAGS)
	@echo "Shared library created successfully!"

# ==================== Object Files ====================
# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling: $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ==================== Test Building ====================
# Build test executables
$(BIN_DIR)/%: $(TEST_DIR)/%.c $(STATIC_LIB) | $(BIN_DIR)
	@echo "Building test: $@"
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ -L$(LIB_DIR) -l$(LIB_NAME) $(LDFLAGS)
	@echo "Test built successfully: $@"

# ==================== Test Running ====================
.PHONY: run-tests test-%

# Run all tests
run-tests: tests
	@echo "=========================================="
	@echo "Running all tests..."
	@echo "=========================================="
	@for test in $(TEST_BINS); do \
		if [ -f $$test ]; then \
			echo "\n>>> Running: $$test"; \
			$$test || echo "Test failed: $$test"; \
		fi; \
	done
	@echo "=========================================="
	@echo "All tests completed!"
	@echo "=========================================="

# Run individual test (usage: make test-test_init)
test-%: $(BIN_DIR)/%
	@echo "Running test: $<"
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

# ==================== Help ====================
help:
	@echo "Libapth Makefile Targets:"
	@echo "=========================="
	@echo "  all          - Build both static and shared libraries (default)"
	@echo "  static       - Build static library (libapth.a)"
	@echo "  shared       - Build shared library (libapth.so)"
	@echo "  tests        - Build all test programs"
	@echo "  run-tests    - Build and run all tests"
	@echo "  test-<name>  - Run specific test (e.g., make test-test_init)"
	@echo "  clean        - Remove all build artifacts"
	@echo "  distclean    - Remove all build artifacts and backup files"
	@echo "  help         - Display this help message"
	@echo ""
	@echo "Build directories:"
	@echo "  $(BUILD_DIR)/obj/ - Object files"
	@echo "  $(BUILD_DIR)/lib/ - Library files"
	@echo "  $(BUILD_DIR)/bin/ - Test executables"
	@echo ""
	@echo "Compiler flags:"
	@echo "  CFLAGS  = $(CFLAGS)"
	@echo "  LDFLAGS = $(LDFLAGS)"

# ==================== Info Target ====================
.PHONY: info

info:
	@echo "Project Information:"
	@echo "===================="
	@echo "Library name: lib$(LIB_NAME)"
	@echo "Source files found: $(words $(SRC_FILES))"
	@echo "Test files found: $(words $(TEST_SOURCES))"
	@echo ""
	@echo "Source files:"
	@$(foreach src,$(SRC_FILES),echo "  - $(src)";)
	@echo ""
	@echo "Test files:"
	@$(foreach test,$(TEST_SOURCES),echo "  - $(test)";)

# ==================== Dependencies ====================
# Automatic dependency generation (optional enhancement)
-include $(OBJ_FILES:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(INCLUDES) -MM -MT $(patsubst %.d,%.o,$@) $< > $@
