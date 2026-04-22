# PennCloud

PennCloud is a distributed cloud platform built in C++ with a focus on scalability, fault tolerance, and low-latency performance. The system combines a replicated key-value backend with user-facing cloud services including file storage, webmail, authentication, and an admin console.

At its core is a coordinator-based distributed key-value store that supports:

- heartbeat-based worker health monitoring,
- deterministic failover to replicas,
- tablet partitioning and replication,
- in-memory serving with checkpoint + command-log recovery.

The broader platform includes:

- a custom multithreaded HTTP server,
- a load balancer with frontend health checks,
- a storage service with folder/file operations and chunked file handling,
- an email service plus SMTP server (including optional external relay),
- an admin dashboard for backend/frontend process control.

## Repository Layout

```text
.
├── CMakeLists.txt                # Main build entrypoint
├── setup.sh                      # End-to-end setup/build/run helper
├── backend/                      # Coordinator, workers, protobufs, host map
├── frontend/                     # HTTP server, load balancer, app services
│   ├── admin/                    # Admin dashboard + launch/shutdown scripts
│   ├── email/                    # Email service, SMTP server, tests
│   ├── storage/                  # File/folder web storage service
│   └── user_account/             # Signup/auth/password management
├── common/                       # Shared artifacts
├── utils/                        # Hashing, logging, serialization helpers
├── grpc/                         # gRPC installation scripts
└── web_storage_service/          # Legacy standalone storage prototype
```

## Architecture Overview

### 1) Backend: Distributed KV Store

- `coordinator` (`backend/coordinator.cc`)
  - listens on `127.0.0.1:10000`
  - maps keys to tablets via hashing
  - serves lookup RPCs for clients/services
  - tracks worker liveness using heartbeats
  - returns failover worker when a primary is down

- `worker` (`backend/worker.cc`)
  - one process per worker ID (`1..6`) on ports `10001..10006`
  - each worker stores 3 tablets: its own + two predecessor replicas
  - supports KV RPCs: `Put`, `Get`, `CPut`, `Delete`
  - replicates writes to follower workers
  - supports `CopyTablet` streaming for recovery and admin inspection

- Recovery + durability
  - periodic checkpointing to `backend/checkpoints/`
  - command logging to `backend/command_logs/`
  - restart path recovers from replica copy when possible, otherwise local checkpoint+log replay

### 2) Frontend: Cloud Services Layer

- `http_server` (`frontend/http_server.cc`)
  - multithreaded socket-based HTTP server
  - routes authentication, storage, email, admin, static pages, and health checks
  - session handling backed by KV (`session-ids` row)

- `load_balancer` (`frontend/load_balancer.cc`)
  - defaults to port `80` (commonly launched on `8080`)
  - probes frontends with `HEAD /health`
  - round-robin redirect (`307`) to live frontend instances (`8081`, `8082`, `8083`)
  - exposes `GET /health` JSON status for admin UI

- `user_account_service` (`frontend/user_account/user_account_service.cc`)
  - signup/login/password change
  - stores credentials in KV row pattern `<username>-userinfo`

- `storage_service` (`frontend/storage/storage_service.cc`)
  - hierarchical file manager over KV
  - create/move/rename/delete folders and files
  - upload/download file support with chunked storage/streaming
  - per-user base row pattern `<username>-storage`

- `email_service` (`frontend/email/email_service.cc`)
  - inbox/sent views, compose/send, read, reply, forward, delete
  - per-user mailbox row patterns: `inbox_email:<user>`, `sent_email:<user>`

- `smtp_server` (`frontend/email/smtp_server.cc`)
  - supports SMTP command flow: `HELO`, `MAIL FROM`, `RCPT TO`, `DATA`, `RSET`, `QUIT`
  - local delivery to KV-backed inboxes
  - optional external relay with MX lookup via `-x`

- `admin_service` (`frontend/admin/admin_service.cc`)
  - dashboard for worker/frontend health
  - backend toggles through worker admin RPC
  - frontend toggles through process commands
  - tablet data browsing via `CopyTablet` + deserialization

## Protocols and Ports

### Protocols

- gRPC + Protobuf for coordinator/worker and worker admin interfaces
- HTTP/1.1 for frontend app and load balancer
- SMTP for email transport

### Default Ports

- Coordinator: `10000`
- Workers: `10001` to `10006`
- Frontend app servers: `8081`, `8082`, `8083`
- Load balancer: `8080` (default code path is `80`)
- SMTP server: `2500`

### Configuration

- Worker host map: `backend/hosts.txt`

## Build and Setup

### Option A: Use `setup.sh` (quick path)

From repo root:

```bash
bash setup.sh
```

The script installs dependencies, builds with CMake, and prints commands to launch all services.

### Option B: Manual build

```bash
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

Built binaries include:

- `coordinator`
- `worker`
- `kvstore_client`
- `http_server`
- `load_balancer`
- `smtp_server`

## Running the System

From `build/`:

```bash
./coordinator > ../backend/logs/coordinator.log &
./worker 1 > ../backend/logs/worker1.txt &
./worker 2 > ../backend/logs/worker2.txt &
./worker 3 > ../backend/logs/worker3.txt &
./worker 4 > ../backend/logs/worker4.txt &
./worker 5 > ../backend/logs/worker5.txt &
./worker 6 > ../backend/logs/worker6.txt &

./http_server -p 8081 > logs/http_server1.log &
./http_server -p 8082 > logs/http_server2.log &
./http_server -p 8083 > logs/http_server3.log &
./load_balancer -p 8080 > logs/load_balancer.log &
./smtp_server -p 2500 -x -v > logs/smtp_server.log &
```

Then open:

- `http://localhost:8080` (through load balancer)

### Scripted launch/shutdown

From `frontend/admin/test/`:

```bash
python3 launchAllServers.py
python3 shutdownAllServers.py
```

## Testing

### Python tests (email/SMTP/integration)

Located under:

- `frontend/email/`
- `frontend/email/test/email_service_test/`
- `frontend/email/test/smtp_test/`

Examples:

```bash
python3 frontend/email/integration_test.py
python3 frontend/email/email_service_test.py
python3 frontend/email/test/smtp_test/test_smtp_sequence.py
python3 frontend/email/test/email_service_test/test_full_email_flow.py
```

### C++ tests

HTTP server test sources are in `frontend/test/` and `frontend/test/include/`.

## Notable Design Notes

- Data partitioning is hash-based over 6 tablets.
- Replication is write-through from primary to replicas.
- Worker recovery prefers remote tablet copy; falls back to local checkpoint/log replay.
- Admin console can inspect tablet contents through backend streaming RPC.

## Known Limitations / Caveats

- Metadata “encryption” in email/SMTP paths is placeholder/reversible.
- Leader election is not consensus-based; failover is coordinator-determined from liveness.
- Several operational assumptions are hardcoded (host/port layouts, worker count).
- `web_storage_service/` appears to be an older standalone prototype and is not part of the main CMake target graph.

## Team

- Vinay Padegal
- Ashay Katre
- Navya Saxena
- Adrija Mukherjee
