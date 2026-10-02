# Measure

`Measure` is a small gRPC-based measurement system:
- one server accepts measurements from multiple clients
- each client can subscribe to server-pushed threshold commands
- the server exposes a localhost-only graph page that shows recent measurements
- each client is drawn in a different colour on the graph

## Overview

Clients generate measurements and, in the current sample client, send values
above the active threshold to the server. The server stores every received
measurement in a bounded in-memory history, appends it to `result.txt`, and
serves a simple graph UI on localhost.

## Architecture

```mermaid
flowchart LR
  C1[Client: client1]
  C2[Client: client2]
  C3[Client: client3]
  S[gRPC Server<br/>measure_server]
  G[Local graph UI<br/>http://127.0.0.1:8080]
  F[result.txt]

  C1 -->|RecordMeasurement / Subscribe| S
  C2 -->|RecordMeasurement / Subscribe| S
  C3 -->|RecordMeasurement / Subscribe| S
  S -->|recent aggregated measurements| G
  S -->|append timestamp client_id point| F
```

## Build

### Option 1: use installed protobuf and gRPC

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$MY_INSTALL_DIR"
cmake --build build --parallel 4
```

### Option 2: let CMake fetch protobuf and gRPC

If protobuf and gRPC are not already installed locally:
```bash
cmake -S . -B build -DGRPC_FETCHCONTENT=ON
cmake --build build --parallel 4
```

CMake generates the protobuf and gRPC sources as part of the build.

## Tests

Catch2 v3 tests are available through CTest. Enable them when configuring:
```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

## Run

Start the server:
```bash
./build/measure_server --port=50051 --http_port=8080 --samples_retained=500
```

Start one or more clients with different IDs:
```bash
./build/measure_client --target=localhost:50051 --client_id=client1
./build/measure_client --target=localhost:50051 --client_id=client2
```

Open the graph page:
```text
http://127.0.0.1:8080/
```

## What the graph shows

- one combined graph for all connected clients
- one colour per client
- recent measurements only, bounded by `--samples_retained`
- periodic refresh every 2 seconds

The graph data comes from the server endpoint:
```text
http://127.0.0.1:8080/measurements.json
```

## Data and behavior notes

- The graph UI is only exposed on localhost.
- Each recorded measurement includes:
  - `point`
  - `client_id`
  - `timestamp_unix_ms`
- The server keeps a bounded in-memory history for the graph.
- The server also appends measurements to `result.txt` as:
  - `timestamp client_id point`
- Clients subscribe to threshold updates from the server using gRPC streaming.
