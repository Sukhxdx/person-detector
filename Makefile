# `web` and `install` collide with real paths in the tree, so declaring every
# target phony is required for them to run at all.
.PHONY: all build debug test check-memory clean format install dirs validate \
        analyze-validation demo replay run simulate web

BUILD_DIR       := build
DEBUG_DIR       := build-debug
VALGRIND_DIR    := build-valgrind
CMAKE           := cmake
CMAKE_FLAGS     := -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
NPROC           := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TARGET          := person_detector
TEST_TARGET     := test_estimator
FORMAT_DIRS     := include src tests

all: build

dirs:
	@mkdir -p $(BUILD_DIR) $(DEBUG_DIR)

build: dirs
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --parallel $(NPROC)

debug: dirs
	$(CMAKE) -S . -B $(DEBUG_DIR) $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(DEBUG_DIR) --parallel $(NPROC)

test: debug
	$(CMAKE) --build $(DEBUG_DIR) --target $(TEST_TARGET) --parallel $(NPROC)
	$(DEBUG_DIR)/$(TEST_TARGET)

# Valgrind cannot run against an AddressSanitizer-instrumented binary: the two
# tools both remap large address ranges and abort. This target therefore builds
# its own sanitizer-free binary with debug symbols instead of reusing `debug`.
check-memory:
	@command -v valgrind >/dev/null 2>&1 || { echo "valgrind not installed"; exit 1; }
	$(CMAKE) -S . -B $(VALGRIND_DIR) $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo
	$(CMAKE) --build $(VALGRIND_DIR) --target $(TEST_TARGET) --parallel $(NPROC)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
		--error-exitcode=1 $(VALGRIND_DIR)/$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR) $(DEBUG_DIR) $(VALGRIND_DIR)

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 1; }
	clang-format -i $(shell find $(FORMAT_DIRS) -type f \( -name '*.c' -o -name '*.h' \))

install: build
	$(CMAKE) --install $(BUILD_DIR)

run: build
	sudo $(BUILD_DIR)/$(TARGET) --verbose

replay: debug
	@echo "Replaying sample traffic (Ctrl+C to stop)..."
	$(DEBUG_DIR)/$(TARGET) --replay validation/sample_traffic.jsonl --replay-loop \
		--interval 2 --verbose

validate:
	bash validation/run_validation.sh 30 5

analyze-validation:
	python3 validation/analyze_results.py \
		--estimates validation/sample_estimates.jsonl \
		--ground-truth validation/ground_truth_template.csv \
		--output validation/results_summary.md

demo:
	bash scripts/demo.sh

simulate:
	python3 scripts/simulate_traffic.py --count 10 --duration 10 --output /tmp/sim.jsonl

web:
	cd web && uvicorn main:app --reload --port 8000
