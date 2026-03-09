SHELL := bash

# ============================================================================
# Just Weather Client - Makefile
# ============================================================================
# Usage: make help - to see all available commands
#
# Quick start:
#   make install-lib  - setup jansson library symlink
#   make              - build debug version
#   make test         - run basic test
#   make help         - show all commands
# ============================================================================

# ------------------------------------------------------------
# Compiler + global settings
# ------------------------------------------------------------
CC          := gcc

SRC_DIR     := src/
LIB_DIR     := lib
INC_DIR     := includes

BUILD_MODE  ?= debug
BUILD_DIR   := build/$(BUILD_MODE)
BIN  := $(BUILD_DIR)/just-weather-client

# ------------------------------------------------------------
# Build configuration
# ------------------------------------------------------------
ifeq ($(BUILD_MODE),release)
    CFLAGS_BASE := -O3 -DNDEBUG
    BUILD_TYPE  := Release
else
    CFLAGS_BASE := -O1 -g
    BUILD_TYPE  := Debug
endif

# ------------------------------------------------------------
# Include directories
# ------------------------------------------------------------
SRC_INCLUDES := $(shell find $(SRC_DIR) -type d 2>/dev/null || echo $(SRC_DIR))
LIB_INCLUDES := $(shell find -L $(LIB_DIR) -type d 2>/dev/null || echo "")

# ------------------------------------------------------------
# Compiler flags
# ------------------------------------------------------------

INCLUDES := $(addprefix -I,$(SRC_INCLUDES)) $(addprefix -I,$(LIB_INCLUDES)) -I$(INC_DIR)
#POSIX_FLAGS := -D_POSIX_C_SOURCE=200809L

CFLAGS_SRC := $(CFLAGS_BASE) -Wall -Werror -Wfatal-errors -MMD -MP $(INCLUDES)
CFLAGS_LIB := $(CFLAGS_BASE) -w $(INCLUDES)

JANSSON_CFLAGS := $(filter-out -Werror -Wfatal-errors,$(CFLAGS_SRC)) -w -Ilib/jansson

LDFLAGS :=
LIBS    := -ljansson

# ------------------------------------------------------------
# Source and object files
# ------------------------------------------------------------
SRC_FILES := $(shell find $(SRC_DIR) -type f -name '*.c' 2>/dev/null)

OBJ_SRC := $(patsubst $(SRC_DIR)%.c,$(BUILD_DIR)/src/%.o,$(SRC_FILES))

# ------------------------------------------------------------
# Jansson integration
# ------------------------------------------------------------
JANSSON_SRC := $(shell find lib/jansson/ -maxdepth 1 -type f -name '*.c' 2>/dev/null)
JANSSON_OBJ := $(patsubst lib/jansson/%.c,$(BUILD_DIR)/jansson/%.o,$(JANSSON_SRC))

OBJ     := $(OBJ_SRC) $(JANSSON_OBJ)
DEP     := $(OBJ:.o=.d)

# ------------------------------------------------------------
# Build rules
# ------------------------------------------------------------
.PHONY: all
all: $(BIN)
	@echo "Build complete. [$(BUILD_TYPE)]"

# Show help
.PHONY: help
help:
	@echo "Just Weather Client - Makefile Commands"
	@echo ""
	@echo "SETUP:"
	@echo "  make install-lib     Setup symlink to jansson library"
	@echo ""
	@echo "BUILD:"
	@echo "  make                 Build debug version (default)"
	@echo "  make debug           Build debug version"
	@echo "  make release         Build release version"
	@echo "  make clean           Remove build artifacts"
	@echo "  make clean-all       Remove build artifacts and cache"
	@echo ""
	@echo "RUN & TEST:"
	@echo "  make run             Run client (show help)"
	@echo "  make test            Test with coordinates (Stockholm)"
	@echo "  make test-city       Test weather by city name"
	@echo "  make test-search     Test city search"
	@echo "  make test-homepage   Test homepage endpoint"
	@echo "  make test-echo       Test echo endpoint"
	@echo "  make test-all        Run all tests in sequence"
	@echo "  make interactive     Launch interactive mode"
	@echo "  make demo            Interactive demo of features"
	@echo ""
	@echo "CACHE:"
	@echo "  make show-cache      Show cache contents"
	@echo "  make clear-cache     Clear cache via client"
	@echo ""
	@echo "DEBUG:"
	@echo "  make asan            Build with AddressSanitizer"
	@echo "  make gdb             Run in GDB debugger"
	@echo "  make valgrind        Run with Valgrind"
	@echo ""
	@echo "CODE QUALITY:"
	@echo "  make format          Check code formatting"
	@echo "  make format-fix      Fix code formatting"
	@echo "  make lint            Run clang-tidy"
	@echo "  make lint-fix        Fix lint issues"
	@echo "  make lint-ci         Lint for CI (strict mode)"
	@echo ""

# Link client binary
$(BIN): $(OBJ)
	@echo "Linking client binary..."
	@mkdir -p $(dir $@)
	@$(CC) $(LDFLAGS) $(OBJ) -o $@ $(LIBS)

# Compile project sources (strict flags)
$(BUILD_DIR)/src/%.o: $(SRC_DIR)%.c
	@echo "Compiling client $<... [$(BUILD_TYPE)]"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS_SRC) -c $< -o $@

# Compile jansson library (relaxed flags)
$(BUILD_DIR)/jansson/%.o: lib/jansson/%.c
	@echo "Compiling Jansson $<... [$(BUILD_TYPE)]"
	@mkdir -p $(dir $@)
	@$(CC) $(JANSSON_CFLAGS) -c $< -o $@

# ------------------------------------------------------------
# Build modes
# ------------------------------------------------------------
.PHONY: debug
debug:
	@$(MAKE) --no-print-directory BUILD_MODE=debug all

.PHONY: release
release:
	@$(MAKE) --no-print-directory BUILD_MODE=release all

# ------------------------------------------------------------
# Debugging and Sanitizers
# ------------------------------------------------------------
.PHONY: asan
asan:
	@echo "Building with AddressSanitizer..."
	@$(MAKE) --no-print-directory \
		CFLAGS_BASE="-g -O1 -fsanitize=address -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address" \
		all

.PHONY: gdb
gdb: $(BIN)
	@echo "Launching client in GDB..."
	@gdb --quiet --args ./$(BIN)

.PHONY: valgrind
valgrind: $(BIN)
	@echo "Running under Valgrind..."
	@valgrind --leak-check=full --show-leak-kinds=all ./$(BIN)

# ------------------------------------------------------------
# Utilities
# ------------------------------------------------------------
.PHONY: run
run: $(BIN)
	@echo "Running $(BIN)..."
	@./$(BIN)

# Test client with example coordinates (Stockholm)
.PHONY: test
test: $(BIN)
	@echo "Testing client with Stockholm weather..."
	@./$(BIN) current 59.33 18.07

# Test weather by city name
.PHONY: test-city
test-city: $(BIN)
	@echo "Testing client with city name..."
	@./$(BIN) weather Stockholm SE

# Test city search
.PHONY: test-search
test-search: $(BIN)
	@echo "Testing city search..."
	@./$(BIN) cities Stock

# Test homepage endpoint
.PHONY: test-homepage
test-homepage: $(BIN)
	@echo "Testing homepage endpoint..."
	@./$(BIN) homepage

# Test echo endpoint
.PHONY: test-echo
test-echo: $(BIN)
	@echo "Testing echo endpoint..."
	@./$(BIN) echo

# Interactive mode
.PHONY: interactive
interactive: $(BIN)
	@./$(BIN) interactive

# Clear cache
.PHONY: clear-cache
clear-cache: $(BIN)
	@echo "Clearing cache..."
	@./$(BIN) clear-cache

# Show cache contents (if cache directory exists)
.PHONY: show-cache
show-cache:
	@echo "Cache directory contents:"
	@if [ -d "src/cache" ]; then \
		ls -lah src/cache/ 2>/dev/null || echo "Cache is empty"; \
	else \
		echo "Cache directory doesn't exist"; \
	fi

# Run all tests in sequence
.PHONY: test-all
test-all: $(BIN)
	@echo "=== Running all client tests ==="
	@echo ""
	@echo "1. Testing with coordinates (Stockholm):"
	@./$(BIN) current 59.33 18.07 || true
	@echo ""
	@echo "2. Testing weather by city name:"
	@./$(BIN) weather Stockholm SE || true
	@echo ""
	@echo "3. Testing city search:"
	@./$(BIN) cities Stock || true
	@echo ""
	@echo "4. Testing homepage endpoint:"
	@./$(BIN) homepage || true
	@echo ""
	@echo "5. Testing echo endpoint:"
	@./$(BIN) echo || true
	@echo ""
	@echo "=== All tests completed ==="

# Demonstrate all features
.PHONY: demo
demo: $(BIN)
	@echo "=== Just Weather Client Demo ==="
	@echo ""
	@echo "1. Getting weather for Stockholm (coordinates)..."
	@sleep 1
	@./$(BIN) current 59.33 18.07 || true
	@echo ""
	@read -p "Press Enter to continue..." dummy
	@echo ""
	@echo "2. Getting weather by city name..."
	@sleep 1
	@./$(BIN) weather Kyiv UA || true
	@echo ""
	@read -p "Press Enter to continue..." dummy
	@echo ""
	@echo "3. Searching for cities..."
	@sleep 1
	@./$(BIN) cities Kyiv || true
	@echo ""
	@echo "=== Demo completed ==="

.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf build
	@echo "Cleaned build artifacts."

# Clean everything including cache
.PHONY: clean-all
clean-all: clean
	@echo "Cleaning cache..."
	@rm -rf src/cache/*
	@echo "Everything cleaned."

# Show formatting errors without modifying files
.PHONY: format
format:
	@echo "Checking formatting..."
	@unformatted=$$(find . \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format -style=file -output-replacements-xml | \
		grep -c "<replacement " || true); \
	if [ $$unformatted -ne 0 ]; then \
		echo "$$unformatted file(s) need formatting"; \
		find . \( -name '*.c' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format -style=file -n -Werror; \
		exit 1; \
	else \
		echo "All files properly formatted"; \
	fi

# Actually fixes formatting
.PHONY: format-fix
format-fix:
	@echo "Applying clang-format..."
	find . \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i -style=file
	@echo "Formatting applied."

.PHONY: lint
lint:
	@echo "Running clang-tidy using compile_commands.json..."
	@find src \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Linting $$file"; \
		clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--quiet \
			--header-filter='^(src)/' \
			--system-headers=false || true; \
	done
	@echo "Lint complete (see warnings above)."

.PHONY: lint-fix
lint-fix:
	@echo "Running clang-tidy with auto-fix on src/ (excluding jansson)..."
	@find src \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Fixing $$file"; \
		clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--fix \
			--fix-errors \
			--header-filter='src/.*\.(h|hpp)$$' \
			--system-headers=false || true; \
	done
	@echo "Auto-fix complete. Please review changes with 'git diff'."

# CI target: fails only on naming violations
.PHONY: lint-ci
lint-ci:
	@echo "Running clang-tidy for CI (naming violations = errors)..."
	@rm -f /tmp/clang-tidy-failed
	@find src \( -name '*.c' -o -name '*.h' \) ! -path "*/jansson/*" -print0 | \
	while IFS= read -r -d '' file; do \
		echo "→ Checking $$file"; \
		if ! clang-tidy "$$file" \
			--config-file=.clang-tidy \
			--quiet \
			--header-filter='^(src)/' \
			--system-headers=false; then \
			touch /tmp/clang-tidy-failed; \
		fi; \
	done
	@if [ -f /tmp/clang-tidy-failed ]; then \
		rm -f /tmp/clang-tidy-failed; \
		echo "❌ Lint failed: naming standard violations found"; \
		exit 1; \
	else \
		echo "✅ Lint passed"; \
	fi

.PHONY: install-lib
install-lib:
	git clone https://github.com/stockholm-3/lib.git ../lib

# ------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------
-include $(DEP)
