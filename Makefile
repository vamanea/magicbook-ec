# This Makefile builds the kernel module using the running kernel's
# build system. It supports cross-compilation and custom kernel paths.

# Kernel build directory - defaults to running kernel
# Override with: make KERNELDIR=/path/to/kernel
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Module name (must match .c file name without extension)
MODULE_NAME = magicbook-ec

# Architecture
ARCH ?= arm64

# Cross-compilation prefix (empty by default)
CROSS_COMPILE ?=

# Build directory (where object files go)
BUILD_DIR ?= $(PWD)

# Verbose output (0=quiet, 1=verbose)
V ?= 0

# Source files (add more .c files here if needed)
obj-m += $(MODULE_NAME).o

# Additional source files (if module has multiple C files)
# $(MODULE_NAME)-objs += file1.o file2.o

# Additional compiler flags
ccflags-y := -Wall -Werror -Wno-declaration-after-statement -Wno-unused-function
ccflags-y += -DDEBUG

# Kernel module build command
all: $(BUILD_DIR)
	$(MAKE) -C $(KERNELDIR) M=$(BUILD_DIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		V=$(V) \
		modules

# Clean build artifacts
clean:
	rm -f *.o *.ko *.mod.c *.mod.o *.symvers *.order
	rm -f .*.cmd .tmp_versions/
	rm -f Module.symvers modules.order

# Install the module
install: all
	sudo make -C $(KERNELDIR) M=$(BUILD_DIR) modules_install
	sudo depmod -a

# Remove the module from system
uninstall:
	sudo rm -f /lib/modules/$(shell uname -r)/extra/$(MODULE_NAME).ko
	sudo depmod -a

# Load the module
load: all
	sudo modprobe sparse-keymap
	sudo insmod $(BUILD_DIR)/$(MODULE_NAME).ko

# Unload the module
unload:
	sudo rmmod $(MODULE_NAME)

# Check module information
info:
	modinfo $(BUILD_DIR)/$(MODULE_NAME).ko

# Quick build and test
quick: all test

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build the kernel module"
	@echo "  clean     - Remove build artifacts"
	@echo "  install   - Install module to system"
	@echo "  uninstall - Remove module from system"
	@echo "  load      - Load the module"
	@echo "  unload    - Unload the module"
	@echo "  info      - Show module information"
	@echo "  test      - Load module and test /proc entry"
	@echo "  quick     - Build and test"
	@echo ""
	@echo "Variables:"
	@echo "  KERNELDIR - Path to kernel build directory (default: /lib/modules/$$(uname -r)/build)"
	@echo "  ARCH      - Target architecture (default: auto-detect)"
	@echo "  CROSS_COMPILE - Cross-compilation prefix (default: empty)"
	@echo "  V         - Verbose output: 0=silent, 1=verbose (default: 0)"
	@echo ""
	@echo "Example: make KERNELDIR=~/linux-5.15 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-"

# Phony targets (not actual files)
.PHONY: all clean install uninstall load unload info test quick help
