#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "distcompile.grpc.pb.h"
#include "util.h"

namespace dc {

constexpr int kMaxMessage = 256 << 20;  // objects and binaries travel as single messages

inline std::unique_ptr<Coordinator::Stub> connect(const std::string& addr) {
  grpc::ChannelArguments args;
  args.SetMaxReceiveMessageSize(kMaxMessage);
  args.SetMaxSendMessageSize(kMaxMessage);
  return Coordinator::NewStub(grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args));
}

// Calls fn(ctx) until it succeeds or fails for a reason retrying won't fix.
// UNAVAILABLE (coordinator restarting, network blip) and DEADLINE_EXCEEDED are
// retried with exponential backoff. Only used for calls that are safe to repeat;
// they all are here, because every write is keyed by content or guarded in SQL.
template <class Fn>
grpc::Status with_retry(const char* what, Fn&& fn, int attempts = 8, int deadline_s = 60) {
  grpc::Status st;
  int backoff_ms = 100;
  for (int i = 0; i < attempts; i++) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(deadline_s));
    double t0 = now_ms();
    st = fn(ctx);
    stats().add(std::string("rpc.") + what, now_ms() - t0);
    if (st.ok()) return st;
    auto code = st.error_code();
    if (code != grpc::StatusCode::UNAVAILABLE && code != grpc::StatusCode::DEADLINE_EXCEEDED) return st;
    stats().add(std::string("rpc.") + what + ".retry", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms = std::min(backoff_ms * 2, 3000);
  }
  return st;
}

}  // namespace dc
