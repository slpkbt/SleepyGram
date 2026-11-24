#pragma once

#include <QtCore/QString>

class QWidget;

namespace Ayu::ScreenReader {

// Speaks a short message; a no-op where Qt lacks QAccessibleAnnouncementEvent.
void Announce(QWidget *widget, const QString &text, bool assertive = false);

// "3 of 12" for one painted element of a set, both numbers 1-based.
[[nodiscard]] QString Position(int index, int count);

} // namespace Ayu::ScreenReader
