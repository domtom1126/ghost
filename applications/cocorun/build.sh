#!/bin/bash
ROOT="../.."
if [ -f "$ROOT/variables.sh" ]; then
	. "$ROOT/variables.sh"
fi
. "$ROOT/ghost.sh"


# Define build setup
SRC=src
OBJ=obj
ARTIFACT_NAME=cocorun.bin
ARTIFACT_NAME_STATIC=cocorun-static.bin
CFLAGS="-std=c++11 -I$SRC"
LDFLAGS="-lcoconut"
MAKE_STATIC=1

# Include application build tasks
. "../applications.sh"
