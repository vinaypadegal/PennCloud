#!/bin/bash

password="vm"

echo "$password" | sudo -S apt update

echo "$password" | sudo -S apt install -y build-essential autoconf libtool pkg-config libsystemd-dev

echo "$password" | sudo -S apt install -y cmake

cd ~
git clone --recurse-submodules -b v1.67.1 --depth 1 --shallow-submodules https://github.com/grpc/grpc

cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      ../..

echo "$password" | sudo -S make -j$(nproc) install
