// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The credential sidecar. No
// network: what is tested is that the token is stored the way a credential has
// to be, because the token alone is enough to act as the player on
// RetroAchievements.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "cheevos/cheevos_client.h"

using namespace ds;

namespace {

std::string make_dir() {
  char tmpl[] = "/tmp/dsperate_cheevos_credsXXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  return std::string(d);
}

void remove_dir(const std::string& dir) {
  ::unlink((dir + "/cheevos.token").c_str());
  ::unlink((dir + "/cheevos.token.tmp").c_str());
  ::rmdir(dir.c_str());
}

void no_file_is_not_an_error() {
  const std::string dir = make_dir();
  cheevos::Credentials c;
  std::string err;
  // Never signed in. That is a normal state, not a failure, and conflating the
  // two would make a first run look broken.
  CHECK(cheevos::load_credentials(dir, c, err));
  CHECK(err.empty());
  CHECK(c.empty());
  remove_dir(dir);
}

void a_token_round_trips() {
  const std::string dir = make_dir();
  std::string err;
  cheevos::Credentials in;
  in.username = "someplayer";
  in.token = "AbCdEf0123456789";
  CHECK(cheevos::save_credentials(dir, in, err));
  CHECK(err.empty());

  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(err.empty());
  CHECK(!out.empty());
  CHECK(out.username == in.username);
  CHECK(out.token == in.token);
  remove_dir(dir);
}

// The token is a credential. 0600 from creation, not chmod'ed afterwards: a
// token another user could read even briefly is a token to treat as leaked.
void the_file_is_owner_only() {
  const std::string dir = make_dir();
  std::string err;
  cheevos::Credentials in{"someplayer", "AbCdEf0123456789"};
  CHECK(cheevos::save_credentials(dir, in, err));

  struct stat st{};
  CHECK(::stat((dir + "/cheevos.token").c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  remove_dir(dir);
}

// Saving twice must leave one good file, not a stale temporary beside it.
void saving_again_replaces_it() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(cheevos::save_credentials(dir, {"first", "token-one"}, err));
  CHECK(cheevos::save_credentials(dir, {"second", "token-two"}, err));

  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(out.username == "second");
  CHECK(out.token == "token-two");
  struct stat st{};
  CHECK(::stat((dir + "/cheevos.token.tmp").c_str(), &st) != 0);   // no leftovers
  remove_dir(dir);
}

void signing_out_removes_it() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(cheevos::save_credentials(dir, {"someplayer", "tok"}, err));
  cheevos::clear_credentials(dir);
  cheevos::Credentials out;
  CHECK(cheevos::load_credentials(dir, out, err));
  CHECK(out.empty());
  remove_dir(dir);
}

// The file is line-based, so a newline in either field would silently truncate
// or shift the other one. Refuse rather than write a file we cannot read back.
void a_newline_is_refused() {
  const std::string dir = make_dir();
  std::string err;
  CHECK(!cheevos::save_credentials(dir, {"some\nplayer", "tok"}, err));
  CHECK(!err.empty());
  CHECK(!cheevos::save_credentials(dir, {"someplayer", "to\nk"}, err));
  CHECK(!cheevos::save_credentials(dir, {"", "tok"}, err));
  CHECK(!cheevos::save_credentials(dir, {"someplayer", ""}, err));
  remove_dir(dir);
}

// A half-written file (one line only) must not come back as usable
// credentials, or we would try to sign in with an empty token every launch.
void an_incomplete_file_is_rejected() {
  const std::string dir = make_dir();
  std::FILE* f = std::fopen((dir + "/cheevos.token").c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("someplayer\n", f);
  std::fclose(f);

  cheevos::Credentials out;
  std::string err;
  CHECK(!cheevos::load_credentials(dir, out, err));
  CHECK(!err.empty());
  CHECK(out.empty());
  remove_dir(dir);
}

// Importing the sign-in the CFW's front end already made. Both file shapes,
// and one hard rule: the password sitting next to the token in those files is
// never read.
void the_cfw_sign_in_is_imported() {
  const std::string dir = make_dir();

  // ROCKNIX / batocera shape: bare key=value.
  const std::string es = dir + "/system.cfg";
  std::FILE* f = std::fopen(es.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("global.retroachievements=1\n"
             "global.retroachievements.username=someplayer\n"
             "global.retroachievements.password=hunter2secret\n"
             "global.retroachievements.token=0123456789abcdef\n"
             "global.retroachievements.hardcore=1\n", f);
  std::fclose(f);

  cheevos::Credentials c;
  CHECK(cheevos::read_cfw_credentials(es, c));
  CHECK(c.username == "someplayer");
  CHECK(c.token == "0123456789abcdef");
  // The password is in that file and must not have been picked up anywhere.
  CHECK(c.token.find("hunter2") == std::string::npos);
  CHECK(c.username.find("hunter2") == std::string::npos);

  // RetroArch shape: spaces and quotes.
  const std::string ra = dir + "/retroarch.cfg";
  f = std::fopen(ra.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("cheevos_username = \"otherplayer\"\n"
             "cheevos_password = \"alsosecret\"\n"
             "cheevos_token = \"fedcba9876543210\"\n", f);
  std::fclose(f);
  CHECK(cheevos::read_cfw_credentials(ra, c));
  CHECK(c.username == "otherplayer");
  CHECK(c.token == "fedcba9876543210");

  // Signed out, or never signed in: RetroArch leaves the keys present and
  // empty, which must read as "nothing here" rather than as a blank token we
  // would then try to sign in with.
  const std::string empty = dir + "/empty.cfg";
  f = std::fopen(empty.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("cheevos_username = \"\"\ncheevos_token = \"\"\n", f);
  std::fclose(f);
  CHECK(!cheevos::read_cfw_credentials(empty, c));
  CHECK(c.empty());

  // A username with no token is not usable either.
  const std::string half = dir + "/half.cfg";
  f = std::fopen(half.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("global.retroachievements.username=someplayer\n", f);
  std::fclose(f);
  CHECK(!cheevos::read_cfw_credentials(half, c));

  CHECK(!cheevos::read_cfw_credentials(dir + "/not-there.cfg", c));

  // The probe honours the override, which is also how this is testable.
  ::setenv("DS_CHEEVOS_CFW_CONFIG", es.c_str(), 1);
  std::string source;
  CHECK(cheevos::import_cfw_credentials(c, source));
  CHECK(source == es);
  CHECK(c.token == "0123456789abcdef");
  ::unsetenv("DS_CHEEVOS_CFW_CONFIG");

  ::unlink(es.c_str()); ::unlink(ra.c_str()); ::unlink(empty.c_str()); ::unlink(half.c_str());
  remove_dir(dir);
}

// --cheevos-token: PPSSPP writes the token alone with no trailing newline,
// and our own two-line file has to keep working through the same reader.
void a_token_file_is_read() {
  const std::string dir = make_dir();
  cheevos::Credentials c;
  std::string err;

  const std::string bare = dir + "/ppsspp_retroachievements.dat";
  std::FILE* f = std::fopen(bare.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("0123456789abcdef", f);        // no newline, exactly as PPSSPP's helper writes it
  std::fclose(f);
  CHECK(cheevos::read_token_file(bare, c, err));
  CHECK(c.username.empty());                 // the caller supplies it from the config
  CHECK(c.token == "0123456789abcdef");

  const std::string ours = dir + "/two-line";
  f = std::fopen(ours.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("someplayer\nfedcba9876543210\n", f);
  std::fclose(f);
  CHECK(cheevos::read_token_file(ours, c, err));
  CHECK(c.username == "someplayer");
  CHECK(c.token == "fedcba9876543210");

  // An empty file and a missing one are both failures with a reason: the
  // player named this file, so silence would leave them guessing.
  const std::string empty = dir + "/empty";
  f = std::fopen(empty.c_str(), "w");
  CHECK(f != nullptr);
  std::fputs("\n  \n", f);
  std::fclose(f);
  CHECK(!cheevos::read_token_file(empty, c, err));
  CHECK(!err.empty());
  CHECK(!cheevos::read_token_file(dir + "/not-there", c, err));
  CHECK(!err.empty());

  ::unlink(bare.c_str()); ::unlink(ours.c_str()); ::unlink(empty.c_str());
  remove_dir(dir);
}

} // namespace

int main() {
  no_file_is_not_an_error();
  a_token_round_trips();
  the_file_is_owner_only();
  saving_again_replaces_it();
  signing_out_removes_it();
  a_newline_is_refused();
  an_incomplete_file_is_rejected();
  the_cfw_sign_in_is_imported();
  a_token_file_is_read();
  std::printf("cheevos_client: ok\n");
  return 0;
}
