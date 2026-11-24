/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Core::Updates {

#ifdef Q_OS_MACOS
struct VerifiedUpdate;

enum class MacUpdatePreflight {
	Allowed,
	PolicyDenied,
	Failed,
};

[[nodiscard]] MacUpdatePreflight PreflightMacUpdate(
	const VerifiedUpdate &update,
	QString &error);
#endif // Q_OS_MACOS

[[nodiscard]] QString SleepyUpdateTransactionPath(const QString &executable);

[[nodiscard]] bool SleepyUpdateInstallationSupported(
	const QString &executable,
	const QString &workDir);

[[nodiscard]] bool SleepyUpdateProbeDirectoryValid(
	const QString &executable,
	const QString &directory);

[[nodiscard]] QString CreateSleepyUpdateHelperDirectory();

bool RemoveSleepyUpdateHelperDirectory(const QString &directory);

[[nodiscard]] int RunSleepyUpdater(const QStringList &args, QString &error);

} // namespace Core::Updates
