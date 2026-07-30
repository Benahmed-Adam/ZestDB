CXX = g++

BASE_CXXFLAGS = -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror -Wnon-virtual-dtor -std=c++20 -I$(SRCDIR) -Wno-deprecated-declarations

SANFLAGS =
COMMON_LDFLAGS = -lcrypto -lssl -lzippp_static -lzip -lz -lbz2 -llzma -lzstd

ifeq ($(DEBUG), 1)
    CXXFLAGS = $(BASE_CXXFLAGS) -g3 -O0 $(SANFLAGS)
    LDFLAGS  = $(SANFLAGS) $(COMMON_LDFLAGS)
else
    CXXFLAGS = $(BASE_CXXFLAGS) -O3
    LDFLAGS  = $(COMMON_LDFLAGS)
endif

SRCDIR = src
OBJDIR = obj
BINDIR = bin

TARGET = zestdb
OUTPUT = $(BINDIR)/$(TARGET)

SRC_CXX = $(shell find $(SRCDIR) -name "*.cpp")
OBJ_CXX = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRC_CXX))

TESTDIR = tests
TESTBINDIR = test_bin

TEST_LDFLAGS = -lCatch2Main -lCatch2 -lpthread

TEST_SRC_CXX = $(filter-out $(SRCDIR)/main.cpp, $(SRC_CXX))
TEST_OBJ_CXX = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(TEST_SRC_CXX))

TEST_CPPS = $(wildcard $(TESTDIR)/test_*.cpp)
TEST_BINS = $(patsubst $(TESTDIR)/%.cpp, $(TESTBINDIR)/%, $(TEST_CPPS))

all:
	@$(MAKE) $(OUTPUT)

$(OUTPUT): $(OBJ_CXX)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

$(TESTBINDIR)/%: $(TESTDIR)/%.cpp $(TEST_OBJ_CXX)
	@mkdir -p $(BINDIR) $(TESTBINDIR)
	$(CXX) $(CXXFLAGS) $< $(TEST_OBJ_CXX) -o $@ $(LDFLAGS) $(TEST_LDFLAGS)

tests: $(TEST_BINS)
	@for test in $(TEST_BINS); do echo "=== Running $$test ===" && ./$$test && echo; done

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(OBJDIR) $(TESTBINDIR)

run: all
	./$(OUTPUT)

format:
	find . -type d -name lib -prune -o -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i {} +

.PHONY: all clean run format tests