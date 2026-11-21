// This is the source code of AyuGram for Desktop.
#pragma once

#include <string_view>

namespace Ayu::ChatLock {

enum class PinInputError {
	None,
	Length,
	Mismatch,
};

[[nodiscard]] constexpr PinInputError CheckNewPin(
		std::u16string_view first,
		std::u16string_view confirmation) {
	if (first.size() < 4 || first.size() > 64) {
		return PinInputError::Length;
	}
	return (first == confirmation)
		? PinInputError::None
		: PinInputError::Mismatch;
}

struct PinDialogState final {
	bool closed = false;
	bool submitting = false;
	bool completed = false;

	[[nodiscard]] bool beginSubmit() {
		if (closed || submitting || completed) {
			return false;
		}
		submitting = true;
		return true;
	}
};

} // namespace Ayu::ChatLock
