/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/dedicated_file_loader.h"

namespace Main {
class Session;
} // namespace Main

namespace Core {

bool UpdaterDisabled();
void SetUpdaterDisabledAtStartup();

class Updater;

class UpdateChecker {
public:
	enum class State {
		None,
		Available,
		Download,
		Ready,
		MacUpdateBlocked,
	};
	using Progress = MTP::AbstractDedicatedLoader::Progress;

	UpdateChecker();

	rpl::producer<> checking() const;
	rpl::producer<> isLatest() const;
	rpl::producer<Progress> progress() const;
	rpl::producer<> failed() const;
	rpl::producer<> ready() const;
	rpl::producer<> available() const;
	rpl::producer<> busy() const;
	rpl::producer<> macUpdateBlocked() const;

	void start(bool forceWait = false);
	void stop();
	void checkNow();
	void download();
	void test();

	void setMtproto(base::weak_ptr<Main::Session> session);

	State state() const;
	QString availableVersion() const;
	bool hasPackage() const;
	int already() const;
	int size() const;
	bool percent() const;

private:
	const std::shared_ptr<Updater> _updater;

};

bool checkReadyUpdate();
void UpdateApplication();
QString countAlphaVersionSignature(uint64 version);

} // namespace Core
