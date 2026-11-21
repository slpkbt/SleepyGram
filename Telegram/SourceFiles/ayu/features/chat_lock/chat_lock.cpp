#include "ayu/features/chat_lock/chat_lock.h"

#include "ayu/features/chat_lock/chat_pin_input.h"

#include "base/random.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_auth_key.h"
#include "settings/cloud_password/settings_cloud_password_common.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/fields/password_input.h"
#include "window/window_session_controller.h"
#include "styles/style_settings.h"
#include "styles/style_passcode_box.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <QApplication>
#include <QEvent>
#include <QStringList>
#include <QStringView>
#include <QTimer>

namespace Ayu::ChatLock {
namespace {

constexpr auto kPinSaltSize = 16;
constexpr auto kMaximumFailures = 5;
constexpr auto kMaximumBackoff = crl::time(60'000);
constexpr auto kInactivityTimeout = 5 * 60 * 1000;

struct PinMaterial final {
	QByteArray salt;
	QByteArray hash;
	Storage::details::PasscodeKdf kdf;
};

struct State final {
	std::set<PeerId> protectedPeers;
	std::set<PeerId> unlockedPeers;
	std::unique_ptr<QTimer> timer;
	int failures = 0;
	crl::time blockedUntil = 0;
	bool verifying = false;
	bool loaded = false;
};

class Manager final : public QObject {
public:
	static Manager &Instance() {
		static auto manager = Manager();
		return manager;
	}

	State &state(uint64 accountId) {
		auto &result = _states[accountId];
		if (!result.timer) {
			result.timer = std::make_unique<QTimer>();
			result.timer->setSingleShot(true);
			connect(result.timer.get(), &QTimer::timeout, this, [=] {
				LockAccount(accountId);
			});
		}
		return result;
	}

	void load(not_null<Main::Session*> session) {
		auto &value = state(session->userId().bare);
		if (value.loaded) {
			return;
		}
		value.loaded = true;
		const auto peers = session->local().readPref<QString>(
			"ayu.chatPin.protected",
			QString());
		for (const auto &part : peers.split(u',', Qt::SkipEmptyParts)) {
			bool ok = false;
			const auto raw = part.toULongLong(&ok);
			if (ok && raw) {
				value.protectedPeers.insert(PeerId(PeerIdHelper{ raw }));
			}
		}
	}

	bool hasPin(not_null<Main::Session*> session) const {
		return !session->local().readPref<QString>(
			"ayu.chatPin.hash",
			QString()).isEmpty();
	}

	void fire(uint64 accountId, PeerId peerId) {
		_changes.fire({ .accountId = accountId, .peerId = peerId });
	}

	void startTimer(uint64 accountId) {
		if (_activeAccountId != accountId) {
			return;
		}
		state(accountId).timer->start(kInactivityTimeout);
	}

	void noteActivity() {
		if (!_activeAccountId) {
			return;
		}
		const auto i = _states.find(_activeAccountId);
		if (i != end(_states) && !i->second.unlockedPeers.empty()) {
			startTimer(_activeAccountId);
		}
	}

	void lockPeer(uint64 accountId, PeerId peerId) {
		const auto i = _states.find(accountId);
		if (i == end(_states) || !i->second.unlockedPeers.erase(peerId)) {
			return;
		}
		if (i->second.unlockedPeers.empty()) {
			i->second.timer->stop();
		}
		fire(accountId, peerId);
	}

	void lockAccount(uint64 accountId) {
		const auto i = _states.find(accountId);
		if (i == end(_states) || i->second.unlockedPeers.empty()) {
			return;
		}
		i->second.unlockedPeers.clear();
		i->second.timer->stop();
		fire(accountId, PeerId());
	}

	void switchAccount(uint64 previous, uint64 next) {
		if (previous) {
			lockAccount(previous);
		}
		if (next) {
			lockAccount(next);
		}
		_activeAccountId = next;
	}

	void unlockPeer(not_null<Main::Session*> session, PeerId peerId) {
		const auto id = session->userId().bare;
		auto &value = state(id);
		value.unlockedPeers.insert(peerId);
		value.failures = 0;
		value.blockedUntil = 0;
		value.verifying = false;
		startTimer(id);
		fire(id, peerId);
	}

	bool failed(not_null<Main::Session*> session) {
		auto &value = state(session->userId().bare);
		value.verifying = false;
		const auto now = crl::now();
		if (value.blockedUntil > now) {
			return true;
		}
		value.failures = std::min(value.failures + 1, kMaximumFailures);
		if (value.failures == kMaximumFailures) {
			const auto multiplier = (value.failures - kMaximumFailures + 1);
			value.blockedUntil = now + std::min(
				crl::time(5'000 * multiplier),
				kMaximumBackoff);
		}
		return false;
	}

	bool blocked(not_null<Main::Session*> session) {
		const auto &value = state(session->userId().bare);
		return value.verifying || (value.blockedUntil > crl::now());
	}

	bool beginVerification(not_null<Main::Session*> session) {
		auto &value = state(session->userId().bare);
		if (value.verifying || value.blockedUntil > crl::now()) {
			return false;
		}
		value.verifying = true;
		return true;
	}

	void endVerification(uint64 accountId) {
		state(accountId).verifying = false;
	}

	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::KeyPress
			|| event->type() == QEvent::MouseButtonPress
			|| event->type() == QEvent::TouchBegin) {
			noteActivity();
		}
		return QObject::eventFilter(watched, event);
	}

	rpl::producer<Change> changes() const {
		return _changes.events();
	}

private:
	Manager() {
		if (qApp) {
			qApp->installEventFilter(this);
		}
	}

	std::map<uint64, State> _states;
	uint64 _activeAccountId = 0;
	rpl::event_stream<Change> _changes;
};

struct Derivation final {
	QByteArray passcode;
	PinMaterial material;
	QByteArray result;
	bool matched = false;

	void run() {
		const auto key = Storage::details::CreatePasscodeKey(
			passcode,
			material.salt,
			material.kdf);
		if (key) {
			const auto bytes = key->data();
			result = QByteArray(
				reinterpret_cast<const char*>(bytes.data()),
				int(bytes.size()));
			if (!material.hash.isEmpty()) {
				matched = (result.size() == material.hash.size())
					&& (CRYPTO_memcmp(
						result.constData(),
						material.hash.constData(),
						size_t(result.size())) == 0);
			}
		}
		OPENSSL_cleanse(passcode.data(), size_t(passcode.size()));
	}
};

PinMaterial ReadPinMaterial(not_null<Main::Session*> session) {
	const auto kdfText = session->local().readPref<QString>(
		"ayu.chatPin.kdf",
		QString());
	const auto parts = kdfText.split(u',');
	if (parts.size() != 4) {
		return {};
	}
	bool ok = true;
	const auto read = [&](int index) {
		bool current = false;
		const auto value = parts[index].toUInt(&current);
		ok = ok && current;
		return value;
	};
	const auto kdf = Storage::details::PasscodeKdf{
		.kind = read(0),
		.memory = read(1),
		.time = read(2),
		.parallel = read(3),
	};
	const auto salt = QByteArray::fromBase64(session->local().readPref<QString>(
		"ayu.chatPin.salt",
		QString()).toUtf8());
	const auto hash = QByteArray::fromHex(session->local().readPref<QString>(
		"ayu.chatPin.hash",
		QString()).toLatin1());
	if (!ok || !kdf.valid() || !kdf.costWithinLimits()
		|| salt.size() < Storage::details::kPasscodeSaltMinSize
		|| hash.isEmpty()) {
		return {};
	}
	return { salt, hash, kdf };
}

void RunDerivation(Derivation job, Fn<void(Derivation)> done) {
	crl::async([job = std::move(job), done = std::move(done)]() mutable {
		job.run();
		crl::on_main([job = std::move(job), done = std::move(done)]() mutable {
			done(std::move(job));
		});
	});
}

} // namespace

bool HasPin(not_null<Main::Session*> session) {
	return Manager::Instance().hasPin(session);
}

bool IsPeerProtected(not_null<Main::Session*> session, PeerId peerId) {
	auto &manager = Manager::Instance();
	manager.load(session);
	return manager.state(session->userId().bare).protectedPeers.contains(peerId);
}

bool IsChatLocked(not_null<Main::Session*> session, PeerId peerId) {
	if (!IsPeerProtected(session, peerId)) {
		return false;
	}
	if (!HasPin(session)) {
		return true;
	}
	return !Manager::Instance().state(session->userId().bare).unlockedPeers.contains(peerId);
}

namespace {

void SetPin(
		not_null<Main::Session*> session,
		QString pin,
		Fn<bool()> canCommit,
		Fn<void(bool)> done) {
	auto salt = QByteArray(kPinSaltSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	auto job = Derivation{
		.passcode = pin.toUtf8(),
		.material = {
			.salt = salt,
			.kdf = Storage::details::DefaultPasscodeKdf(),
		},
	};
	pin.fill(QChar(0));
	const auto weak = base::make_weak(session.get());
	RunDerivation(std::move(job), [=](Derivation result) mutable {
		const auto strong = weak.get();
		if (!canCommit()) {
			return;
		}
		if (!strong || HasPin(strong) || result.result.isEmpty()) {
			done(false);
			return;
		}
		const auto kdf = result.material.kdf;
		strong->local().writePref<QString>(
			"ayu.chatPin.salt",
			QString::fromLatin1(result.material.salt.toBase64()));
		strong->local().writePref<QString>(
			"ayu.chatPin.hash",
			QString::fromLatin1(result.result.toHex()));
		strong->local().writePref<QString>(
			"ayu.chatPin.kdf",
			QStringLiteral("%1,%2,%3,%4")
				.arg(kdf.kind)
				.arg(kdf.memory)
				.arg(kdf.time)
				.arg(kdf.parallel));
		done(true);
	});
}

void VerifyPin(
		not_null<Main::Session*> session,
		PeerId peerId,
		QString pin,
		Fn<bool()> canCommit,
		Fn<void(bool)> done) {
	auto &manager = Manager::Instance();
	manager.load(session);
	if (manager.blocked(session)) {
		done(false);
		return;
	}
	if (!manager.beginVerification(session)) {
		done(false);
		return;
	}
	auto material = ReadPinMaterial(session);
	if (material.hash.isEmpty() || pin.isEmpty()) {
		manager.failed(session);
		done(false);
		return;
	}
	auto job = Derivation{ .passcode = pin.toUtf8(), .material = material };
	pin.fill(QChar(0));
	const auto weak = base::make_weak(session.get());
	const auto accountId = session->userId().bare;
	RunDerivation(std::move(job), [=](Derivation result) mutable {
		const auto strong = weak.get();
		if (!strong || !canCommit()) {
			Manager::Instance().endVerification(accountId);
			return;
		}
		if (!result.matched) {
			Manager::Instance().failed(strong);
			done(false);
			return;
		}
		Manager::Instance().unlockPeer(strong, peerId);
		done(true);
	});
}

} // namespace

bool SetPeerProtected(
		not_null<Main::Session*> session,
		PeerId peerId,
		bool protectedChat) {
	auto &manager = Manager::Instance();
	manager.load(session);
	auto &value = manager.state(session->userId().bare);
	if (protectedChat && !manager.hasPin(session)) {
		return false;
	}
	if (!protectedChat
		&& value.protectedPeers.contains(peerId)
		&& !value.unlockedPeers.contains(peerId)) {
		return false;
	}
	if (protectedChat) {
		value.protectedPeers.insert(peerId);
		value.unlockedPeers.erase(peerId);
	} else {
		value.protectedPeers.erase(peerId);
		value.unlockedPeers.erase(peerId);
	}
	auto ids = QStringList();
	for (const auto id : value.protectedPeers) {
		ids.push_back(QString::number(id.value));
	}
	session->local().writePref<QString>(
		"ayu.chatPin.protected",
		ids.join(u','));
	manager.fire(session->userId().bare, peerId);
	return true;
}

void LockPeer(uint64 accountId, PeerId peerId) {
	Manager::Instance().lockPeer(accountId, peerId);
}

void LockAccount(uint64 accountId) {
	Manager::Instance().lockAccount(accountId);
}

void OnAccountSwitch(uint64 previousAccountId, uint64 nextAccountId) {
	Manager::Instance().switchAccount(previousAccountId, nextAccountId);
}

void ShowUnlockDialog(
		not_null<Window::SessionController*> controller,
		PeerId peerId,
		Fn<void(bool)> done) {
	const auto weakController = base::make_weak(controller.get());
	const auto weakSession = base::make_weak(&controller->session());
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		using namespace ::Settings::CloudPassword;
		const auto weakBox = base::weak_qptr<Ui::BoxContent>(box.get());
		const auto state = std::make_shared<PinDialogState>();
		const auto canCommit = [=] {
			const auto controller = weakController.get();
			const auto session = weakSession.get();
			return !state->closed && weakBox && controller && session
				&& (&controller->session() == session);
		};
		box->setTitle(tr::lng_ayu_chat_pin_unlock());
		const auto content = box->verticalLayout();
		const auto field = AddPasswordField(
			content,
			tr::lng_passcode_enter_old(),
			QString());
		const auto error = AddError(content, field);
		box->setFocusCallback([=] { field->setFocusFast(); });
		box->boxClosing() | rpl::on_next([=] {
			state->closed = true;
			if (!state->completed && done) {
				done(false);
			}
		}, box->lifetime());
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		const auto submit = [=] {
			if (state->submitting || !canCommit()) {
				return;
			}
			const auto session = weakSession.get();
			if (field->text().isEmpty()) {
				field->showError();
				return;
			}
			if (Manager::Instance().blocked(session)) {
				error->setText(tr::lng_ayu_chat_pin_wait(tr::now));
				error->show();
				return;
			}
			if (!state->beginSubmit()) {
				return;
			}
			auto pin = field->text();
			field->clear();
			field->setDisabled(true);
			error->hide();
			VerifyPin(session, peerId, std::move(pin), canCommit, [=](bool success) {
				if (!canCommit()) {
					return;
				}
				state->submitting = false;
				if (success) {
					state->completed = true;
					weakBox.get()->closeBox();
					if (done) {
						done(true);
					}
				} else {
					field->setDisabled(false);
					field->showError();
					error->setText(tr::lng_passcode_wrong(tr::now));
					error->show();
				}
			});
		};
		QObject::connect(field.get(), &Ui::MaskedInputField::submitted, submit);
		box->addButton(tr::lng_continue(), submit);
	}));
}

void ShowProtectDialog(
		not_null<Window::SessionController*> controller,
		PeerId peerId) {
	const auto session = &controller->session();
	if (HasPin(session)) {
		if (IsPeerProtected(session, peerId)) {
			if (IsChatLocked(session, peerId)) {
				ShowUnlockDialog(controller, peerId, nullptr);
			} else {
				SetPeerProtected(session, peerId, false);
			}
		} else {
			SetPeerProtected(session, peerId, true);
		}
		return;
	}
	const auto weakController = base::make_weak(controller.get());
	const auto weakSession = base::make_weak(session);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		using namespace ::Settings::CloudPassword;
		const auto weakBox = base::weak_qptr<Ui::BoxContent>(box.get());
		const auto state = std::make_shared<PinDialogState>();
		const auto canCommit = [=] {
			const auto controller = weakController.get();
			const auto session = weakSession.get();
			return !state->closed && weakBox && controller && session
				&& (&controller->session() == session);
		};
		const auto content = box->verticalLayout();
		box->setTitle(tr::lng_ayu_chat_pin_protect());
		const auto first = AddPasswordField(
			content,
			tr::lng_passcode_enter_first(),
			QString());
		const auto second = AddPasswordField(
			content,
			tr::lng_passcode_confirm_new(),
			QString());
		const auto error = AddError(content, second);
		box->setFocusCallback([=] { first->setFocusFast(); });
		box->boxClosing() | rpl::on_next([=] {
			state->closed = true;
		}, box->lifetime());
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		const auto submit = [=] {
			if (state->submitting || !canCommit()) {
				return;
			}
			const auto inputError = CheckNewPin(
				QStringView(first->text()),
				QStringView(second->text()));
			if (inputError == PinInputError::Length) {
				first->showError();
				error->setText(tr::lng_ayu_chat_pin_length(tr::now));
				error->show();
				return;
			}
			if (inputError == PinInputError::Mismatch) {
				second->showError();
				error->setText(tr::lng_passcode_differ(tr::now));
				error->show();
				return;
			}
			if (!state->beginSubmit()) {
				return;
			}
			auto pin = first->text();
			first->clear();
			second->clear();
			first->setDisabled(true);
			second->setDisabled(true);
			error->hide();
			SetPin(weakSession.get(), std::move(pin), canCommit, [=](bool success) {
				if (!canCommit()) {
					return;
				}
				state->submitting = false;
				if (success && SetPeerProtected(weakSession.get(), peerId, true)) {
					if (const auto strongBox = weakBox.get()) {
						strongBox->closeBox();
					}
				} else {
					first->setDisabled(false);
					second->setDisabled(false);
					first->showError();
					error->setText(tr::lng_ayu_chat_pin_save_failed(tr::now));
					error->show();
				}
			});
		};
		QObject::connect(first.get(), &Ui::MaskedInputField::submitted, [=] {
			if (second->text().isEmpty()) {
				second->setFocusFast();
			} else {
				submit();
			}
		});
		QObject::connect(second.get(), &Ui::MaskedInputField::submitted, submit);
		box->addButton(tr::lng_continue(), submit);
	}));
}

rpl::producer<Change> Changes() {
	return Manager::Instance().changes();
}

} // namespace Ayu::ChatLock
