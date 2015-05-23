#!/bin/bash

if [ "$#" -ne 1 ]; then
    echo "build expects 1 argument found" $#
    exit 1
fi

cd $1

bmake depend
if [ $? -eq 0 ]; then
    bmake
fi
if [ $? -eq 0 ]; then
    bmake install
fi

cd -
