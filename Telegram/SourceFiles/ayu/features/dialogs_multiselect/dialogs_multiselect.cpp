#include "ayu/features/dialogs_multiselect/dialogs_multiselect.h"

#include "apiwrap.h"
#include "ayu/features/chat_lock/chat_lock.h"
#include "base/call_delayed.h"
#include "data/data_chat_filters.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_mark_as_read.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <algorithm>
#include <deque>
#include <memory>

namespace Ayu::DialogsMultiSelect {
namespace {

constexpr auto kArchiveChunkSize = 100;
constexpr auto kPacedActionDelayMs = 45;

struct PacedQueue {
	std::deque<PeerId> peerIds;
	Fn<void(not_null<History*>)> step;
};

void SchedulePacedPeerStep(
		not_null<Main::Session*> session,
		std::shared_ptr<PacedQueue> state,
		crl::time delay) {
	base::call_delayed(delay, session, [session, state = std::move(state)] {
		const auto peerId = state->peerIds.front();
		state->peerIds.pop_front();
		if (const auto history = session->data().historyLoaded(peerId)) {
			state->step(history);
		}
		if (!state->peerIds.empty()) {
			SchedulePacedPeerStep(session, state, kPacedActionDelayMs);
		}
	});
}

void RunPacedPeerQueue(
		not_null<Main::Session*> session,
		std::vector<PeerId> peerIds,
		Fn<void(not_null<History*>)> step) {
	if (peerIds.empty()) {
		return;
	}
	auto state = std::make_shared<PacedQueue>();
	state->peerIds.assign(peerIds.begin(), peerIds.end());
	state->step = std::move(step);
	SchedulePacedPeerStep(session, std::move(state), 0);
}

[[nodiscard]] bool CanToggleArchive(not_null<History*> history) {
	const auto peer = history->peer;
	return !peer->isSelf()
		&& !peer->isNotificationsUser()
		&& !peer->isRepliesChat();
}

void BulkToggleArchived(
		not_null<Window::SessionController*> controller,
		std::vector<PeerId> peerIds,
		bool archived) {
	const auto session = &controller->session();
	auto filtered = std::vector<PeerId>();
	filtered.reserve(peerIds.size());
	for (const auto peerId : peerIds) {
		if (const auto history = session->data().historyLoaded(peerId)) {
			if (CanToggleArchive(history)) {
				filtered.push_back(peerId);
			}
		}
	}
	if (filtered.empty()) {
		return;
	}

	const auto archiveId = Data::Folder::kId;
	const auto weakController = base::make_weak(controller);
	for (auto offset = std::size_t(0);
			offset < filtered.size();
			offset += kArchiveChunkSize) {
		const auto end = std::min(
			offset + kArchiveChunkSize,
			filtered.size());
		auto chunkIds = std::vector<PeerId>(
			filtered.begin() + offset,
			filtered.begin() + end);
		auto inputs = QVector<MTPInputFolderPeer>();
		inputs.reserve(int(chunkIds.size()));
		auto anyPinned = false;
		for (const auto peerId : chunkIds) {
			if (const auto history = session->data().historyLoaded(peerId)) {
				anyPinned = anyPinned || history->isPinnedDialog(0);
				inputs.push_back(MTP_inputFolderPeer(
					history->peer->input(),
					MTP_int(archived ? archiveId : 0)));
			}
		}
		if (inputs.isEmpty()) {
			continue;
		}
		session->api().request(MTPfolders_EditPeerFolders(
			MTP_vector<MTPInputFolderPeer>(std::move(inputs))
		)).done(crl::guard(session, [=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			for (const auto peerId : chunkIds) {
				if (const auto history = session->data().historyLoaded(peerId)) {
					if (archived) {
						history->setFolder(session->data().folder(archiveId));
					} else {
						history->clearFolder();
					}
				}
			}
			if (anyPinned) {
				session->data().notifyPinnedDialogsOrderUpdated();
			}
		})).fail(crl::guard(session, [=](const MTP::Error &error) {
			if (const auto strong = weakController.get()) {
				strong->showToast(error.type());
			}
		})).send();
	}
	controller->showToast((archived
		? tr::lng_dialogs_multiselect_archived_toast
		: tr::lng_dialogs_multiselect_unarchived_toast)(
			tr::now,
			lt_count,
			int(filtered.size())));
}

void BulkAddToFolder(
		not_null<Window::SessionController*> controller,
		FilterId filterId,
		std::vector<PeerId> peerIds) {
	const auto session = &controller->session();
	const auto &list = session->data().chatsFilters().list();
	const auto it = ranges::find(list, filterId, &Data::ChatFilter::id);
	if (it == end(list)) {
		return;
	}
	const auto was = *it;
	auto always = base::duplicate(was.always());
	auto pinned = was.pinned();
	auto never = base::duplicate(was.never());

	auto addedCount = 0;
	const auto limit = Data::PremiumLimits(session).dialogFiltersChatsCurrent();
	auto hitLimit = false;
	for (const auto peerId : peerIds) {
		const auto history = session->data().historyLoaded(peerId);
		if (!history || was.contains(history)) {
			continue;
		}
		const auto wasExcluded = never.remove(history);
		const auto probe = Data::ChatFilter(
			was.id(),
			was.title(),
			was.iconEmoji(),
			was.colorIndex(),
			was.flags(),
			base::duplicate(always),
			pinned,
			base::duplicate(never));
		if (!probe.contains(history)) {
			if (int(always.size()) >= limit) {
				hitLimit = true;
				if (wasExcluded) {
					never.insert(history);
				}
				continue;
			}
			always.insert(history);
		}
		++addedCount;
	}
	if (!addedCount) {
		if (hitLimit) {
			controller->showToast(
				tr::lng_dialogs_multiselect_folder_limit(tr::now));
		}
		return;
	}
	const auto updated = Data::ChatFilter(
		was.id(),
		was.title(),
		was.iconEmoji(),
		was.colorIndex(),
		was.flags(),
		std::move(always),
		std::move(pinned),
		std::move(never));
	session->data().chatsFilters().set(updated);
	const auto weakController = base::make_weak(controller);
	session->api().request(MTPmessages_UpdateDialogFilter(
		MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
		MTP_int(updated.id()),
		updated.tl()
	)).done(crl::guard(session, [=] {
		if (const auto strong = weakController.get()) {
			strong->showToast(tr::lng_dialogs_multiselect_added_folder_toast(
				tr::now,
				lt_count,
				addedCount));
		}
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		session->data().chatsFilters().set(was);
		if (const auto strong = weakController.get()) {
			strong->showToast(error.type());
		}
	})).send();
}

} // namespace

void FillMultiDialogsMenu(
		not_null<Window::SessionController*> controller,
		not_null<Ui::PopupMenu*> menu,
		std::vector<not_null<History*>> histories,
		Fn<void()> onDone) {
	if (histories.empty()) {
		return;
	}
	const auto session = &controller->session();
	auto peerIds = std::vector<PeerId>();
	peerIds.reserve(histories.size());
	auto anyUnmuted = false;
	auto anyNotArchived = false;
	auto anyUnread = false;
	for (const auto history : histories) {
		peerIds.push_back(history->peer->id);
		if (!history->muted()) {
			anyUnmuted = true;
		}
		if (CanToggleArchive(history) && !history->folder()) {
			anyNotArchived = true;
		}
		if (MarkAsReadMenu::IsUnreadThread(history)) {
			anyUnread = true;
		}
	}

	const auto count = int(peerIds.size());
	const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
	const auto weakController = base::make_weak(controller);

	addAction(
		(anyUnmuted
			? tr::lng_dialogs_multiselect_mute
			: tr::lng_dialogs_multiselect_unmute)(
				tr::now,
				lt_count,
				count),
		[=] {
			const auto muteValue = anyUnmuted
				? Data::MuteValue{ .forever = true }
				: Data::MuteValue{ .unmute = true };
			RunPacedPeerQueue(
				session,
				peerIds,
				[=](not_null<History*> history) {
					session->data().notifySettings().update(
						history->peer,
						muteValue);
				});
			if (const auto strong = weakController.get()) {
				strong->showToast((anyUnmuted
					? tr::lng_dialogs_multiselect_muted_toast
					: tr::lng_dialogs_multiselect_unmuted_toast)(
						tr::now,
						lt_count,
						count));
			}
			if (onDone) {
				onDone();
			}
		},
		anyUnmuted ? &st::menuIconMute : &st::menuIconUnmute);

	addAction(
		(anyNotArchived
			? tr::lng_dialogs_multiselect_archive
			: tr::lng_dialogs_multiselect_unarchive)(
				tr::now,
				lt_count,
				count),
		[=] {
			if (const auto strong = weakController.get()) {
				BulkToggleArchived(strong, peerIds, anyNotArchived);
			}
			if (onDone) {
				onDone();
			}
		},
		anyNotArchived ? &st::menuIconArchive : &st::menuIconUnarchive);

	const auto &filters = session->data().chatsFilters().list();
	auto realFilters = std::vector<std::pair<FilterId, QString>>();
	for (const auto &filter : filters) {
		if (filter.id() > 0 && !filter.chatlist()) {
			realFilters.emplace_back(filter.id(), filter.title().text.text);
		}
	}
	if (!realFilters.empty()) {
		addAction({
			.text = tr::lng_dialogs_multiselect_add_folder(tr::now),
			.icon = &st::menuIconAddToFolder,
			.fillSubmenu = [=](not_null<Ui::PopupMenu*> submenu) {
				for (const auto &[filterId, title] : realFilters) {
					submenu->addAction(title, [=] {
						if (const auto strong = weakController.get()) {
							BulkAddToFolder(strong, filterId, peerIds);
						}
						if (onDone) {
							onDone();
						}
					});
				}
			},
		});
	}

	if (anyUnread) {
		addAction(
			tr::lng_dialogs_multiselect_mark_read(tr::now, lt_count, count),
			[=] {
				auto unlockedUnread = std::vector<PeerId>();
				auto skippedLocked = 0;
				for (const auto peerId : peerIds) {
					const auto history = session->data().historyLoaded(peerId);
					if (!history || !MarkAsReadMenu::IsUnreadThread(history)) {
						continue;
					}
					if (Ayu::ChatLock::IsChatLocked(session, peerId)) {
						++skippedLocked;
						continue;
					}
					unlockedUnread.push_back(peerId);
				}
				if (unlockedUnread.empty()) {
					if (skippedLocked > 0) {
						if (const auto strong = weakController.get()) {
							strong->showToast(
								tr::lng_dialogs_multiselect_locked_skipped(
									tr::now,
									lt_count,
									skippedLocked));
						}
					}
					if (onDone) {
						onDone();
					}
					return;
				}
				RunPacedPeerQueue(
					session,
					unlockedUnread,
					[](not_null<History*> history) {
						MarkAsReadMenu::MarkAsReadThread(
							history,
							MarkAsReadMenu::MarkAsReadMuted::Include);
					});
				if (const auto strong = weakController.get()) {
					if (skippedLocked > 0) {
						strong->showToast(
							tr::lng_dialogs_multiselect_locked_skipped(
								tr::now,
								lt_count,
								skippedLocked));
					} else {
						strong->showToast(
							tr::lng_dialogs_multiselect_read_toast(
								tr::now,
								lt_count,
								int(unlockedUnread.size())));
					}
				}
				if (onDone) {
					onDone();
				}
			},
			&st::menuIconMarkRead);
	}

	addAction(
		tr::lng_dialogs_multiselect_clear(tr::now),
		[=] {
			if (onDone) {
				onDone();
			}
		},
		&st::menuIconClear);
}

} // namespace Ayu::DialogsMultiSelect
