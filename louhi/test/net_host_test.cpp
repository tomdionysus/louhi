//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A task's interface on its node, and its firewall (design, "A task's network
// namespace"):
// - the host end is named from the task id, at most 15 characters, so a
//   restarted agent can tell whose it is;
// - attaching makes a veth pair, moves its far end into the sandbox's
//   namespace as eth0 with the task's address and a default route, and
//   routes the address to the host end (in the VPC's routing domain when
//   there is one), with proxy ARP so the task needs no gateway address;
// - every step runs in order; one that fails stops the rest, undoes what was
//   made, and says which step and why;
// - detaching removes the pair, and a pair not there is no error;
// - names, addresses and process ids are checked before anything runs;
// - the firewall applies its script with `nft -f -` and removes a table
//   whether or not it is there.

#include <gtest/gtest.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "louhi/net/firewall.h"
#include "louhi/net/host.h"

namespace louhi::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using Argv = std::vector<std::string>;

// Programs that answer what the test queued, else exit 0, each answer posted.
class FakeCommands final : public Commands {
 public:
  void run(Invocation invocation, runtime::Deadline, runtime::Executor executor, runtime::Completion<CommandResult> done) override {
    runtime::Result<CommandResult> answer = CommandResult{};
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _ran.push_back(invocation);
      if (!_answers.empty()) {
        answer = std::move(_answers.front());
        _answers.pop_front();
      }
    }
    asio::post(executor, [done = std::move(done), answer = std::move(answer)]() mutable { done(std::move(answer)); });
  }
  void queue(runtime::Result<CommandResult> answer) {
    std::lock_guard<std::mutex> lock(_mutex);
    _answers.push_back(std::move(answer));
  }
  std::vector<Invocation> ran() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _ran;
  }

 private:
  std::mutex _mutex;
  std::vector<Invocation> _ran;
  std::deque<runtime::Result<CommandResult>> _answers;
};

model::TaskId task() {
  model::TaskId id;
  for (size_t i = 0; i < id.bytes.size(); ++i) id.bytes[i] = static_cast<uint8_t>(0xa0 + i);
  return id;
}

TaskLink link(std::string vrf = "") {
  TaskLink l;
  l.host = host_interface(task());
  l.sandbox_pid = 4242;
  l.address = "10.0.1.5/24";
  l.vrf = std::move(vrf);
  return l;
}

struct NetHost : ::testing::Test {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};
  FakeCommands commands;
  IpHost host{commands};
  NftFirewall firewall{commands};

  ~NetHost() override {
    work.reset();
    io.stop();
    thread.join();
  }

  template <class Call>
  runtime::Result<runtime::Done> wait(Call call) {
    std::promise<runtime::Result<runtime::Done>> answer;
    auto future = answer.get_future();
    call([&answer](runtime::Result<runtime::Done> got) { answer.set_value(std::move(got)); });
    return future.get();
  }
};

TEST(NetHostNames, TheHostEndIsNamedFromTheTaskAndFitsTheKernel) {
  EXPECT_EQ(host_interface(task()), "lha0a1a2a3a4a5");
  EXPECT_LE(host_interface(task()).size(), 15u);
  EXPECT_EQ(task_of_interface("lha0a1a2a3a4a5"), "a0a1a2a3a4a5");
  EXPECT_FALSE(task_of_interface("eth0"));
  EXPECT_FALSE(task_of_interface("lhzz"));
}

TEST(NetHostPlan, AttachingMakesThePairAndRoutesTheAddressToIt) {
  const auto plan = attach_plan(link());
  const std::vector<Argv> expected{
      {"ip", "link", "add", "lha0a1a2a3a4a5", "type", "veth", "peer", "name", "lpa0a1a2a3a4a5"},
      {"ip", "link", "set", "lpa0a1a2a3a4a5", "netns", "4242"},
      {"nsenter", "--target", "4242", "--net", "ip", "link", "set", "lpa0a1a2a3a4a5", "name", "eth0"},
      {"nsenter", "--target", "4242", "--net", "ip", "address", "add", "10.0.1.5/24", "dev", "eth0"},
      {"nsenter", "--target", "4242", "--net", "ip", "link", "set", "lo", "up"},
      {"nsenter", "--target", "4242", "--net", "ip", "link", "set", "eth0", "up"},
      {"nsenter", "--target", "4242", "--net", "ip", "route", "add", "default", "dev", "eth0"},
      {"sysctl", "-w", "net.ipv4.conf.lha0a1a2a3a4a5.proxy_arp=1"},
      {"ip", "link", "set", "lha0a1a2a3a4a5", "up"},
      {"ip", "route", "add", "10.0.1.5/32", "dev", "lha0a1a2a3a4a5"},
  };
  EXPECT_EQ(plan, expected);
}

TEST(NetHostPlan, InAVpcsRoutingDomainTheHostEndAndItsRouteAreThere) {
  const auto plan = attach_plan(link("lv-main"));
  EXPECT_NE(std::find(plan.begin(), plan.end(), Argv{"ip", "link", "set", "lha0a1a2a3a4a5", "master", "lv-main"}), plan.end());
  EXPECT_EQ(plan.back(), (Argv{"ip", "route", "add", "10.0.1.5/32", "dev", "lha0a1a2a3a4a5", "vrf", "lv-main"}));
}

TEST(NetHostPlan, WhatItIsGivenIsCheckedFirst) {
  auto bad_address = link();
  bad_address.address = "10.0.1.5";
  EXPECT_THROW(attach_plan(bad_address), std::invalid_argument);
  bad_address.address = "10.0.1.5/24; reboot";
  EXPECT_THROW(attach_plan(bad_address), std::invalid_argument);
  auto bad_pid = link();
  bad_pid.sandbox_pid = 0;
  EXPECT_THROW(attach_plan(bad_pid), std::invalid_argument);
  auto bad_host = link();
  bad_host.host = "eth0";
  EXPECT_THROW(attach_plan(bad_host), std::invalid_argument) << "only a task's own interface is ever touched";
  EXPECT_THROW(attach_plan(link("Bad VRF")), std::invalid_argument);
  EXPECT_THROW(detach_plan("eth0"), std::invalid_argument);
}

TEST_F(NetHost, AttachRunsEveryStepInOrder) {
  const auto got = wait([&](auto done) { host.attach(link(), 1s, io.get_executor(), std::move(done)); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  const auto ran = commands.ran();
  const auto plan = attach_plan(link());
  ASSERT_EQ(ran.size(), plan.size());
  for (size_t i = 0; i < ran.size(); ++i) EXPECT_EQ(ran[i].argv, plan[i]);
}

TEST_F(NetHost, AStepThatFailsStopsTheRestUndoesThePairAndSaysWhy) {
  commands.queue(CommandResult{});
  commands.queue(CommandResult{1, "Cannot find device", false});
  const auto got = wait([&](auto done) { host.attach(link(), 1s, io.get_executor(), std::move(done)); });
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, runtime::Error::Kind::refused);
  EXPECT_NE(got.error().message.find("netns"), std::string::npos) << got.error().message;
  EXPECT_NE(got.error().message.find("Cannot find device"), std::string::npos) << got.error().message;
  const auto ran = commands.ran();
  ASSERT_EQ(ran.size(), 3u) << "two steps, then the undo";
  EXPECT_EQ(ran[2].argv, detach_plan("lha0a1a2a3a4a5").front());
}

TEST_F(NetHost, DetachRemovesThePairAndOneNotThereIsNoError) {
  commands.queue(CommandResult{1, "Cannot find device \"lha0a1a2a3a4a5\"", false});
  const auto got = wait([&](auto done) { host.detach("lha0a1a2a3a4a5", 1s, io.get_executor(), std::move(done)); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(commands.ran().at(0).argv, (Argv{"ip", "link", "delete", "lha0a1a2a3a4a5"}));
}

TEST_F(NetHost, TheFirewallAppliesItsScriptWithNft) {
  const std::string script = render(resolve("lha0a1a2a3a4a5", {}, {}));
  const auto got = wait([&](auto done) { firewall.apply(script, 1s, io.get_executor(), std::move(done)); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(commands.ran().at(0).argv, (Argv{"nft", "-f", "-"}));
  EXPECT_EQ(commands.ran().at(0).input, script);
}

TEST_F(NetHost, ARulesetNftRefusesIsRefusedWithItsWords) {
  commands.queue(CommandResult{1, "Error: syntax error", false});
  const auto got = wait([&](auto done) { firewall.apply("bad", 1s, io.get_executor(), std::move(done)); });
  ASSERT_FALSE(got.ok());
  EXPECT_NE(got.error().message.find("syntax error"), std::string::npos) << got.error().message;
}

TEST_F(NetHost, TheFirewallRemovesATableWhetherOrNotItIsThere) {
  const auto got = wait([&](auto done) { firewall.remove("louhi_lha0a1a2a3a4a5", 1s, io.get_executor(), std::move(done)); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(commands.ran().at(0).argv, (Argv{"nft", "-f", "-"}));
  EXPECT_EQ(commands.ran().at(0).input, "add table inet louhi_lha0a1a2a3a4a5\ndelete table inet louhi_lha0a1a2a3a4a5\n");
  EXPECT_THROW(firewall.remove("x; flush ruleset", 1s, io.get_executor(), [](auto) {}), std::invalid_argument);
}

}  // namespace
}  // namespace louhi::net
