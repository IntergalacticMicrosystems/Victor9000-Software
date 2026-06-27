#===============================================================================
# MEM Makefile for Open Watcom v2
# Target: Victor 9000 DOS 3.1 (16-bit, 8086)
# Build Host: Linux with Open Watcom cross-compiler
#
# Adapted from igc/Makefile. Unlike igc this tool uses the Watcom C library
# (printf), so no special viclibc objects are linked.
#===============================================================================

#-------------------------------------------------------------------------------
# Watcom Environment (adjust WATCOM path if different)
#-------------------------------------------------------------------------------
WATCOM ?= /opt/watcom

CC = $(WATCOM)/binl64/wcc
LD = $(WATCOM)/binl64/wlink

export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

#-------------------------------------------------------------------------------
# Target Configuration
#-------------------------------------------------------------------------------
TARGET = mem
SRCDIR = src
OBJDIR = obj
BINDIR = bin

#-------------------------------------------------------------------------------
# Compiler Flags (same set igc uses)
#   -0      8086 instructions only (critical for Victor 9000)
#   -mc     Compact memory model (near code, far data)
#   -s      Remove stack overflow checks
#   -os     Optimize for size
#   -d0     No debug info
#   -w4     Warning level 4
#   -e25    Stop after 25 errors
#   -zq     Quiet mode
#   -bt=dos Target DOS
#   -fpc    Floating-point calls to library
#   -j      char signed by default
#-------------------------------------------------------------------------------
CFLAGS = -0 -mc -s -os -d0 -w4 -e25 -zq -bt=dos -fpc -j -i=$(SRCDIR)

#-------------------------------------------------------------------------------
# Source and Object Files
#-------------------------------------------------------------------------------
SRCS = $(SRCDIR)/main.c \
       $(SRCDIR)/dosmem.c

OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.obj,$(SRCS))

#-------------------------------------------------------------------------------
# Build Rules
#-------------------------------------------------------------------------------
.PHONY: all clean distclean dirs

all: dirs $(BINDIR)/$(TARGET).exe
	@echo "Build complete: $(BINDIR)/$(TARGET).exe"

dirs:
	@mkdir -p $(OBJDIR) $(BINDIR)

# Link via a response file. SYSTEM dos pulls in the Watcom 16-bit C library.
$(BINDIR)/$(TARGET).exe: $(OBJS)
	@echo "Linking $(TARGET).exe..."
	@echo "NAME $@" > $(OBJDIR)/link.lnk
	@echo "SYSTEM dos" >> $(OBJDIR)/link.lnk
	@echo "OPTION quiet" >> $(OBJDIR)/link.lnk
	@echo "OPTION stack=8192" >> $(OBJDIR)/link.lnk
	@echo "OPTION map=$(BINDIR)/$(TARGET).map" >> $(OBJDIR)/link.lnk
	@for obj in $(OBJS); do echo "FILE $$obj" >> $(OBJDIR)/link.lnk; done
	$(LD) @$(OBJDIR)/link.lnk

# Every object depends on every header (only two files here).
$(OBJDIR)/%.obj: $(SRCDIR)/%.c $(SRCDIR)/*.h | dirs
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -fo=$@ $<

#-------------------------------------------------------------------------------
# Clean
#-------------------------------------------------------------------------------
clean:
	rm -f $(OBJDIR)/*.obj $(OBJDIR)/*.lnk
	rm -f $(BINDIR)/$(TARGET).exe $(BINDIR)/$(TARGET).map

distclean: clean
	rm -rf $(OBJDIR)
	-rmdir $(BINDIR) 2>/dev/null

#-------------------------------------------------------------------------------
# Deploy to MAME disk image (same mechanism igc uses)
#-------------------------------------------------------------------------------
MAME_DIR ?= /root/bldmame1
DISK_IMG ?= $(MAME_DIR)/vichd31.img
DEST_PARTITION ?= 1
DEST_DIR ?=

ifeq ($(strip $(DEST_DIR)),)
DEST_PATH = \\$(TARGET).EXE
else
DEST_PATH = \\$(DEST_DIR)\\$(TARGET).EXE
endif

.PHONY: deploy

deploy: $(BINDIR)/$(TARGET).exe
	@test -d $(MAME_DIR) || { \
		echo "MAME_DIR=$(MAME_DIR) not found - run with MAME_DIR=<path>"; \
		exit 1; }
	@echo "Deploying to MAME disk image..."
	cd $(MAME_DIR) && python3 -m vtg_image_util copy \
		$(CURDIR)/$(BINDIR)/$(TARGET).exe \
		$(DISK_IMG):$(DEST_PARTITION):$(DEST_PATH)
	@echo "Deployed to $(DISK_IMG):$(DEST_PARTITION):$(DEST_PATH)"
