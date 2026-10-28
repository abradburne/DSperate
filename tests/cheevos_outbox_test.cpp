// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The outbox of unconfirmed unlocks (cheevos_outbox.h). No network: what is tested
// is that an award rcheevos asks for is recognised, kept across a restart, kept
// once, and struck off only by an answer that settles it.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "cheevos/cheevos_outbox.h"
#include "rc_api_runtime.h"

using namespace ds;

namespace {

std::string make_dir() {
  char tmpl[] = "/tmp/dsperate_cheevos_outboxXXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  return std::string(d);
}

// The request body exactly as rcheevos builds it, so a change to its form fails here.
std::string award_body(const char* user, u32 id, const char* hash, u32 seconds_since_unlock) {
  rc_api_award_achievement_request_t params{};
  params.username = user;
  params.api_token = "SECRETTOKEN0123";
  params.achievement_id = id;
  params.hardcore = 0;
  params.game_hash = hash;
  params.seconds_since_unlock = seconds_since_unlock;
  rc_api_request_t request{};
  CHECK(rc_api_init_award_achievement_request(&request, &params) == RC_OK);
  std::string body = request.post_data ? request.post_data : "";
  rc_api_destroy_request(&request);
  return body;
}

void an_award_request_is_recognised() {
  cheevos::PendingUnlock p;
  CHECK(cheevos::parse_award_request(award_body("someplayer", 12345, "0123456789abcdef0123456789abcdef", 0), 1000, p));
  CHECK(p.user == "someplayer");
  CHECK(p.achievement == 12345);
  CHECK(p.hash == "0123456789abcdef0123456789abcdef");
  CHECK(p.unlocked_at == 1000);
  CHECK(!p.hardcore);

  // A retry says how long ago it was earned: the unlock time stays where it was.
  CHECK(cheevos::parse_award_request(award_body("someplayer", 12345, "ab", 30), 1000, p));
  CHECK(p.unlocked_at == 970);

  // Anything else rcheevos sends is not an award.
  CHECK(!cheevos::parse_award_request("r=ping&u=someplayer&t=tok&g=1", 1000, p));
  CHECK(!cheevos::parse_award_request("r=login2&u=someplayer&p=secret", 1000, p));
  CHECK(!cheevos::parse_award_request("", 1000, p));
  CHECK(!cheevos::parse_award_request("r=awardachievement&u=someplayer&a=0", 1000, p));
}

void unlocks_outlast_a_restart() {
  const std::string dir = make_dir();
  const std::string path = dir + "/cheevos.outbox";
  std::string err;
  {
    cheevos::Outbox box;
    CHECK(box.open(path, err));   // no file yet: an empty outbox
    CHECK(box.pending().empty());
    box.add({"someplayer", 111, "aaaa", 5000, false});
    box.add({"someplayer", 222, "aaaa", 5001, false});
    box.add({"SomePlayer", 111, "aaaa", 6000, false});   // the same unlock again: kept once
    CHECK(box.pending().size() == 2);
    CHECK(box.pending()[0].unlocked_at == 5000);
  }
  struct stat st{};
  CHECK(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);

  cheevos::Outbox again;
  CHECK(again.open(path, err));
  CHECK(again.pending().size() == 2);
  CHECK(again.pending()[1].user == "someplayer");
  CHECK(again.pending()[1].achievement == 222);
  CHECK(again.pending()[1].hash == "aaaa");
  CHECK(again.pending()[1].unlocked_at == 5001);

  // Struck off one by one; the last one takes the file with it.
  again.remove("someplayer", 111);
  CHECK(again.pending().size() == 1);
  again.remove("SOMEPLAYER", 222);
  CHECK(again.pending().empty());
  CHECK(::stat(path.c_str(), &st) != 0);
  CHECK(::stat((path + ".tmp").c_str(), &st) != 0);
  ::rmdir(dir.c_str());
}

// A damaged line costs that unlock, not the others; a file of another format is refused.
void a_damaged_file_is_read_as_far_as_it_goes() {
  const std::string dir = make_dir();
  const std::string path = dir + "/cheevos.outbox";
  std::FILE* f = std::fopen(path.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("dsperate-outbox 1\nsomeplayer\t111\taaaa\t5000\t0\nsomeplayer\tnotanumber\taaaa\t5000\t0\n"
             "someplayer\t333\tbbbb\t5002\t0\nhalf a line\n", f);
  std::fclose(f);
  cheevos::Outbox box;
  std::string err;
  CHECK(box.open(path, err));
  CHECK(box.pending().size() == 2);
  CHECK(box.pending()[1].achievement == 333);

  f = std::fopen(path.c_str(), "w");
  std::fputs("something else\n", f);
  std::fclose(f);
  CHECK(!box.open(path, err));
  CHECK(!err.empty());
  CHECK(!box.is_open());
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}

// Only an answer that settles the award strikes it off: none at all, a server
// error, or one a new token or version could change keeps it.
void only_a_settling_answer_strikes_it_off() {
  CHECK(cheevos::award_settled(200));
  CHECK(cheevos::award_settled(404));
  CHECK(cheevos::award_settled(400));
  CHECK(!cheevos::award_settled(0));
  CHECK(!cheevos::award_settled(-2));   // RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR
  CHECK(!cheevos::award_settled(500));
  CHECK(!cheevos::award_settled(503));
  CHECK(!cheevos::award_settled(401));
  CHECK(!cheevos::award_settled(403));
  CHECK(!cheevos::award_settled(429));
}

} // namespace

int main() {
  an_award_request_is_recognised();
  unlocks_outlast_a_restart();
  a_damaged_file_is_read_as_far_as_it_goes();
  only_a_settling_answer_strikes_it_off();
  std::printf("cheevos_outbox: ok\n");
  return 0;
}
