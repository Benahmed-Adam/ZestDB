ifeq ($(DEBUG), 1)
    BUILD_TYPE = Debug
else
    BUILD_TYPE = Release
endif

BUILD_DIR = build/$(BUILD_TYPE)
OUTPUT = bin/zestdb

all: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --target zestdb --parallel

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

tests: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --parallel
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf build bin obj test_bin

run: all
	./$(OUTPUT)

format:
	find . -type d \( -name lib -o -name build \) -prune -o -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i {} +

.PHONY: all clean run format tests
