// This is the source code of AyuGram for Desktop.
#pragma once

#include "rpl/event_stream.h"
#include "rpl/producer.h"
#include "data/data_peer_id.h"
#include "base/basic_types.h"

namespace Main {
class Session;
}
namespace Window {
class SessionController;
}

namespace Ayu::ChatLock {

struct Change final {
	uint64 accountId = 0;
	PeerId peerId;
};

[[nodiscard]] bool HasPin(not_null<Main::Session*> session);
[[nodiscard]] bool IsPeerProtected(
	not_null<Main::Session*> session,
	PeerId peerId);
[[nodiscard]] bool IsChatLocked(
	not_null<Main::Session*> session,
	PeerId peerId);

bool SetPeerProtected(
	not_null<Main::Session*> session,
	PeerId peerId,
	bool protectedChat);
void LockPeer(uint64 accountId, PeerId peerId);
void LockAccount(uint64 accountId);
void OnAccountSwitch(uint64 previousAccountId, uint64 nextAccountId);
void ShowUnlockDialog(
	not_null<Window::SessionController*> controller,
	PeerId peerId,
	Fn<void(bool)> done = nullptr);
void ShowProtectDialog(
	not_null<Window::SessionController*> controller,
	PeerId peerId);

[[nodiscard]] rpl::producer<Change> Changes();

} // namespace Ayu::ChatLock
