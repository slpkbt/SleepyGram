/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/sleepy_update_install.h"

#include <QtCore/QCoreApplication>

#include <cstdio>

#ifdef Q_OS_MACOS
#include <os/log.h>
#endif // Q_OS_MACOS

int main(int argc, char *argv[]) {
	const auto app = QCoreApplication(argc, argv);
	auto error = QString();
	const auto result = Core::Updates::RunSleepyUpdater(
		app.arguments().mid(1),
		error);
	if (!error.isEmpty()) {
		const auto bytes = error.toUtf8();
		fprintf(stderr, "SleepyGram update: %s\n", bytes.constData());
#ifdef Q_OS_MACOS
		// Finder launches do not have a terminal to retain detached stderr.
		// Only updater-owned diagnostic phrases and numeric status codes are reported.
		const auto log = os_log_create("com.sleepygram.desktop", "update");
		os_log_error(log, "%{public}s", bytes.constData());
#endif // Q_OS_MACOS
	}
	return result;
}
