Run `protoc`: 
```
protoc -I . --grpc_out=. --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` ./measure.proto
protoc -I . --cpp_out=. ./measure.proto 
```

Build using `cmake`:
```
mkdir -p cmake/build
pushd cmake/build
cmake -DCMAKE_PREFIX_PATH=$MY_INSTALL_DIR ../..
make -j 4
```

If protobuf and gRPC are not already installed locally, you can have CMake fetch
them:
```
mkdir -p cmake/build
pushd cmake/build
cmake -DGRPC_FETCHCONTENT=ON ../..
make -j 4
```

Run the server:
```
./measure_server --port=50051 --http_port=8080 --samples_retained=500
```

Run one or more clients with different IDs:
```
./measure_client --target=localhost:50051 --client_id=client1
./measure_client --target=localhost:50051 --client_id=client2
```

View the graph in a browser:
```
http://127.0.0.1:8080/
```

Notes:
- The graph UI is only exposed on localhost.
- Each client is shown in a different colour.
- The page refreshes measurement data every 2 seconds.
- The server keeps a bounded in-memory window of recent measurements for the graph and also appends `timestamp client_id point` to `result.txt`.
