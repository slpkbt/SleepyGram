#include "ayu/features/chat_lock/chat_pin_input.h"

#include <cassert>
#include <string>

using namespace Ayu::ChatLock;

static_assert(CheckNewPin(u"1234", u"1234") == PinInputError::None);
static_assert(CheckNewPin(u" 1234 ", u"1234") == PinInputError::Mismatch);
static_assert(CheckNewPin(u"1234", u"1235") == PinInputError::Mismatch);
static_assert(CheckNewPin(u"123", u"123") == PinInputError::Length);
static_assert(CheckNewPin(u"", u"") == PinInputError::Length);

int main() {
	const auto maxLength = std::u16string(64, u'a');
	const auto longPin = maxLength + u'a';
	assert(CheckNewPin(maxLength, maxLength) == PinInputError::None);
	assert(CheckNewPin(longPin, longPin) == PinInputError::Length);

	auto dialog = PinDialogState();
	assert(dialog.beginSubmit());
	assert(!dialog.beginSubmit());
	dialog.submitting = false;
	assert(dialog.beginSubmit());
	dialog.submitting = false;
	dialog.completed = true;
	assert(!dialog.beginSubmit());

	auto closed = PinDialogState();
	closed.closed = true;
	assert(!closed.beginSubmit());
}
