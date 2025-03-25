#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"

#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "measure.grpc.pb.h"
#else
#include "measure.grpc.pb.h"
#endif

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using measure::Measure;
using measure::Measurement;
using measure::Thumbs;

class MeasureClient {
 public:
  MeasureClient(std::shared_ptr<Channel> channel)
      : stub_(Measure::NewStub(channel)) {}

  // Assembles the client's payload, sends it and presents the response back
  // from the server.
  int RecordMeasurement(const int& value) {
    // Data we are sending to the server.
    Measurement measurement;
    measurement.set_point(value);

    // Container for the data we expect from the server.
    Thumbs thumbs;

    // Context for the client. It could be used to convey extra information to
    // the server and/or tweak certain RPC behaviors.
    ClientContext context;

    // The actual RPC.
    Status status = stub_->RecordMeasurement(&context, measurement, &thumbs);

    // Act upon its status.
    if (status.ok()) {
      return thumbs.response();
    } else {
      std::cout << status.error_code() << ": " << status.error_message()
                << std::endl;
      return 0;
    }
  }

 private:
  std::unique_ptr<Measure::Stub> stub_;
};

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  // Instantiate the client. It requires a channel, out of which the actual RPCs
  // are created. This channel models a connection to an endpoint specified by
  // the argument "--target=" which is the only expected argument.
  std::string target_str = absl::GetFlag(FLAGS_target);
  // We indicate that the channel isn't authenticated (use of
  // InsecureChannelCredentials()).
  MeasureClient measure(
      grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));
  int value = 6;
  int reply = measure.RecordMeasurement(value);
  std::cout << "Measured: " << value << std::endl;
  if (reply==0) std::cout << "Recieved: thumbs up" << std::endl;
  if (reply==1) std::cout << "Recieved: thumbs down" << std::endl;
  return 0;
}
