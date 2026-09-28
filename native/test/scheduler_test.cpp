#include <vector>
#include "../src/scheduler.h"
#include "test_main.h"

using namespace webdsp;

static ScheduledTrigger makeEvent(int64_t atFrame, int32_t voiceId) {
  return {atFrame, voiceId, /*sampleId*/ 1, /*busId*/ 0, 1.0f, 1.0f, 0, -1, false, false};
}

static void testDrainOnlyReturnsEventsInBlockRange() {
  Scheduler s;
  s.push(makeEvent(150, 1));
  s.push(makeEvent(300, 2));
  s.push(makeEvent(500, 3));

  std::vector<int32_t> fired;
  s.drainDue(0, 128, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.empty());
  CHECK(s.pending() == 3);

  s.drainDue(128, 128, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.size() == 1);
  CHECK(fired[0] == 1);
  CHECK(s.pending() == 2);
}

static void testEventsFireInTimeOrderEvenIfPushedOutOfOrder() {
  Scheduler s;
  s.push(makeEvent(500, 3));
  s.push(makeEvent(100, 1));
  s.push(makeEvent(300, 2));

  std::vector<int32_t> fired;
  s.drainDue(0, 1000, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.size() == 3);
  CHECK(fired[0] == 1);
  CHECK(fired[1] == 2);
  CHECK(fired[2] == 3);
}

static void testCancelFromRemovesFutureEventsOnly() {
  Scheduler s;
  s.push(makeEvent(100, 1));
  s.push(makeEvent(300, 2));
  s.push(makeEvent(500, 3));

  s.cancelFrom(300);
  CHECK(s.pending() == 1);

  std::vector<int32_t> fired;
  s.drainDue(0, 1000, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.size() == 1);
  CHECK(fired[0] == 1);
}

static void testBoundaryIsHalfOpen() {
  Scheduler s;
  s.push(makeEvent(128, 1));  // exactly at block end -> belongs to the *next* block

  std::vector<int32_t> fired;
  s.drainDue(0, 128, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.empty());

  s.drainDue(128, 128, [&](const ScheduledTrigger& e) { fired.push_back(e.voiceId); });
  CHECK(fired.size() == 1);
}

int main() {
  testDrainOnlyReturnsEventsInBlockRange();
  testEventsFireInTimeOrderEvenIfPushedOutOfOrder();
  testCancelFromRemovesFutureEventsOnly();
  testBoundaryIsHalfOpen();

  std::printf("scheduler_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
