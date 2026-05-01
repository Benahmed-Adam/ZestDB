CXX = g++

BASE_CXXFLAGS = -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror -std=c++20 -I$(SRCDIR) -Wno-deprecated-declarations

ifeq ($(DEBUG), 1)
    CXXFLAGS = $(BASE_CXXFLAGS) -g -O0
else
    CXXFLAGS = $(BASE_CXXFLAGS) -O3
endif

LDFLAGS = -lcrypto -lssl

SRCDIR = src
OBJDIR = obj
BINDIR = bin

TARGET = zestdb
OUTPUT = $(BINDIR)/$(TARGET)

SRC_CXX = $(shell find $(SRCDIR) -name "*.cpp")
OBJ_CXX = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRC_CXX))

all: 
	@$(MAKE) $(OUTPUT)

$(OUTPUT): $(OBJ_CXX)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

format:
	find src -path src/lib -prune -o -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i --verbose {} +

run: all
	./$(OUTPUT)

.PHONY: all clean run