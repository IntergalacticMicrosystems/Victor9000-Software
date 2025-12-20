#!/bin/bash
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"
make deploy
cd ../../bldmame1
#python3 -m vtg_image_util copy $SCRIPT_DIR/igc.com vichd.img:1:/temp/IGC.COM
cp mame-4x.ini mame.ini
./act victor9k -log -debug -debugger none -w -nomaximize -plugin claudecode -ramsize 512k -harddisk vichd.img

