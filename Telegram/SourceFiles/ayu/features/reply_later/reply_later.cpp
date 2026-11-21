/*
This file is part of SleepyGram Desktop,
an unofficial client based on AyuGram Desktop and Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/features/reply_later/reply_later.h"

#include "ayu/features/chat_lock/chat_lock.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_saved_messages.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <algorithm>

namespace Ayu::ReplyLater {
namespace {

constexpr auto kPrefKey = std::string_view("sleepygram.replyLater.v1");
constexpr auto kMaxEntriesPerAccount = 250;
constexpr auto kPollIntervalMs = crl::time(15 * 1000);

rpl::event_stream<not_null<Main::Session*>> &ChangesStream() {
	static auto stream = rpl::event_stream<not_null<Main::Session*>>();
	return stream;
}

[[nodiscard]] int64 LocalNow() {
	return QDateTime::currentSecsSinceEpoch();
}

void SaveEntries(
		not_null<Main::Session*> session,
		const std::vector<Entry> &entries) {
	auto array = QJsonArray();
	for (const auto &entry : entries) {
		auto obj = QJsonObject();
		obj[u"peerId"_q] = QString::number(entry.peerId);
		obj[u"topicRootId"_q] = QString::number(entry.topicRootId);
		obj[u"sublistPeerId"_q] = QString::number(entry.sublistPeerId);
		obj[u"msgId"_q] = QString::number(entry.msgId);
		obj[u"dueAt"_q] = QString::number(entry.dueAt);
		obj[u"createdAt"_q] = QString::number(entry.createdAt);
		obj[u"firedAt"_q] = QString::number(entry.firedAt);
		obj[u"preview"_q] = entry.preview;
		obj[u"peerName"_q] = entry.peerName;
		array.append(obj);
	}
	if (array.isEmpty()) {
		session->local().clearPref(kPrefKey);
	} else {
		session->local().writePref<QByteArray>(
			kPrefKey,
			QJsonDocument(array).toJson(QJsonDocument::Compact));
	}
	ChangesStream().fire_copy(session);
}

[[nodiscard]] QString MakePreview(not_null<HistoryItem*> item) {
	auto text = item->originalText().text.simplified();
	if (text.isEmpty()) {
		text = item->notificationText().text.simplified();
	}
	if (text.size() > 140) {
		text = text.left(137) + u"..."_q;
	}
	return text;
}

[[nodiscard]] QString FormatDueTime(int64 dueAt, int64 now) {
	const auto dt = QDateTime::fromSecsSinceEpoch(dueAt);
	const auto today = QDate::currentDate();
	const auto dateStr = (dt.date() == today)
		? dt.time().toString(u"HH:mm"_q)
		: (dt.date() == today.addDays(1))
		? tr::lng_reply_later_tomorrow_at(
			tr::now,
			lt_time,
			dt.time().toString(u"HH:mm"_q))
		: dt.toString(u"dd.MM HH:mm"_q);
	if (dueAt <= now) {
		return tr::lng_reply_later_overdue(tr::now, lt_time, dateStr);
	}
	return dateStr;
}

void CheckDueReminders() {
	if (!Core::IsAppLaunched()
		|| Core::Quitting()
		|| !Core::App().domain().started()
		|| Core::App().passcodeLocked()) {
		return;
	}
	const auto now = LocalNow();
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!account) {
			continue;
		}
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto &windows = session->windows();
		if (windows.empty()) {
			continue;
		}
		auto entries = LoadEntries(session);
		auto due = std::vector<Entry>();
		for (auto &entry : entries) {
			if (entry.dueAt > 0 && entry.dueAt <= now && entry.firedAt == 0) {
				entry.firedAt = now;
				due.push_back(entry);
			}
		}
		if (due.empty()) {
			continue;
		}
		const auto primary = std::find_if(
			windows.begin(),
			windows.end(),
			[](const auto &window) { return window->isPrimary(); });
		const auto controller = (primary != windows.end())
			? primary->get()
			: windows.front().get();
		const auto weakController = base::make_weak(controller);
		if (due.size() > 1) {
			const auto openLink = tr::link(
				tr::semibold(tr::lng_reply_later_open(tr::now)));
			const auto text = tr::lng_reply_later_due_many(
				tr::now,
				lt_count,
				int(due.size()),
				lt_link,
				openLink,
				tr::marked);
			controller->showToast(Ui::Toast::Config{
				.text = text,
				.filter = [=](const auto &...) {
					if (const auto strong = weakController.get()) {
						ShowListBox(strong);
					}
					return false;
				},
				.duration = crl::time(8000),
			});
		} else {
			const auto entry = due.front();
			const auto locked = Ayu::ChatLock::IsChatLocked(
				session,
				PeerId(entry.peerId));
			const auto snippet = locked
				? tr::lng_ayu_chat_locked_preview(tr::now)
				: entry.preview;
			const auto title = entry.peerName.isEmpty()
				? tr::lng_reply_later_title(tr::now)
				: entry.peerName;
			const auto openLink = tr::link(
				tr::semibold(tr::lng_reply_later_open(tr::now)));
			auto text = tr::lng_reply_later_due_one(
				tr::now,
				lt_chat,
				tr::semibold(title),
				lt_text,
				tr::marked(snippet),
				lt_link,
				openLink,
				tr::marked);
			controller->showToast(Ui::Toast::Config{
				.text = std::move(text),
				.filter = [=](const auto &...) {
					if (const auto strong = weakController.get()) {
						OpenEntry(strong, entry);
					}
					return false;
				},
				.duration = crl::time(8000),
			});
		}
		SaveEntries(session, entries);
	}
}

base::Timer &PollTimer() {
	static base::Timer timer([] {
		CheckDueReminders();
	});
	return timer;
}

} // namespace

void Initialize() {
	PollTimer().callEach(kPollIntervalMs);
}

int64 ComputePresetTime(Preset preset) {
	const auto now = QDateTime::currentDateTime();
	switch (preset) {
	case Preset::InOneHour:
		return now.toSecsSinceEpoch() + 3600;
	case Preset::ThisEvening: {
		auto targetDate = now.date();
		if (now.time() >= QTime(19, 0)) {
			targetDate = targetDate.addDays(1);
		}
		return QDateTime(targetDate, QTime(19, 0)).toSecsSinceEpoch();
	}
	case Preset::TomorrowMorning:
		return QDateTime(
			now.date().addDays(1),
			QTime(9, 0)).toSecsSinceEpoch();
	}
	return now.toSecsSinceEpoch() + 3600;
}

std::vector<Entry> LoadEntries(not_null<Main::Session*> session) {
	const auto raw = session->local().readPref<QByteArray>(kPrefKey);
	if (raw.isEmpty()) {
		return {};
	}
	const auto doc = QJsonDocument::fromJson(raw);
	if (!doc.isArray()) {
		return {};
	}
	auto result = std::vector<Entry>();
	const auto arr = doc.array();
	result.reserve(arr.size());
	for (const auto &val : arr) {
		if (!val.isObject()) {
			continue;
		}
		const auto obj = val.toObject();
		auto entry = Entry();
		entry.peerId = obj.value(u"peerId"_q).toString().toULongLong();
		entry.topicRootId = obj.value(u"topicRootId"_q).toString().toLongLong();
		entry.sublistPeerId = obj.value(u"sublistPeerId"_q).toString().toULongLong();
		entry.msgId = obj.value(u"msgId"_q).toString().toLongLong();
		entry.dueAt = obj.value(u"dueAt"_q).toString().toLongLong();
		entry.createdAt = obj.value(u"createdAt"_q).toString().toLongLong();
		entry.firedAt = obj.value(u"firedAt"_q).toString().toLongLong();
		entry.preview = obj.value(u"preview"_q).toString();
		entry.peerName = obj.value(u"peerName"_q).toString();
		if (entry.peerId != 0 && entry.msgId != 0) {
			result.push_back(std::move(entry));
		}
	}
	std::sort(result.begin(), result.end(), [](const Entry &a, const Entry &b) {
		if (a.dueAt != b.dueAt) {
			return a.dueAt < b.dueAt;
		}
		return a.createdAt < b.createdAt;
	});
	return result;
}

bool HasEntry(
		not_null<Main::Session*> session,
		PeerId peerId,
		MsgId msgId) {
	const auto entries = LoadEntries(session);
	return std::any_of(entries.begin(), entries.end(), [&](const Entry &e) {
		return e.matches(peerId.value, msgId.bare);
	});
}

int CountDueEntries(not_null<Main::Session*> session) {
	const auto now = LocalNow();
	const auto entries = LoadEntries(session);
	return int(std::count_if(
		entries.begin(),
		entries.end(),
		[&](const Entry &e) { return e.dueAt <= now; }));
}

int CountAllEntries(not_null<Main::Session*> session) {
	return int(LoadEntries(session).size());
}

void ScheduleItem(not_null<HistoryItem*> item, int64 dueAt) {
	const auto session = &item->history()->session();
	const auto peer = item->history()->peer;
	auto entries = LoadEntries(session);
	entries.erase(
		std::remove_if(entries.begin(), entries.end(), [&](const Entry &e) {
			return e.matches(peer->id.value, item->id.bare);
		}),
		entries.end());
	if (entries.size() >= kMaxEntriesPerAccount) {
		entries.erase(entries.begin());
	}
	auto entry = Entry();
	entry.peerId = peer->id.value;
	entry.topicRootId = item->topicRootId().bare;
	entry.sublistPeerId = item->sublistPeerId().value;
	entry.msgId = item->id.bare;
	entry.dueAt = dueAt;
	entry.createdAt = LocalNow();
	entry.firedAt = 0;
	entry.preview = MakePreview(item);
	entry.peerName = peer->name();
	if (const auto topic = item->topic()) {
		if (!topic->title().isEmpty()) {
			entry.peerName += u" / "_q + topic->title();
		}
	}
	entries.push_back(std::move(entry));
	SaveEntries(session, entries);
}

void SnoozeEntry(
		not_null<Main::Session*> session,
		uint64 peerId,
		int64 msgId,
		int64 newDueAt) {
	auto entries = LoadEntries(session);
	for (auto &entry : entries) {
		if (entry.matches(peerId, msgId)) {
			entry.dueAt = newDueAt;
			entry.firedAt = 0;
			SaveEntries(session, entries);
			return;
		}
	}
}

void RemoveEntry(
		not_null<Main::Session*> session,
		uint64 peerId,
		int64 msgId) {
	auto entries = LoadEntries(session);
	const auto wasSize = entries.size();
	entries.erase(
		std::remove_if(entries.begin(), entries.end(), [&](const Entry &e) {
			return e.matches(peerId, msgId);
		}),
		entries.end());
	if (entries.size() != wasSize) {
		SaveEntries(session, entries);
	}
}

void OpenEntry(
		not_null<Window::SessionController*> controller,
		const Entry &entry) {
	const auto session = &controller->session();
	const auto peerId = PeerId(entry.peerId);
	const auto msgId = MsgId(entry.msgId);
	const auto peer = session->data().peer(peerId);
	if (entry.topicRootId != 0) {
		if (const auto topic = peer->forumTopicFor(MsgId(entry.topicRootId))) {
			controller->showTopic(
				topic,
				msgId,
				Window::SectionShow::Way::Forward);
			return;
		}
	}
	if (entry.sublistPeerId != 0) {
		const auto sublistPeer = session->data().peer(PeerId(entry.sublistPeerId));
		const auto sublist = session->data().savedMessages().sublist(sublistPeer);
		controller->showSublist(
			sublist,
			msgId,
			Window::SectionShow::Way::Forward);
		return;
	}
	controller->showPeerHistory(
		peerId,
		Window::SectionShow::Way::Forward,
		msgId);
}

void ShowCustomTimeBox(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto peerId = item->history()->peer->id;
	const auto msgId = item->id;
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto now = TimeId(LocalNow());
		Ui::ChooseDateTimeBox(box, {
			.title = tr::lng_reply_later_choose_time(),
			.submit = tr::lng_reply_later_set(),
			.done = [=](TimeId result) {
				if (const auto fresh = session->data().message(peerId, msgId)) {
					ScheduleItem(fresh, int64(result));
					controller->showToast(tr::lng_reply_later_saved(tr::now));
				}
				box->closeBox();
			},
			.min = [=] { return TimeId(LocalNow() + 60); },
			.time = now + 3600,
		});
	}));
}

void ShowListBox(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_reply_later_title());
		box->setWidth(st::boxWideWidth);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });

		const auto container = box->verticalLayout();
		const auto content = container->add(
			object_ptr<Ui::VerticalLayout>(container));

		const auto rebuild = [=] {
			while (content->count() > 0) {
				delete content->widgetAt(0);
			}
			const auto entries = LoadEntries(session);
			const auto now = LocalNow();
			if (entries.empty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						tr::lng_reply_later_empty(),
						st::boxLabel),
					st::boxPadding);
				return;
			}
			for (const auto &entry : entries) {
				const auto locked = Ayu::ChatLock::IsChatLocked(
					session,
					PeerId(entry.peerId));
				const auto title = entry.peerName.isEmpty()
					? tr::lng_reply_later_title(tr::now)
					: entry.peerName;
				const auto timeText = FormatDueTime(entry.dueAt, now);
				const auto snippet = locked
					? tr::lng_ayu_chat_locked_preview(tr::now)
					: entry.preview;

				const auto row = content->add(
					object_ptr<Ui::SettingsButton>(
						content,
						rpl::single(title + u" · "_q + timeText),
						st::settingsButtonNoIcon));
				row->setClickedCallback([=] {
					box->closeBox();
					OpenEntry(controller, entry);
				});

				if (!snippet.isEmpty()) {
					content->add(
						object_ptr<Ui::FlatLabel>(
							content,
							rpl::single(snippet),
							st::boxDividerLabel),
						QMargins(
							st::boxPadding.left(),
							0,
							st::boxPadding.right(),
							st::defaultVerticalListSkip));
				}

				const auto actions = content->add(
					object_ptr<Ui::RpWidget>(content));
				const auto snoozeBtn = Ui::CreateChild<Ui::LinkButton>(
					actions,
					tr::lng_reply_later_snooze_1h(tr::now),
					st::defaultLinkButton);
				const auto doneBtn = Ui::CreateChild<Ui::LinkButton>(
					actions,
					tr::lng_reply_later_done(tr::now),
					st::defaultLinkButton);
				snoozeBtn->setClickedCallback([=] {
					SnoozeEntry(
						session,
						entry.peerId,
						entry.msgId,
						ComputePresetTime(Preset::InOneHour));
				});
				doneBtn->setClickedCallback([=] {
					RemoveEntry(session, entry.peerId, entry.msgId);
				});
				const auto btnHeight = std::max(
					snoozeBtn->height(),
					doneBtn->height());
				actions->resize(actions->width(), btnHeight + st::defaultVerticalListSkip);
				snoozeBtn->moveToLeft(st::boxPadding.left(), 0);
				doneBtn->moveToLeft(
					st::boxPadding.left()
						+ snoozeBtn->width()
						+ st::boxPadding.left(),
					0);

				Ui::AddDivider(content);
			}
		};

		rebuild();

		rpl::merge(
			Changes() | rpl::filter([=](not_null<Main::Session*> changed) {
				return changed == session;
			}) | rpl::to_empty,
			Ayu::ChatLock::Changes() | rpl::to_empty
		) | rpl::on_next([=] {
			rebuild();
		}, box->lifetime());
	}));
}

rpl::producer<not_null<Main::Session*>> Changes() {
	return ChangesStream().events();
}

} // namespace Ayu::ReplyLater
