CXX = g++
CXXFLAGS = -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror -std=c++17 -I$(SRCDIR) -O3 -Wno-deprecated-declarations
LDFLAGS = -lcrypto -lpthread -lssl

SRCDIR = src
OBJDIR = obj
BINDIR = bin

TARGET = zestdb
OUTPUT = $(BINDIR)/$(TARGET)

SRC_CXX = $(shell find $(SRCDIR) -name "*.cpp")
OBJ_CXX = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRC_CXX))

all: $(OUTPUT)

$(OUTPUT): $(OBJ_CXX)
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(CXXFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)

format:
	find src -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i --verbose {} +

run: $(OUTPUT)
	./$(OUTPUT)

.PHONY: all clean run