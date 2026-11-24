#include "ayu/ui/ayu_screen_reader.h"

#include "lang/lang_keys.h"

#include <QAccessible>
#include <QWidget>

namespace Ayu::ScreenReader {

void Announce(QWidget *widget, const QString &text, bool assertive) {
	if (!widget || text.isEmpty()) {
		return;
	}
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0) || defined(QT_ACCESSIBLE_ANNOUNCEMENT_EVENT)
	auto event = QAccessibleAnnouncementEvent(widget, text);
	if (assertive) {
		event.setPoliteness(QAccessible::AnnouncementPoliteness::Assertive);
	}
	QAccessible::updateAccessibility(&event);
#else // Qt < 6.8
	(void)assertive;
#endif // Qt >= 6.8
}

QString Position(int index, int count) {
	return tr::lng_sr_position(
		tr::now,
		lt_index,
		QString::number(index),
		lt_total,
		QString::number(count));
}

} // namespace Ayu::ScreenReader
