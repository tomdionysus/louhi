//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/verb.hpp>
#include <functional>
#include <memory>
#include <string>

#include "louhi/runtime/driver.h"

// HTTP/1.1 to a daemon on a local (Unix-domain) stream socket: what the Docker
// Engine API is spoken over. Dannan's streams are TCP only, so this is plain
// Asio and Beast (Dannan, 2026-11-15).
//
// One connection per request, closed after it: no pooling to go stale while
// the daemon restarts. Every request has a deadline, after which the
// connection is closed and the caller told so. A reply's header and body are
// bounded, and a stream's lines are bounded, because the reply is bytes from
// outside the process. Completions are posted to the caller's executor once,
// never inline.
namespace louhi::runtime::docker {

struct Reply {
  unsigned status = 0;
  std::string body;
};

class LocalHttp {
 public:
  // `io` runs the connections; the socket at `path` is connected afresh for
  // each request.
  LocalHttp(boost::asio::any_io_executor io, std::string path);

  // A request whose whole reply is read, at most max_reply_bytes. `body`, when
  // not empty, is sent as JSON. Fails with unavailable when the socket cannot
  // be reached or the connection breaks, timed_out at the deadline, malformed
  // for a reply that is not HTTP or is over its bound. Any status completes
  // the request: the caller judges it.
  void request(boost::beast::http::verb verb, std::string target, std::string body, Deadline deadline, Executor executor, Completion<Reply> done);

  // A request whose reply body is a stream of lines (a pull's progress, the
  // event stream). On a 200, each line, without its newline, is posted to
  // `line` in order, then `done` once with the status and an empty body when
  // the daemon ends the stream. On any other status, `done` has the status and
  // the body. A deadline of zero is none. A line over max_stream_line ends the
  // stream as malformed.
  class Stream {
   public:
    virtual ~Stream() = default;
    // Ends it: `done` is posted as unavailable, "cancelled", unless the stream
    // had already ended. No line is posted after. Thread-safe.
    virtual void cancel() = 0;
  };
  std::shared_ptr<Stream> stream(boost::beast::http::verb verb, std::string target, std::string body, Deadline deadline, Executor executor,
                                 std::function<void(std::string)> line, Completion<Reply> done);

 private:
  boost::asio::any_io_executor _io;
  std::string _path;
};

}  // namespace louhi::runtime::docker
