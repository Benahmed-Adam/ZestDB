CXX = g++

BASE_CXXFLAGS = -Wall -Wextra -Wpedantic -Wsign-conversion -Wshadow -Wunused -Werror -Wnon-virtual-dtor -std=c++20 -I$(SRCDIR) -Wno-deprecated-declarations

SANFLAGS =
COMMON_LDFLAGS = -lcrypto -lssl

ifeq ($(DEBUG), 1)
    SANFLAGS = -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer
    CXXFLAGS = $(BASE_CXXFLAGS) -g -O0 $(SANFLAGS)
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

run: all
	./$(OUTPUT)

.PHONY: all clean run