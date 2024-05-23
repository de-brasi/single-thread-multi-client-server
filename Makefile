CXX ?= g++
OPTIMIZATION ?= -O2

# User supplied CXXFLAGS are appended, not replaced, so `make CXXFLAGS=-O0` and
# friends keep the warnings and the include paths.
ALL_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic $(OPTIMIZATION) -Isrc -Itests $(CXXFLAGS)
LINK_FLAGS := $(LDFLAGS)

# make SANITIZE=1 builds everything under AddressSanitizer and
# UndefinedBehaviorSanitizer; the CI runs the test suite this way.
ifeq ($(SANITIZE),1)
ALL_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
LINK_FLAGS += -fsanitize=address,undefined
endif

BUILD_DIR := build
SERVER := $(BUILD_DIR)/server
UNIT_TESTS := $(BUILD_DIR)/unit_tests
INTEGRATION_TESTS := $(BUILD_DIR)/integration_tests

.PHONY: all test unit-test integration-test clean help

all: $(SERVER) $(UNIT_TESTS) $(INTEGRATION_TESTS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SERVER): src/server.cpp src/message_stream.h | $(BUILD_DIR)
	$(CXX) $(ALL_CXXFLAGS) src/server.cpp -o $@ $(LINK_FLAGS)

$(UNIT_TESTS): tests/unit_tests.cpp tests/test_framework.h src/message_stream.h | $(BUILD_DIR)
	$(CXX) $(ALL_CXXFLAGS) tests/unit_tests.cpp -o $@ $(LINK_FLAGS)

$(INTEGRATION_TESTS): tests/integration_tests.cpp tests/test_framework.h | $(BUILD_DIR)
	$(CXX) $(ALL_CXXFLAGS) tests/integration_tests.cpp -o $@ $(LINK_FLAGS)

test: unit-test integration-test

unit-test: $(UNIT_TESTS)
	./$(UNIT_TESTS)

integration-test: $(INTEGRATION_TESTS) $(SERVER)
	./$(INTEGRATION_TESTS) --server ./$(SERVER)

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make                 build the server and both test binaries"
	@echo "make test            run the unit and the integration tests"
	@echo "make unit-test       run the message framing unit tests"
	@echo "make integration-test run the end to end tests against a real server"
	@echo "make SANITIZE=1 test build with ASan/UBSan and run the tests"
	@echo "make clean           remove the build directory"
