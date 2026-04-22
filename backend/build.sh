#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
echo "DIR: $DIR"
cd "$DIR" || exit

echo "====================================="
echo "Setting up Backend KV Store"

# echo
# echo "export MY_INSTALL_DIR=$HOME/.local"
# export MY_INSTALL_DIR=$HOME/.local

echo
echo "make clean"
make clean

echo
echo "cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR ."
cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR .

echo
echo "make"
make

echo
echo "Backend KV Store setup complete"
echo "================="
