/*
This file is part of SleepyGram Desktop,
an unofficial client based on AyuGram Desktop and Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstddef>

namespace Ayu::ReplyLater {

inline constexpr auto kMaxEntriesPerAccount = std::size_t(250);

[[nodiscard]] constexpr bool CanScheduleReminder(
		std::size_t count,
		bool replacingExisting) {
	return replacingExisting || (count < kMaxEntriesPerAccount);
}

} // namespace Ayu::ReplyLater
