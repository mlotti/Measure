#include <iostream>
#include <fstream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"

#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#ifdef BAZEL_BUILD
#include "measure.grpc.pb.h"
#else
#include "measure.grpc.pb.h"
#endif

#include <chrono>
#include <thread>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using measure::Measure;
using measure::Measurement;
using measure::Thumbs;

ABSL_FLAG(uint16_t, port, 50051, "Server port for the service");

void Write(int result) {
  std::ofstream myfile("result.txt", std::ios::app);
  auto t = std::hash<std::thread::id>{}(std::this_thread::get_id());
  if (myfile.is_open()) {
    myfile << result << " " << t <<  std::endl;
    myfile.close();
  }
}

// Logic and data behind the server's behavior.
class MeasureServiceImpl final : public Measure::CallbackService {
  grpc::ServerUnaryReactor* RecordMeasurement(grpc::CallbackServerContext* context,
					      const Measurement* request,
					      Thumbs* reply) override {
    int thumbsUp = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    Write(request->point());
    reply->set_response(thumbsUp);
    auto* reactor = context->DefaultReactor();
    reactor->Finish(Status::OK);
    return reactor;
  }
};

void RunServer(uint16_t port) {
  std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
  MeasureServiceImpl service;

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  // Register "service" as the instance through which we'll communicate with
  // clients. In this case it corresponds to an *synchronous* service.
  builder.RegisterService(&service);
  // Finally assemble the server.
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
}

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  RunServer(absl::GetFlag(FLAGS_port));
  return 0;
}
