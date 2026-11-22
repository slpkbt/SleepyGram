/*
This file is part of SleepyGram Desktop,
an unofficial client based on AyuGram Desktop and Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

#include <QtCore/QString>
#include <vector>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::ReplyLater {

enum class Preset : uchar {
	InOneHour,
	ThisEvening,
	TomorrowMorning,
};

struct Entry {
	uint64 peerId = 0;
	int64 topicRootId = 0;
	uint64 sublistPeerId = 0;
	int64 msgId = 0;
	int64 dueAt = 0;
	int64 createdAt = 0;
	int64 firedAt = 0;
	QString preview;
	QString peerName;

	[[nodiscard]] bool matches(uint64 otherPeerId, int64 otherMsgId) const {
		return peerId == otherPeerId && msgId == otherMsgId;
	}
};

void Initialize();

[[nodiscard]] int64 ComputePresetTime(Preset preset);

[[nodiscard]] std::vector<Entry> LoadEntries(not_null<Main::Session*> session);
[[nodiscard]] bool HasEntry(
	not_null<Main::Session*> session,
	PeerId peerId,
	MsgId msgId);
[[nodiscard]] int CountDueEntries(not_null<Main::Session*> session);
[[nodiscard]] int CountAllEntries(not_null<Main::Session*> session);

[[nodiscard]] bool ScheduleItem(not_null<HistoryItem*> item, int64 dueAt);
void SnoozeEntry(
	not_null<Main::Session*> session,
	uint64 peerId,
	int64 msgId,
	int64 newDueAt);
void RemoveEntry(
	not_null<Main::Session*> session,
	uint64 peerId,
	int64 msgId);

void OpenEntry(
	not_null<Window::SessionController*> controller,
	const Entry &entry);
void ShowCustomTimeBox(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);
void ShowListBox(not_null<Window::SessionController*> controller);

[[nodiscard]] rpl::producer<not_null<Main::Session*>> Changes();

} // namespace Ayu::ReplyLater
