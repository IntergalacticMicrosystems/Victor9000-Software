# Victor 9000 File Transfer Library
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
CFLAGS = -ms -0 -zq -s -ox -w4 -zp1 -i=include -i=../serial/include

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = test
OBJ_DIR = obj

# Dependencies
SERIAL_LIB = ../serial/serial.lib

# Library name
LIBRARY = filetrx.lib

# Test program
TEST_EXE = ftxtest.exe

# File server program
SERV_EXE = ftxserv.exe

# Object files for library
LIB_OBJS = $(OBJ_DIR)/filetrx.obj &
           $(OBJ_DIR)/ftx_send.obj &
           $(OBJ_DIR)/ftx_recv.obj &
           $(OBJ_DIR)/ftx_server.obj &
           $(OBJ_DIR)/ftx_compress.obj &
           $(OBJ_DIR)/ftx_crc32.obj &
           $(OBJ_DIR)/ftx_dosfile.obj

# Default target
all : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(TEST_EXE) $(SERV_EXE)

# Create object directory
$(OBJ_DIR) : .SYMBOLIC
	-mkdir $(OBJ_DIR)

# Build library only
lib : .SYMBOLIC $(OBJ_DIR) $(LIBRARY)

# Library
$(LIBRARY) : $(LIB_OBJS)
	$(LIB) -q -n $@ +$(OBJ_DIR)/filetrx.obj &
		+$(OBJ_DIR)/ftx_send.obj &
		+$(OBJ_DIR)/ftx_recv.obj &
		+$(OBJ_DIR)/ftx_server.obj &
		+$(OBJ_DIR)/ftx_compress.obj &
		+$(OBJ_DIR)/ftx_crc32.obj &
		+$(OBJ_DIR)/ftx_dosfile.obj

# Compile source files
$(OBJ_DIR)/filetrx.obj : $(SRC_DIR)/filetrx.c $(INC_DIR)/filetrx.h &
                         $(INC_DIR)/ftx_protocol.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/filetrx.c

$(OBJ_DIR)/ftx_send.obj : $(SRC_DIR)/ftx_send.c $(INC_DIR)/filetrx.h &
                          $(INC_DIR)/ftx_compress.h $(INC_DIR)/ftx_crc32.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_send.c

$(OBJ_DIR)/ftx_recv.obj : $(SRC_DIR)/ftx_recv.c $(INC_DIR)/filetrx.h &
                          $(INC_DIR)/ftx_compress.h $(INC_DIR)/ftx_crc32.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_recv.c

$(OBJ_DIR)/ftx_server.obj : $(SRC_DIR)/ftx_server.c $(INC_DIR)/filetrx.h &
                            $(INC_DIR)/ftx_protocol.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_server.c

$(OBJ_DIR)/ftx_compress.obj : $(SRC_DIR)/ftx_compress.c $(INC_DIR)/ftx_compress.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_compress.c

$(OBJ_DIR)/ftx_crc32.obj : $(SRC_DIR)/ftx_crc32.c $(INC_DIR)/ftx_crc32.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_crc32.c

$(OBJ_DIR)/ftx_dosfile.obj : $(SRC_DIR)/ftx_dosfile.c $(INC_DIR)/ftx_dosfile.h
	$(CC) $(CFLAGS) -fo=$@ $(SRC_DIR)/ftx_dosfile.c

# Build test program
test : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(TEST_EXE)

$(OBJ_DIR)/ftxtest.obj : $(TEST_DIR)/ftxtest.c $(INC_DIR)/filetrx.h
	$(CC) $(CFLAGS) -fo=$@ $(TEST_DIR)/ftxtest.c

$(TEST_EXE) : $(OBJ_DIR)/ftxtest.obj $(LIBRARY) $(SERIAL_LIB)
	$(LINK) system dos name $@ file $(OBJ_DIR)/ftxtest.obj &
		library $(LIBRARY) library $(SERIAL_LIB)

# Build file server program
serv : .SYMBOLIC $(OBJ_DIR) $(LIBRARY) $(SERV_EXE)

$(OBJ_DIR)/ftxserv.obj : $(TEST_DIR)/ftxserv.c $(INC_DIR)/filetrx.h
	$(CC) $(CFLAGS) -fo=$@ $(TEST_DIR)/ftxserv.c

$(SERV_EXE) : $(OBJ_DIR)/ftxserv.obj $(LIBRARY) $(SERIAL_LIB)
	$(LINK) system dos name $@ file $(OBJ_DIR)/ftxserv.obj &
		library $(LIBRARY) library $(SERIAL_LIB)

# Clean build artifacts
clean : .SYMBOLIC
	-rm -f $(OBJ_DIR)/*.obj
	-rm -f $(LIBRARY)
	-rm -f $(TEST_EXE)
	-rm -f $(SERV_EXE)
	-rm -f *.err
	-rm -f *.map
	-rmdir $(OBJ_DIR)

# Show help
help : .SYMBOLIC
	@echo Victor 9000 File Transfer Library
	@echo ---------------------------------
	@echo Targets:
	@echo   all    - Build library and test program (default)
	@echo   lib    - Build library only
	@echo   test   - Build test program
	@echo   clean  - Remove build artifacts
	@echo   help   - Show this help
