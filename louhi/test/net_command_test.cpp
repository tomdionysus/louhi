//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Running a program (net/command.h; design, "A task's network namespace"):
// - a fixed argument list, never a shell, so an argument is never parsed;
// - its exit code and its output (stdout and stderr together), bounded,
//   with what was cut said;
// - input handed to it on stdin;
// - killed at its deadline, and said so;
// - a program that cannot be started fails, with why;
// - each answer posted to the caller's executor, once.
// Real programs that change nothing: echo, cat, false, sleep.

#include <gtest/gtest.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "louhi/net/command.h"

namespace louhi::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

struct NetCommand : ::testing::Test {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};
  ProcessCommands commands{io};

  ~NetCommand() override {
    work.reset();
    io.stop();
    thread.join();
  }

  runtime::Result<CommandResult> run(Invocation invocation, runtime::Deadline deadline = 5s) {
    std::promise<runtime::Result<CommandResult>> answer;
    auto future = answer.get_future();
    commands.run(std::move(invocation), deadline, io.get_executor(), [&answer](runtime::Result<CommandResult> got) { answer.set_value(std::move(got)); });
    return future.get();
  }
};

TEST_F(NetCommand, AnArgumentIsNeverParsedByAShell) {
  const auto got = run(Invocation{{"echo", "a; echo b", "$HOME", "'q'"}, ""});
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(got.value().exit_code, 0);
  EXPECT_EQ(got.value().output, "a; echo b $HOME 'q'\n");
  EXPECT_FALSE(got.value().truncated);
}

TEST_F(NetCommand, ItsExitCodeIsKept) {
  const auto got = run(Invocation{{"false"}, ""});
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_NE(got.value().exit_code, 0);
}

TEST_F(NetCommand, InputGoesToItsStdin) {
  const auto got = run(Invocation{{"cat"}, "table inet x {\n}\n"});
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(got.value().output, "table inet x {\n}\n");
}

TEST_F(NetCommand, OutputPastItsBoundIsCutAndSaid) {
  const std::string big(ProcessCommands::max_output + 4096, 'x');
  const auto got = run(Invocation{{"cat"}, big});
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(got.value().output.size(), ProcessCommands::max_output);
  EXPECT_TRUE(got.value().truncated);
  EXPECT_EQ(got.value().exit_code, 0) << "it ran to its end; only what is kept was cut";
}

TEST_F(NetCommand, AtItsDeadlineItIsKilled) {
  const auto started = std::chrono::steady_clock::now();
  const auto got = run(Invocation{{"sleep", "30"}, ""}, 300ms);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, runtime::Error::Kind::timed_out);
}

TEST_F(NetCommand, AProgramThatCannotBeStartedFailsSayingWhy) {
  const auto got = run(Invocation{{"louhi-no-such-program"}, ""});
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, runtime::Error::Kind::unavailable);
  EXPECT_NE(got.error().message.find("louhi-no-such-program"), std::string::npos) << got.error().message;
}

TEST_F(NetCommand, AnEmptyArgumentListIsRefused) {
  EXPECT_THROW(commands.run(Invocation{{}, ""}, 1s, io.get_executor(), [](auto) {}), std::invalid_argument);
}

}  // namespace
}  // namespace louhi::net
