#===============================================================================
# IGC Makefile for Open Watcom v2
# Target: Victor 9000 DOS 3.1 (16-bit, 8086)
# Build Host: Linux with Open Watcom cross-compiler
#===============================================================================

#-------------------------------------------------------------------------------
# Watcom Environment (adjust WATCOM path if different)
#-------------------------------------------------------------------------------
WATCOM ?= /opt/watcom

# Use Linux binaries
CC = $(WATCOM)/binl64/wcc
LD = $(WATCOM)/binl64/wlink

# Export for Watcom tools
export WATCOM
export PATH := $(WATCOM)/binl64:$(PATH)
export INCLUDE := $(WATCOM)/h

#-------------------------------------------------------------------------------
# Target Configuration
#-------------------------------------------------------------------------------
TARGET = igc
SRCDIR = src
OBJDIR = obj
BINDIR = bin

#-------------------------------------------------------------------------------
# Compiler Flags
#-------------------------------------------------------------------------------
# -0        8086 instructions only (critical for Victor 9000)
# -mc       Compact memory model (near code, far data)
# -s        Remove stack overflow checks (smaller/faster)
# -os       Optimize for size
# -d0       No debug info (use -d2 for debugging)
# -w4       Warning level 4 (maximum warnings)
# -e25      Stop after 25 errors
# -zq       Quiet mode (suppress banner)
# -bt=dos   Target: DOS
# -fpc      Floating-point calls to library
# -j        Make char signed by default

CFLAGS = -0 -mc -s -os -d0 -w4 -e25 -zq -bt=dos -fpc -j -i=$(SRCDIR)

# Debug build flags (uncomment for debugging)
# CFLAGS = -0 -mc -d2 -w4 -e25 -zq -bt=dos -fpc -j -i=$(SRCDIR)

#-------------------------------------------------------------------------------
# Source and Object Files
#-------------------------------------------------------------------------------
# Explicitly list sources to avoid including test programs like keytest.c
SRCS = $(SRCDIR)/main.c \
       $(SRCDIR)/mem.c \
       $(SRCDIR)/screen.c \
       $(SRCDIR)/keyboard.c \
       $(SRCDIR)/dosapi.c \
       $(SRCDIR)/panel.c \
       $(SRCDIR)/ui.c \
       $(SRCDIR)/util.c \
       $(SRCDIR)/dialog.c \
       $(SRCDIR)/fileops.c \
       $(SRCDIR)/editor.c \
       $(SRCDIR)/config.c

OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.obj,$(SRCS))

#-------------------------------------------------------------------------------
# Build Rules
#-------------------------------------------------------------------------------

.PHONY: all clean distclean dirs

all: dirs $(BINDIR)/$(TARGET).exe
	@echo "Build complete: $(BINDIR)/$(TARGET).exe"

dirs:
	@mkdir -p $(OBJDIR) $(BINDIR)

# Link - generate linker response file for complex commands
$(BINDIR)/$(TARGET).exe: $(OBJS)
	@echo "Linking $(TARGET).exe..."
	@echo "NAME $@" > $(OBJDIR)/link.lnk
	@echo "SYSTEM dos" >> $(OBJDIR)/link.lnk
	@echo "OPTION quiet" >> $(OBJDIR)/link.lnk
	@echo "OPTION stack=8192" >> $(OBJDIR)/link.lnk
	@echo "OPTION map=$(BINDIR)/$(TARGET).map" >> $(OBJDIR)/link.lnk
	@for obj in $(OBJS); do echo "FILE $$obj" >> $(OBJDIR)/link.lnk; done
	$(LD) @$(OBJDIR)/link.lnk

# Compile pattern rule. Every object depends on every header - with 13
# files a few spurious rebuilds beat maintaining per-file lists by hand.
$(OBJDIR)/%.obj: $(SRCDIR)/%.c $(SRCDIR)/*.h | dirs
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -fo=$@ $<

#-------------------------------------------------------------------------------
# Clean
#-------------------------------------------------------------------------------

clean:
	rm -f $(OBJDIR)/*.obj $(OBJDIR)/*.lnk
	rm -f $(BINDIR)/$(TARGET).exe $(BINDIR)/$(TARGET).map
	rm -f $(BINDIR)/keytest.exe

# Like clean, but also removes the build directories. bin/igc.exe is
# tracked in git, so only obj/ is removed wholesale.
distclean: clean
	rm -rf $(OBJDIR)
	-rmdir $(BINDIR) 2>/dev/null

#-------------------------------------------------------------------------------
# Keytest utility (separate build)
#-------------------------------------------------------------------------------

.PHONY: keytest

keytest: dirs $(BINDIR)/keytest.exe
	@echo "Build complete: $(BINDIR)/keytest.exe"

$(BINDIR)/keytest.exe: $(OBJDIR)/keytest.obj
	@echo "Linking keytest.exe..."
	$(LD) NAME $@ SYSTEM dos OPTION quiet FILE $(OBJDIR)/keytest.obj

$(OBJDIR)/keytest.obj: $(SRCDIR)/keytest.c | dirs
	@echo "Compiling keytest.c..."
	$(CC) $(CFLAGS) -fo=$@ $<

#-------------------------------------------------------------------------------
# Deploy to MAME disk image
#-------------------------------------------------------------------------------

MAME_DIR ?= /root/bldmame1
DISK_IMG ?= $(MAME_DIR)/vichd31.img
DEST_PARTITION ?= 1
DEST_DIR ?=

# Destination path inside the image ("\FOO\IGC.EXE", or "\IGC.EXE" when
# DEST_DIR is empty - avoid a doubled backslash)
ifeq ($(strip $(DEST_DIR)),)
DEST_PATH = \\$(TARGET).EXE
else
DEST_PATH = \\$(DEST_DIR)\\$(TARGET).EXE
endif

.PHONY: deploy deploy-keytest

deploy: $(BINDIR)/$(TARGET).exe
	@test -d $(MAME_DIR) || { \
		echo "MAME_DIR=$(MAME_DIR) not found - run with MAME_DIR=<path>"; \
		exit 1; }
	@echo "Deploying to MAME disk image..."
	cd $(MAME_DIR) && python3 -m vtg_image_util copy \
		$(CURDIR)/$(BINDIR)/$(TARGET).exe \
		$(DISK_IMG):$(DEST_PARTITION):$(DEST_PATH)
	@echo "Deployed to $(DISK_IMG):$(DEST_PARTITION):$(DEST_PATH)"

deploy-keytest: $(BINDIR)/keytest.exe
	@test -d $(MAME_DIR) || { \
		echo "MAME_DIR=$(MAME_DIR) not found - run with MAME_DIR=<path>"; \
		exit 1; }
	@echo "Deploying keytest to MAME disk image..."
	cd $(MAME_DIR) && python3 -m vtg_image_util copy \
		$(CURDIR)/$(BINDIR)/keytest.exe \
		$(DISK_IMG):$(DEST_PARTITION):\\KEYTEST.EXE
