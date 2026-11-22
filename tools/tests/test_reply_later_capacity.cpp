#include "ayu/features/reply_later/reply_later_capacity.h"

using Ayu::ReplyLater::CanScheduleReminder;
using Ayu::ReplyLater::kMaxEntriesPerAccount;

static_assert(CanScheduleReminder(0, false));
static_assert(CanScheduleReminder(kMaxEntriesPerAccount - 1, false));
static_assert(!CanScheduleReminder(kMaxEntriesPerAccount, false));
static_assert(!CanScheduleReminder(kMaxEntriesPerAccount + 1, false));
static_assert(CanScheduleReminder(kMaxEntriesPerAccount, true));
static_assert(CanScheduleReminder(kMaxEntriesPerAccount + 1, true));

int main() {
}
