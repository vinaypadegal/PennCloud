#!/bin/bash
set -e

echo "=== Cloning repository ==="
git clone https://github.com/CIS5550/sp25-cis5050-T04.git
cd sp25-cis5050-T04/

echo "=== Installing dependencies ==="
sudo apt install -y build-essential cmake git curl autoconf libtool pkg-config \
  libssl-dev libprotobuf-dev protobuf-compiler dnsutils python3-pip

echo "=== GRPC setup  - will take time ==="
cd grpc
bash install2.sh

cd ..
echo "=== Creating backend data directories ==="
mkdir -p backend/checkpoints
mkdir -p backend/logs
mkdir -p backend/command_logs

echo "=== Build the project"
mkdir build
cd build
cmake ..
make -j$(nproc)

echo "=== Setup Complete, running the application. Run the following commands preferably in separate terminals"
echo "cd build"
echo "./coordinator > ../backend/logs/coordinator.log &
 ./worker 1 > ../backend/logs/worker1.txt & ./worker 2 > ../backend/logs/worker2.txt & ./worker 3 > ../backend/logs/worker3.txt & ./worker 4 > ../backend/logs/worker4.txt & ./worker 5 > ../backend/logs/worker5.txt & ./worker 6 > ../backend/logs/worker6.txt &
 ./http_server -p 8081 > logs/http_server1.log & ./http_server -p 8082 > logs/http_server2.log & ./http_server -p 8083 > logs/http_server3.log &
 ./load_balancer -p 8080 > logs/load_balancer.log &
 ./smtp_server -p 2500 -x -v > logs/smtp_server.log "

echo "== Alternative way to run - use the scripts"
echo "cd frontend/admin/test"
echo "python3 ./launchAllServers.py"
echo "python3 ./shutdownAllServers.py"