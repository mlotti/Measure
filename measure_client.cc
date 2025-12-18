#include <iostream>
#include <memory>
#include <string>
#include <random>

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

    std::mutex mu;
    int result;
    std::condition_variable cv;
    bool done = false;
    
    // The actual RPC.
    stub_->async()->RecordMeasurement(
        &context, &measurement, &thumbs,
	[&result, &mu, &cv, &done, thumbs, this](Status status) {
	    int ret;
	    if (!status.ok()) {
	      std::cout << "RecordMeasurement RPC failed" << std::endl;
	      ret = 0;
	    } else {
	      std::cout << "Measurement recieved" << std::endl;
	      ret = thumbs.response();
	    }
	    std::lock_guard<std::mutex> lock(mu);
	    result = ret;
	    done = true;
	    cv.notify_one();
	});
    std::cout << "waiting around" << std::endl;
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&done] {return done;});
    return result;
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

  // Set up random number generator for values between 0 and 10
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 10);

  int value = 0;
  int reply = 0;
  if (argc==1) {
    for (int i=0; i<10; i++) {
      value = dis(gen);
      std::cout << "Generated random number: " << value << std::endl;
      
      // Only send to server if value is above 8
      if (value > 8) {
        reply = measure.RecordMeasurement(value);
        std::cout << "Measured: " << value << std::endl;
        if (reply==0) std::cout << "Recieved: thumbs up" << std::endl;
        if (reply==1) std::cout << "Recieved: thumbs down" << std::endl;
      } else {
        std::cout << "Value " << value << " not sent (not above 8)" << std::endl;
      }
    }
  }
  if (argc>1) {
    value = std::stoi(argv[1]);
    reply = measure.RecordMeasurement(value);
    std::cout << "Measured: " << value << std::endl;
    if (reply==0) std::cout << "Recieved: thumbs up" << std::endl;
    if (reply==1) std::cout << "Recieved: thumbs down" << std::endl;
  }
  return 0;
}
