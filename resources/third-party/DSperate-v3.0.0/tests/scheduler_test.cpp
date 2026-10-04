// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/nds.h"
#include "check.h"

using namespace ds;

static int fired = 0;
static u64 fired_at = 0;
static void on_event(NDS& nds, u32 param) { ++fired; fired_at = nds.sched.now(); CHECK(param == 7); }

int main() {
  NDS nds;
  nds.sched.schedule(EventId::Dma, 1000, on_event, 7);
  nds.run_frame();
  CHECK(fired == 1);
  CHECK(fired_at >= 1000);
  CHECK(nds.sched.now() >= CYCLES_PER_FRAME);
  CHECK(nds.frame_count == 1);
  std::puts("scheduler: ok");
  return 0;
}
