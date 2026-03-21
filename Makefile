# fynab Makefile — macOS arm64, Apple Clang 15, SQLCipher via Homebrew
# -----------------------------------------------------------------------
# Install dependencies once:
#   brew install sqlcipher
# Then build:
#   make
# Run:
#   ./fynab [path/to/fynab.db]

CXX      := clang++
TARGET   := fynab
SRCDIR   := src
SRCS     := $(wildcard $(SRCDIR)/*.cpp)

# Homebrew prefix (arm64 default is /opt/homebrew; Intel is /usr/local)
BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
SQLCIPHER   := $(BREW_PREFIX)/opt/sqlcipher

# Flags
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic \
            -I$(SQLCIPHER)/include/sqlcipher

LDFLAGS  := -L$(SQLCIPHER)/lib \
            -lsqlcipher \
            -Wl,-rpath,$(SQLCIPHER)/lib

# Debug build
DEBUGFLAGS := -std=c++17 -g -fsanitize=address,undefined \
              -I$(SQLCIPHER)/include/sqlcipher

.PHONY: all debug clean install check-deps

all: check-deps $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built: $(TARGET)"

debug: $(SRCS)
	$(CXX) $(DEBUGFLAGS) $^ -o $(TARGET)_debug $(LDFLAGS)
	@echo "Built: $(TARGET)_debug"

check-deps:
	@if [ ! -d "$(SQLCIPHER)" ]; then \
		echo "ERROR: sqlcipher not found at $(SQLCIPHER)"; \
		echo "Install with: brew install sqlcipher"; \
		exit 1; \
	fi

clean:
	rm -f $(TARGET) $(TARGET)_debug

install: all
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"
