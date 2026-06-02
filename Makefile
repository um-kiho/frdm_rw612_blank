# Makefile for MCUXpresso IDE integration
# This wraps west/CMake build commands for Zephyr

BOARD ?= frdm_rw612
BUILD_TYPE ?= debug
BUILD_DIR = $(BUILD_TYPE)

.PHONY: all clean configure build flash

# Default target
all: build

# Configure using CMake preset
configure:
	@echo "Configuring Zephyr project for $(BOARD)..."
	west build -b $(BOARD) -d $(BUILD_DIR) --cmake-only

# Build the project
build:
	@echo "Building Zephyr project..."
	@if [ ! -d "$(BUILD_DIR)" ]; then \
		echo "Build directory not found. Running configure first..."; \
		west build -b $(BOARD) -d $(BUILD_DIR); \
	else \
		west build -d $(BUILD_DIR); \
	fi

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@if [ -d "$(BUILD_DIR)" ]; then \
		rm -rf $(BUILD_DIR); \
	fi
	@echo "Clean complete."

# Flash to board
flash: build
	@echo "Flashing to $(BOARD)..."
	west flash -d $(BUILD_DIR)

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build the project (default)"
	@echo "  configure - Run CMake configuration only"
	@echo "  build     - Build the project"
	@echo "  clean     - Clean build artifacts"
	@echo "  flash     - Build and flash to board"
	@echo ""
	@echo "Variables:"
	@echo "  BOARD      - Target board (default: frdm_rw612)"
	@echo "  BUILD_TYPE - Build configuration (default: debug)"
