#!/bin/bash

password="vm"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
echo "DIR: $DIR"
cd "$DIR" || exit

echo "====================================="
echo "Installing gRPC"

echo
echo "export MY_INSTALL_DIR=$HOME/.local"
export MY_INSTALL_DIR=$HOME/.local

echo
echo "mkdir -p $MY_INSTALL_DIR"
mkdir -p $MY_INSTALL_DIR

echo
echo "export PATH="$MY_INSTALL_DIR/bin:$PATH""
[[ ":$PATH:" != *":$MY_INSTALL_DIR/bin:"* ]] && export PATH="$MY_INSTALL_DIR/bin:$PATH"

echo
echo "sudo apt install -y cmake"
echo "$password" | sudo -S apt install -y cmake

echo
echo "sudo apt install -y build-essential autoconf libtool pkg-config libprotobuf-dev protobuf-compiler"
echo "$password" | sudo -S apt install -y build-essential autoconf libtool pkg-config libprotobuf-dev protobuf-compiler

echo
echo "sudo apt-get install libprotobuf-dev protobuf-compiler"
echo "$password" | sudo -S apt-get install libprotobuf-dev protobuf-compiler

echo
echo "cmake --version"
cmake --version

echo
if [ ! -d "grpc" ]; then
  echo
  echo "git clone --recurse-submodules -b v1.71.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc"
  git clone --recurse-submodules -b v1.71.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc

  echo
  echo "cd grpc"
  cd grpc

  echo
  echo "mkdir -p cmake/build"
  mkdir -p cmake/build

  echo
  echo "cd cmake/build"
  cd cmake/build

  echo
  echo "cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR ../.."
  cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR ../..

  echo 
  echo "sudo make"
  echo "$password" | sudo -S make
  
  echo 
  echo "sudo make install"
  echo "$password" | sudo -S make install
fi

echo
echo "gRPC Installation Successful"
echo "================="


