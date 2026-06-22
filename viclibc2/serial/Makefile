# Victor 9000 Serial Communications Library
# Makefile for OpenWatcom v2 wmake
#
# Build Requirements:
#   source /opt/watcom/owsetenv.sh
#   export LIB=$WATCOM/lib286/dos
#
# Usage:
#   wmake           - Build library and test program
#   wmake lib       - Build library only
#   wmake clean     - Clean build artifacts

# OpenWatcom tools
CC = wcc
LIB = wlib
LINK = wlink

# Compiler flags for 8086 real mode, small model
# -ms  = Small memory model
# -0   = 8086 target
# -zq  = Quiet mode
# -s   = No stack checking
# -ox  = Maximum optimization
# -w4  = Warning level 4
# -zp1 = Pack structures to 1-byte alignment (match Python struct.pack)
CFLAGS = -ms -0 -zq -s -ox -w4 -zp1 -i=include

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = test
OBJ_DIR = obj

# Library name
LIBRARY = serial.lib

# Polled speed test
SPD_EXE = spdtest.exe

# Default target
all : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(SPD_EXE)

# Create object directory
$(OBJ_DIR) : .SYMBOLIC
	-mkdir $(OBJ_DIR)

# Build library only
lib : .SYMBOLIC $(OBJ_DIR) $(LIBRARY)

# Library depends on all object files
$(LIBRARY) : $(OBJ_DIR)/serial.obj $(OBJ_DIR)/serial_hw.obj &
             $(OBJ_DIR)/packet.obj
	$(LIB) -q -n $@ +$(OBJ_DIR)/serial.obj +$(OBJ_DIR)/serial_hw.obj &
		+$(OBJ_DIR)/packet.obj

# Compile source files
$(OBJ_DIR)/serial.obj : $(SRC_DIR)/serial.c $(INC_DIR)/serial.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/serial.c

$(OBJ_DIR)/serial_hw.obj : $(SRC_DIR)/serial_hw.c $(INC_DIR)/serial.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/serial_hw.c

$(OBJ_DIR)/packet.obj : $(SRC_DIR)/packet.c $(INC_DIR)/packet.h $(INC_DIR)/serial.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/packet.c

# Build polled speed test
spd : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(SPD_EXE)

$(OBJ_DIR)/spdtest.obj : $(TEST_DIR)/spdtest.c $(INC_DIR)/serial.h
	$(CC) $(CFLAGS) -fo=$@ $(TEST_DIR)/spdtest.c

$(SPD_EXE) : $(OBJ_DIR)/spdtest.obj $(LIBRARY)
	$(LINK) system dos name $@ file $(OBJ_DIR)/spdtest.obj library $(LIBRARY)

# Packet-layer receive test - drives pkt_recv_raw() (the asm hot loop) and needs
# the guard profile, so packet.c is recompiled here with -DPKT_PROFILE_GUARD and
# that object is linked in place of the library's default packet.obj.
PKT_EXE = pkttest.exe
PKTFLAGS = $(CFLAGS) -DPKT_PROFILE_GUARD

pkt : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(PKT_EXE)

$(OBJ_DIR)/packet_p.obj : $(SRC_DIR)/packet.c $(INC_DIR)/packet.h $(INC_DIR)/serial.h
	$(CC) $(PKTFLAGS) -fo=$@ $(SRC_DIR)/packet.c

$(OBJ_DIR)/pkttest.obj : $(TEST_DIR)/pkttest.c $(INC_DIR)/serial.h $(INC_DIR)/packet.h
	$(CC) $(PKTFLAGS) -fo=$@ $(TEST_DIR)/pkttest.c

$(PKT_EXE) : $(OBJ_DIR)/pkttest.obj $(OBJ_DIR)/serial.obj $(OBJ_DIR)/serial_hw.obj &
             $(OBJ_DIR)/packet_p.obj
	$(LINK) system dos name $@ file $(OBJ_DIR)/pkttest.obj &
		file $(OBJ_DIR)/serial.obj file $(OBJ_DIR)/serial_hw.obj &
		file $(OBJ_DIR)/packet_p.obj

# Clean build artifacts
clean : .SYMBOLIC
	-rm -f $(OBJ_DIR)/*.obj
	-rm -f $(LIBRARY)
	-rm -f $(SPD_EXE)
	-rm -f *.err
	-rm -f *.map
	-rmdir $(OBJ_DIR)
