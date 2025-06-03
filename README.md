dummy change Run `protoc`: 
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
