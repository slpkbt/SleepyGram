/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/sleepy_update_install.h"

#include "core/update_channel.h"
#include "core/update_keys.h"
#include "core/update_unpack.h"
#include "core/update_verify.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QThread>
#include <QtCore/QUuid>

#include <array>
#include <limits>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#else // Q_OS_WIN
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/sysctl.h>
#endif // Q_OS_MACOS
#endif // Q_OS_WIN

namespace Core::Updates {
namespace {

constexpr auto kJournalMagic = quint32(0x53475531);
constexpr auto kMaxPackageSize = qint64(kMaxPayloadSize) + 1024 * 1024;
constexpr auto kMaxManifestSize = 256 * 1024 + 64;
constexpr auto kMaxJournalSize = 64 * 1024;
constexpr auto kParentTimeout = 45000;
constexpr auto kProbeTimeout = 60000;
constexpr auto kReleaseTimeout = 15000;
constexpr auto kPollInterval = 100;

constexpr auto kStateName = u".sleepy-update";
constexpr auto kTransactionName = u"transaction";
constexpr auto kJournalName = u"journal";
constexpr auto kStageName = u"stage";
constexpr auto kBackupName = u"backup";
constexpr auto kTempPrefix = u"sleepygram-updater-";

#ifdef Q_OS_MACOS
constexpr auto kBundleName = u"SleepyGram.app";
constexpr auto kExecutableName = u"Contents/MacOS/SleepyGram";
constexpr auto kUpdaterName = u"Contents/Frameworks/Updater";
#elif defined Q_OS_WIN
constexpr auto kExecutableName = u"SleepyGram.exe";
constexpr auto kUpdaterName = u"Updater.exe";
constexpr auto kAppFiles = std::array{
	u"SleepyGramRemovalHelper.exe",
	u"LICENSE-SQLCipher.txt",
	u"build-manifest.json",
	u"modules/x64/d3d/d3dcompiler_47.dll",
	kUpdaterName,
	kExecutableName,
};
constexpr auto kAppDirectories = std::array{
	u"modules",
	u"modules/x64",
	u"modules/x64/d3d",
};
#else // Q_OS_MACOS || Q_OS_WIN
constexpr auto kExecutableName = u"SleepyGram";
constexpr auto kUpdaterName = u"Updater";
constexpr auto kAppFiles = std::array{
	u"SleepyGramRemovalHelper",
	u"LICENSE-SQLCipher.txt",
	u"build-manifest.json",
	kUpdaterName,
	kExecutableName,
};
constexpr auto kAppDirectories = std::array<const char16_t*, 0>();
#endif // Q_OS_MACOS || Q_OS_WIN

enum class Kind {
	Missing,
	File,
	Directory,
	Unsafe,
};

struct Location {
	QString executable;
	QString root;
	QString bundle;
	QString state;
	QString transaction;
	QString stage;
	QString backup;
};

enum class Phase : quint8 {
	Staging,
	Prepared,
	Committed,
};

struct Journal {
	QString workDir;
	QString key;
	quint64 device = 0;
	quint64 inode = 0;
	quint32 files = 0;
	quint32 originals = 0;
	quint32 directories = 0;
	Phase phase = Phase::Staging;
};

[[nodiscard]] QString Join(const QString &base, QStringView name) {
	return QDir(base).filePath(name.toString());
}

[[nodiscard]] bool Fail(QString &error, const char *message) {
	error = QString::fromLatin1(message);
	return false;
}

[[nodiscard]] QString Absolute(QString path) {
	path = QDir::fromNativeSeparators(path);
	if (!QDir::isAbsolutePath(path)) {
		return {};
	}
	return QDir::cleanPath(path);
}

[[nodiscard]] bool IsWithin(const QString &base, const QString &path) {
	const auto prefix = base.endsWith(u'/') ? base : base + u'/';
#if defined Q_OS_WIN || defined Q_OS_MACOS
	constexpr auto sensitivity = Qt::CaseInsensitive;
#else // Q_OS_WIN || Q_OS_MACOS
	constexpr auto sensitivity = Qt::CaseSensitive;
#endif // Q_OS_WIN || Q_OS_MACOS
	return path.compare(base, sensitivity) == 0
		|| path.startsWith(prefix, sensitivity);
}

[[nodiscard]] std::optional<Location> GetLocation(const QString &path) {
	const auto executable = Absolute(path);
	if (executable.isEmpty()) {
		return std::nullopt;
	}
#ifdef Q_OS_MACOS
	const auto suffix = Join(QStringLiteral("/") + QStringView(kBundleName), kExecutableName);
	if (!executable.endsWith(suffix)) {
		return std::nullopt;
	}
	const auto bundle = executable.chopped(QStringView(kExecutableName).size() + 1);
	const auto root = QFileInfo(bundle).absolutePath();
#else // Q_OS_MACOS
	if (QFileInfo(executable).fileName() != QStringView(kExecutableName)) {
		return std::nullopt;
	}
	const auto bundle = QString();
	const auto root = QFileInfo(executable).absolutePath();
#endif // Q_OS_MACOS
	for (const auto &part : root.split(u'/', Qt::SkipEmptyParts)) {
		if (part.compare(u"tdata", Qt::CaseInsensitive) == 0) {
			return std::nullopt;
		}
	}
	const auto state = Join(root, kStateName);
	const auto transaction = Join(state, kTransactionName);
	return Location{
		executable,
		root,
		bundle,
		state,
		transaction,
		Join(transaction, kStageName),
		Join(transaction, kBackupName),
	};
}

#ifdef Q_OS_WIN

struct CloseHandleDeleter {
	void operator()(void *value) const {
		CloseHandle(value);
	}
};

struct LocalFreeDeleter {
	void operator()(void *value) const {
		LocalFree(value);
	}
};

using Handle = std::unique_ptr<void, CloseHandleDeleter>;
using LocalBuffer = std::unique_ptr<void, LocalFreeDeleter>;

[[nodiscard]] Handle FileHandle(HANDLE value) {
	return Handle(value != INVALID_HANDLE_VALUE ? value : nullptr);
}

[[nodiscard]] QByteArray CurrentUser() {
	auto raw = HANDLE();
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
		return {};
	}
	const auto token = Handle(raw);
	auto size = DWORD(0);
	const auto empty = GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
	if (empty || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) {
		return {};
	}
	auto storage = QByteArray(size, Qt::Uninitialized);
	if (!GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size)) {
		return {};
	}
	return storage;
}

[[nodiscard]] PSID UserSid(const QByteArray &user) {
	return reinterpret_cast<const TOKEN_USER*>(user.constData())->User.Sid;
}

[[nodiscard]] bool IsPrivateDirectory(const QString &path) {
	const auto user = CurrentUser();
	if (user.isEmpty()) {
		return false;
	}
	const auto native = QDir::toNativeSeparators(path);
	const auto entry = FileHandle(CreateFileW(
		reinterpret_cast<LPCWSTR>(native.utf16()),
		READ_CONTROL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	if (!entry) {
		return false;
	}
	auto owner = PSID();
	auto acl = PACL();
	auto descriptor = PSECURITY_DESCRIPTOR();
	const auto status = GetSecurityInfo(
		entry.get(),
		SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
		&owner,
		nullptr,
		&acl,
		nullptr,
		&descriptor);
	const auto guard = LocalBuffer(descriptor);
	if (status != ERROR_SUCCESS || !owner || !acl || !EqualSid(owner, UserSid(user))) {
		return false;
	}
	for (auto i = DWORD(0); i != acl->AceCount; ++i) {
		auto ace = LPVOID();
		if (!GetAce(acl, i, &ace)
			|| reinterpret_cast<PACE_HEADER>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE
			|| !EqualSid(&reinterpret_cast<PACCESS_ALLOWED_ACE>(ace)->SidStart, owner)) {
			return false;
		}
	}
	return acl->AceCount > 0;
}

[[nodiscard]] bool CreatePrivateDirectory(const QString &path) {
	const auto user = CurrentUser();
	if (user.isEmpty()) {
		return false;
	}
	auto access = EXPLICIT_ACCESSW();
	access.grfAccessPermissions = FILE_ALL_ACCESS;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(UserSid(user));
	auto acl = PACL();
	const auto status = SetEntriesInAclW(1, &access, nullptr, &acl);
	const auto guard = LocalBuffer(acl);
	auto descriptor = SECURITY_DESCRIPTOR();
	if (status != ERROR_SUCCESS
		|| !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !SetSecurityDescriptorOwner(&descriptor, UserSid(user), FALSE)
		|| !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
		|| !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
		return false;
	}
	auto attributes = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE };
	return CreateDirectoryW(
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
		&attributes);
}

[[nodiscard]] Kind PathKind(const QString &path) {
	const auto attrs = GetFileAttributesW(
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		const auto error = GetLastError();
		return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			? Kind::Missing
			: Kind::Unsafe;
	}
	if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
		return Kind::Unsafe;
	}
	return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? Kind::Directory : Kind::File;
}

[[nodiscard]] bool SyncFile(const QString &path) {
	const auto file = FileHandle(CreateFileW(
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT,
		nullptr));
	return file && FlushFileBuffers(file.get());
}

[[nodiscard]] bool SyncDirectory(const QString &) {
	return true;
}

[[nodiscard]] bool Rename(const QString &from, const QString &to, bool replace) {
	const auto flags = DWORD(MOVEFILE_WRITE_THROUGH)
		| (replace ? DWORD(MOVEFILE_REPLACE_EXISTING) : DWORD(0));
	return MoveFileExW(
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(from).utf16()),
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(to).utf16()),
		flags);
}

[[nodiscard]] bool BackupFile(const QString &source, const QString &backup) {
	return CreateHardLinkW(
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(backup).utf16()),
		reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()),
		nullptr);
}

[[nodiscard]] bool ProcessAlive(qint64 pid) {
	const auto process = Handle(OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid)));
	return process
		? (WaitForSingleObject(process.get(), 0) != WAIT_OBJECT_0)
		: (GetLastError() != ERROR_INVALID_PARAMETER);
}

#else // Q_OS_WIN

[[nodiscard]] Kind PathKind(const QString &path) {
	struct stat info = {};
	if (lstat(QFile::encodeName(path).constData(), &info) != 0) {
		return (errno == ENOENT) ? Kind::Missing : Kind::Unsafe;
	} else if (S_ISDIR(info.st_mode)) {
		return Kind::Directory;
	} else if (S_ISREG(info.st_mode)) {
		return Kind::File;
	}
	return Kind::Unsafe;
}

[[nodiscard]] bool IsPrivateDirectory(const QString &path) {
	struct stat info = {};
	return lstat(QFile::encodeName(path).constData(), &info) == 0
		&& S_ISDIR(info.st_mode)
		&& info.st_uid == geteuid()
		&& (info.st_mode & 07777) == 0700;
}

[[nodiscard]] bool CreatePrivateDirectory(const QString &path) {
	return mkdir(QFile::encodeName(path).constData(), 0700) == 0;
}

[[nodiscard]] bool SyncPath(const QString &path, int flags) {
	const auto fd = open(
		QFile::encodeName(path).constData(),
		O_RDONLY | O_NOFOLLOW | O_CLOEXEC | flags);
	if (fd < 0) {
		return false;
	}
	const auto synced = fsync(fd) == 0;
	return close(fd) == 0 && synced;
}

[[nodiscard]] bool SyncDirectory(const QString &path) {
	return SyncPath(path, O_DIRECTORY);
}

[[nodiscard]] bool SyncFile(const QString &path) {
	return SyncPath(path, 0);
}

[[nodiscard]] bool Rename(const QString &from, const QString &to, bool replace) {
	if (!replace && PathKind(to) != Kind::Missing) {
		return false;
	}
	return rename(
		QFile::encodeName(from).constData(),
		QFile::encodeName(to).constData()) == 0;
}

#ifndef Q_OS_MACOS
[[nodiscard]] bool BackupFile(const QString &source, const QString &backup) {
	return link(
		QFile::encodeName(source).constData(),
		QFile::encodeName(backup).constData()) == 0;
}
#endif // !Q_OS_MACOS

[[nodiscard]] bool ProcessAlive(qint64 pid) {
	if (kill(pid_t(pid), 0) != 0) {
		return errno != ESRCH;
	}
#ifndef Q_OS_MACOS
	// An unreaped Linux zombie still answers kill(pid, 0).
	auto process = QFile(QStringLiteral("/proc/%1/stat").arg(pid));
	if (process.open(QIODevice::ReadOnly)) {
		const auto info = process.read(4096);
		const auto end = info.lastIndexOf(')');
		if (end >= 0
			&& end + 3 < info.size()
			&& info[end + 1] == ' '
			&& info[end + 3] == ' ') {
			return info[end + 2] != 'Z' && info[end + 2] != 'X';
		}
	}
#endif // !Q_OS_MACOS
	return true;
}

#endif // Q_OS_WIN

[[nodiscard]] bool SafeDirectories(const QString &path) {
	for (auto current = path; ; current = QFileInfo(current).absolutePath()) {
		if (PathKind(current) != Kind::Directory) {
			return false;
		}
#ifndef Q_OS_WIN
		struct stat info = {};
		if (lstat(QFile::encodeName(current).constData(), &info) != 0
			|| (info.st_uid != 0 && info.st_uid != geteuid())
			|| ((info.st_mode & S_IWOTH)
				&& !(info.st_uid == 0 && (info.st_mode & S_ISVTX)))
			|| ((info.st_mode & S_IWGRP) && info.st_uid != 0)) {
			return false;
		}
#endif // !Q_OS_WIN
		if (QDir(current).isRoot()) {
			return true;
		}
	}
}

[[nodiscard]] bool SafeWorkDir(const Location &location, const QString &path) {
	if (path.isEmpty() || PathKind(path) != Kind::Directory) {
		return false;
	}
	const auto canonical = QFileInfo(path).canonicalFilePath();
	return !canonical.isEmpty()
		&& !IsWithin(location.state, canonical)
		&& (location.bundle.isEmpty() || !IsWithin(location.bundle, canonical));
}

[[nodiscard]] bool SafeInstallation(const Location &location) {
	return SafeDirectories(QFileInfo(location.executable).absolutePath())
		&& QFileInfo(location.root).isWritable()
		&& PathKind(location.executable) == Kind::File
		&& QFileInfo(location.executable).isExecutable();
}

[[nodiscard]] bool PrivateTree(const Location &location) {
	return SafeDirectories(location.root)
		&& IsPrivateDirectory(location.state)
		&& IsPrivateDirectory(location.transaction);
}

[[nodiscard]] QString TempDirectory() {
	return QFileInfo(QDir::tempPath()).canonicalFilePath();
}

[[nodiscard]] bool IsTemporaryHelperDirectory(const QString &path) {
	const auto info = QFileInfo(path);
	const auto temp = TempDirectory();
	const auto name = info.fileName();
	const auto prefix = QStringView(kTempPrefix);
	return !temp.isEmpty()
		&& info.absolutePath() == temp
		&& name.startsWith(prefix)
		&& !QUuid::fromString(QStringView(name).sliced(prefix.size())).isNull()
		&& SafeDirectories(temp)
		&& IsPrivateDirectory(path);
}

struct HelperDirectoryCleanup {
	QString path;
	~HelperDirectoryCleanup();
};

HelperDirectoryCleanup::~HelperDirectoryCleanup() {
	if (!path.isEmpty()) {
		RemoveSleepyUpdateHelperDirectory(path);
	}
}

[[nodiscard]] bool ValidKey(const QString &key) {
	if (key.size() > 255) {
		return false;
	}
	for (const auto c : key) {
		if (!(c >= u'a' && c <= u'z')
			&& !(c >= u'0' && c <= u'9')
			&& c != u'-'
			&& c != u'_') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::optional<QByteArray> ReadFile(
		const QString &path,
		qint64 limit) {
	if (PathKind(path) != Kind::File) {
		return std::nullopt;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > limit) {
		return std::nullopt;
	}
	const auto result = file.read(limit + 1);
	return result.size() > 0 && result.size() <= limit && file.atEnd()
		? std::make_optional(result)
		: std::nullopt;
}

[[nodiscard]] bool WriteState(const QString &path, const QByteArray &bytes) {
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(bytes) != bytes.size()
		|| !file.commit()) {
		return false;
	}
	return SyncFile(path) && SyncDirectory(QFileInfo(path).absolutePath());
}

[[nodiscard]] bool WriteJournal(const Location &location, const Journal &journal) {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kJournalMagic << journal.workDir << journal.key << journal.device << journal.inode
		<< journal.files << journal.originals << journal.directories << quint8(journal.phase);
	return stream.status() == QDataStream::Ok
		&& WriteState(Join(location.transaction, kJournalName), bytes);
}

[[nodiscard]] std::optional<Journal> ReadJournal(const Location &location) {
	const auto bytes = ReadFile(Join(location.transaction, kJournalName), kMaxJournalSize);
	if (!bytes) {
		return std::nullopt;
	}
	auto journal = Journal();
	auto magic = quint32();
	auto phase = quint8();
	auto stream = QDataStream(*bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	stream >> magic >> journal.workDir >> journal.key >> journal.device >> journal.inode
		>> journal.files >> journal.originals >> journal.directories >> phase;
	if (stream.status() != QDataStream::Ok
		|| !stream.atEnd()
		|| magic != kJournalMagic
		|| phase > quint8(Phase::Committed)
		|| !ValidKey(journal.key)
		|| Absolute(journal.workDir) != journal.workDir
		|| !SafeWorkDir(location, journal.workDir)) {
		return std::nullopt;
	}
	journal.phase = Phase(phase);
	if (journal.phase == Phase::Staging) {
		return (journal.device || journal.inode || journal.files
			|| journal.originals || journal.directories)
			? std::nullopt
			: std::make_optional(journal);
	}
#ifdef Q_OS_MACOS
	if (!journal.inode || journal.files || journal.originals || journal.directories) {
		return std::nullopt;
	}
#else // Q_OS_MACOS
	if (journal.device
		|| journal.inode
		|| !(journal.originals & (quint32(1) << (kAppFiles.size() - 1)))
		|| !(journal.files & (quint32(1) << (kAppFiles.size() - 2)))
		|| journal.files >= (quint32(1) << kAppFiles.size())
		|| (journal.originals & ~journal.files)
		|| journal.directories >= (quint32(1) << kAppDirectories.size())) {
		return std::nullopt;
	}
#endif // Q_OS_MACOS
	return journal;
}

[[nodiscard]] QStringList AppArguments(const QString &workDir, const QString &key) {
	auto result = QStringList();
	if (!key.isEmpty()) {
		result << QStringLiteral("-key") << key;
	}
	if (!workDir.isEmpty()) {
		result << QStringLiteral("-workdir") << workDir;
	}
	return result;
}

[[nodiscard]] bool Relaunch(
		const Location &location,
		const QString &workDir,
		const QString &key) {
	auto args = AppArguments(workDir, key);
#ifdef Q_OS_WIN
	const auto helperDirectory = QCoreApplication::applicationDirPath();
	if (IsTemporaryHelperDirectory(helperDirectory)) {
		args << QStringLiteral("-sleepy-update-cleanup") << helperDirectory;
	}
#endif // Q_OS_WIN
	const auto cwd = workDir.isEmpty() ? QDir::homePath() : workDir;
	return PathKind(location.executable) == Kind::File
		&& QProcess::startDetached(location.executable, args, cwd);
}

[[nodiscard]] bool WaitForParent(qint64 pid) {
	if (pid <= 1
		|| pid > (std::numeric_limits<int>::max)()
		|| pid == QCoreApplication::applicationPid()) {
		return false;
	}
	auto timer = QElapsedTimer();
	timer.start();
	while (ProcessAlive(pid) && timer.elapsed() < kParentTimeout) {
		QThread::msleep(kPollInterval);
	}
	return !ProcessAlive(pid);
}

#ifdef Q_OS_MACOS
[[nodiscard]] int Restart(const QStringList &args, QString &error) {
	if (args.size() < 5) {
		error = QStringLiteral("Invalid restart arguments.");
		return 1;
	}
	const auto executable = Absolute(args[1]);
	const auto workDir = Absolute(args[2]);
	auto validPid = false;
	const auto pid = args[3].toLongLong(&validPid);
	const auto key = args[4];
	const auto suffix = QStringLiteral("/Contents/MacOS/SleepyGram");
	const auto bundle = executable.chopped(
		executable.endsWith(suffix) ? suffix.size() : 0);
	if (bundle == executable
		|| !bundle.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)
		|| PathKind(executable) != Kind::File
		|| !QFileInfo(executable).isExecutable()
		|| (!args[2].isEmpty() && !QFileInfo(workDir).isDir())
		|| !validPid
		|| !ValidKey(key)) {
		error = QStringLiteral("The SleepyGram restart arguments are invalid.");
		return 1;
	}
	auto launch = AppArguments(workDir, key);
	for (const auto &flag : args.mid(5)) {
		if (flag != QStringView(u"-tosettings")
			&& flag != QStringView(u"-autostart")
			&& flag != QStringView(u"-debug")
			&& flag != QStringView(u"-startintray")) {
			error = QStringLiteral("An unsupported SleepyGram restart option was requested.");
			return 1;
		}
		launch.append(flag);
	}
	const auto cwd = workDir.isEmpty() ? QDir::homePath() : workDir;
	if (!WaitForParent(pid)
		|| !QProcess::startDetached(executable, launch, cwd)) {
		error = QStringLiteral("Could not restart SleepyGram.");
		return 1;
	}
	return 0;
}
#endif // Q_OS_MACOS

[[nodiscard]] std::optional<Target> ExpectedTarget() {
#ifdef Q_OS_MACOS
#ifdef Q_PROCESSOR_ARM
	return Target{ Os::Mac, Arch::Arm };
#else // Q_PROCESSOR_ARM
	auto translated = int(0);
	auto size = sizeof(translated);
	const auto rosetta = sysctlbyname(
		"sysctl.proc_translated",
		&translated,
		&size,
		nullptr,
		0) == 0 && translated == 1;
	return Target{ Os::Mac, rosetta ? Arch::Arm : Arch::X64 };
#endif // Q_PROCESSOR_ARM
#elif defined Q_OS_WIN
#if defined Q_PROCESSOR_X86_64
	return Target{ Os::Windows, Arch::X64 };
#elif defined Q_PROCESSOR_ARM
	return Target{ Os::Windows, Arch::Arm };
#else // Q_PROCESSOR_X86_64 || Q_PROCESSOR_ARM
	return Target{ Os::Windows, Arch::X86 };
#endif // Q_PROCESSOR_X86_64 || Q_PROCESSOR_ARM
#elif defined Q_PROCESSOR_X86_64
	return Target{ Os::Linux, Arch::X64 };
#else // Q_OS_MACOS || Q_OS_WIN || Q_PROCESSOR_X86_64
	return std::nullopt;
#endif // Q_OS_MACOS || Q_OS_WIN || Q_PROCESSOR_X86_64
}

[[nodiscard]] bool ExtractVerified(
		const QString &package,
		const QString &stage,
		quint64 runningVersion,
		quint64 targetVersion,
		QString &error) {
	const auto expected = ExpectedTarget();
	if (!expected || runningVersion != AppVersion || targetVersion <= runningVersion) {
		return Fail(error, "This updater cannot install the requested version or target.");
	}
	const auto root = RootPublicKeyPem();
	auto held = ParseVerifiedManifest(EmbeddedManifest(), EmbeddedManifestSignature(), root, &error);
	if (!held) {
		return false;
	}
	const auto manifestPath = Join(QCoreApplication::applicationDirPath(), u"held-manifest");
	if (PathKind(manifestPath) != Kind::Missing) {
		const auto persisted = ReadFile(manifestPath, kMaxManifestSize);
		if (!persisted || persisted->size() <= 64) {
			return Fail(error, "The supplied update key manifest is invalid.");
		}
		auto parsed = ParseVerifiedManifest(persisted->mid(64), persisted->left(64), root, &error);
		if (!parsed) {
			return false;
		}
		if (parsed->version > held->version) {
			held = std::move(parsed);
		}
	}
	auto bytes = ReadFile(package, kMaxPackageSize);
	if (!bytes) {
		return Fail(error, "Could not read the signed update package.");
	}
	const auto expectedVersion = MakeUpdateVersion(quint32(targetVersion), 0);
	const auto update = VerifyUpdate(
		*bytes,
		BuildUpdateChannel,
		AppBetaVersion,
		*expected,
		RunningUpdateVersion(),
		held,
		root,
		QDateTime::currentSecsSinceEpoch(),
		&error);
	bytes.reset();
	if (!update) {
		return false;
	} else if (update->envelope.version != expectedVersion) {
		return Fail(error, "The signed update does not match the requested version.");
	}
	const auto uncompressed = DecompressUpdatePayload(*update, &error);
	return uncompressed && ExtractUpdateFiles(*uncompressed, expectedVersion, stage, &error);
}

#ifdef Q_OS_MACOS

[[nodiscard]] QString StagedBundle(const Location &location) {
	return Join(location.stage, kBundleName);
}

[[nodiscard]] bool CheckBundle(
		const QString &bundle,
		bool staged,
		QString &error) {
	struct stat root = {};
	if (lstat(QFile::encodeName(bundle).constData(), &root) != 0
		|| !S_ISDIR(root.st_mode)
		|| (root.st_mode & (S_IWGRP | S_IWOTH))
		|| PathKind(Join(bundle, kExecutableName)) != Kind::File
		|| !QFileInfo(Join(bundle, kExecutableName)).isExecutable()
		|| PathKind(Join(bundle, kUpdaterName)) != Kind::File
		|| !QFileInfo(Join(bundle, kUpdaterName)).isExecutable()) {
		return Fail(error, staged
			? "The staged macOS bundle layout is invalid."
			: "The installed macOS bundle layout is invalid.");
	}
	auto directories = QStringList{ bundle };
	auto entries = QDirIterator(
		bundle,
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		QDirIterator::Subdirectories);
	while (entries.hasNext()) {
		const auto path = entries.next();
		struct stat info = {};
		if (lstat(QFile::encodeName(path).constData(), &info) != 0
			|| info.st_dev != root.st_dev
			|| (info.st_mode & (S_IWGRP | S_IWOTH))
			|| QFileInfo(path).fileName().compare(u"tdata", Qt::CaseInsensitive) == 0) {
			return Fail(error, staged
				? "The staged macOS bundle has an unsafe file or directory."
				: "The installed macOS bundle has an unsafe file or directory.");
		} else if (S_ISDIR(info.st_mode)) {
			directories.push_back(path);
		} else if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
			return Fail(error, staged
				? "The staged macOS bundle contains an unsupported file type."
				: "The installed macOS bundle contains an unsupported file type.");
		} else if (staged && !SyncFile(path)) {
			return Fail(error, "Could not sync a staged macOS bundle file.");
		}
	}
	if (staged) {
		for (auto i = directories.crbegin(); i != directories.crend(); ++i) {
			if (!SyncDirectory(*i)) {
				return Fail(error, "Could not sync a staged macOS bundle directory.");
			}
		}
	}
	auto codesign = QProcess();
	codesign.setStandardErrorFile(QProcess::nullDevice());
	codesign.start(
		QStringLiteral("/usr/bin/codesign"),
		{
			QStringLiteral("--verify"),
			QStringLiteral("--deep"),
			QStringLiteral("--strict"),
			QStringLiteral("-R=identifier \"com.sleepygram.desktop\""),
			bundle,
		});
	if (!codesign.waitForFinished(30000)) {
		return Fail(error, "macOS codesign could not finish bundle verification.");
	}
	if (codesign.exitStatus() != QProcess::NormalExit || codesign.exitCode() != 0) {
		return Fail(error, staged
			? "macOS codesign rejected the staged bundle."
			: "macOS codesign rejected the installed bundle.");
	}
	return true;
}

[[nodiscard]] bool BundleIdentity(const QString &path, quint64 &device, quint64 &inode) {
	struct stat identity = {};
	if (lstat(QFile::encodeName(path).constData(), &identity) != 0
		|| !S_ISDIR(identity.st_mode)) {
		return false;
	}
	device = quint64(identity.st_dev);
	inode = quint64(identity.st_ino);
	return true;
}

[[nodiscard]] bool OriginalBundle(const QString &path, const Journal &journal) {
	auto device = quint64(0);
	auto inode = quint64(0);
	return BundleIdentity(path, device, inode)
		&& device == journal.device
		&& inode == journal.inode;
}

[[nodiscard]] bool Prepare(const Location &location, Journal &journal, QString &error) {
	const auto entries = QDir(location.stage).entryList(
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
	const auto staged = StagedBundle(location);
	if (entries != QStringList{ QStringView(kBundleName).toString() }) {
		return Fail(error, "The staged macOS update must contain one SleepyGram bundle.");
	}
	if (!CheckBundle(staged, true, error) || !CheckBundle(location.bundle, false, error)) {
		return false;
	} else if (!BundleIdentity(location.bundle, journal.device, journal.inode)) {
		return Fail(error, "Could not identify the installed macOS bundle.");
	}
	return SyncDirectory(location.stage)
		|| Fail(error, "Could not sync the macOS update staging directory.");
}

[[nodiscard]] bool SwapBundle(const Location &location, QString &error) {
	const auto staged = StagedBundle(location);
	if (PathKind(staged) != Kind::Directory
		|| PathKind(location.bundle) != Kind::Directory) {
		return Fail(error, "Could not find both macOS bundles before exchanging them.");
	}
	if (renameatx_np(
			AT_FDCWD,
			QFile::encodeName(staged).constData(),
			AT_FDCWD,
			QFile::encodeName(location.bundle).constData(),
			RENAME_SWAP) != 0) {
		error = QStringLiteral("Could not exchange the macOS bundles (errno %1).").arg(errno);
		return false;
	}
	return (SyncDirectory(location.stage) && SyncDirectory(location.root))
		|| Fail(error, "Could not sync the exchanged macOS bundles.");
}

[[nodiscard]] bool Install(
		const Location &location,
		const Journal &journal,
		QString &error) {
	if (!OriginalBundle(location.bundle, journal)) {
		return Fail(error, "The installed bundle changed during update preparation.");
	}
	return SwapBundle(location, error);
}

[[nodiscard]] bool RollBack(
		const Location &location,
		const Journal &journal,
		QString &error) {
	if (OriginalBundle(location.bundle, journal)) {
		return SyncDirectory(location.root) && SyncDirectory(location.stage);
	} else if (!OriginalBundle(StagedBundle(location), journal)) {
		return Fail(error, "The original SleepyGram bundle could not be located.");
	}
	return SwapBundle(location, error);
}

#else // Q_OS_MACOS

[[nodiscard]] quint32 FileMask(QStringView path) {
	for (auto i = size_t(0); i != kAppFiles.size(); ++i) {
		if (path == QStringView(kAppFiles[i])) {
			return quint32(1) << i;
		}
	}
	return 0;
}

[[nodiscard]] bool CheckDirectoryChain(
		const Location &location,
		const QString &path,
		bool missingAllowed) {
	for (auto current = QFileInfo(path).absolutePath();
		current != location.root;
		current = QFileInfo(current).absolutePath()) {
		const auto kind = PathKind(current);
		if (kind == Kind::Missing && missingAllowed) {
			continue;
		}
		if (kind != Kind::Directory || !QFileInfo(current).isWritable()) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool Prepare(const Location &location, Journal &journal, QString &error) {
	const auto stage = QDir(location.stage);
	auto entries = QDirIterator(
		location.stage,
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
		QDirIterator::Subdirectories);
	auto targetDirectories = QStringList();
	while (entries.hasNext()) {
		const auto path = entries.next();
		const auto name = stage.relativeFilePath(path);
		const auto kind = PathKind(path);
		if (kind == Kind::Directory) {
			auto allowed = false;
			for (const auto directory : kAppDirectories) {
				allowed = allowed || name == QStringView(directory);
			}
			if (!allowed) {
				return Fail(error, "The package contains an unexpected app directory.");
			}
			targetDirectories.push_back(name);
			continue;
		}
		const auto mask = FileMask(name);
		if (kind != Kind::File || !mask) {
			return Fail(error, "The package contains a file outside the app allowlist.");
		}
		const auto target = Join(location.root, name);
		const auto oldKind = PathKind(target);
		if (!CheckDirectoryChain(location, target, true)
			|| (oldKind != Kind::File && oldKind != Kind::Missing)
			|| !SyncFile(path)) {
			return Fail(error, "An installed app file or its directory is unsafe or unavailable.");
		}
		journal.files |= mask;
		if (oldKind == Kind::File) {
			const auto backup = Join(location.backup, name);
			const auto parent = QFileInfo(backup).absolutePath();
			if (!QDir().mkpath(parent)
				|| !BackupFile(target, backup)
				|| !SyncDirectory(parent)) {
				return Fail(error, "Cannot preserve the existing app file on this volume.");
			}
			journal.originals |= mask;
		}
	}
	if (!(journal.files & FileMask(kExecutableName))
		|| !(journal.originals & FileMask(kExecutableName))
		|| !(journal.files & FileMask(kUpdaterName))
		|| !QFileInfo(Join(location.stage, kExecutableName)).isExecutable()
		|| !QFileInfo(Join(location.stage, kUpdaterName)).isExecutable()) {
		return Fail(error, "The package does not contain the expected executable and updater.");
	}
	for (auto i = size_t(0); i != kAppDirectories.size(); ++i) {
		const auto name = kAppDirectories[i];
		if (targetDirectories.contains(QStringView(name).toString())
			&& PathKind(Join(location.root, name)) == Kind::Missing) {
			journal.directories |= quint32(1) << i;
		}
	}
	for (const auto directory : kAppDirectories) {
		const auto staged = Join(location.stage, directory);
		const auto backup = Join(location.backup, directory);
		if ((PathKind(staged) == Kind::Directory && !SyncDirectory(staged))
			|| (PathKind(backup) == Kind::Directory && !SyncDirectory(backup))) {
			return Fail(error, "Could not sync the packaged app directories.");
		}
	}
	return SyncDirectory(location.stage) && SyncDirectory(location.backup);
}

[[nodiscard]] bool Install(
		const Location &location,
		const Journal &journal,
		QString &error) {
	for (auto i = size_t(0); i != kAppDirectories.size(); ++i) {
		if (journal.directories & (quint32(1) << i)) {
			const auto path = Join(location.root, kAppDirectories[i]);
			if (PathKind(path) != Kind::Missing
				|| !QDir().mkdir(path)
				|| !SyncDirectory(QFileInfo(path).absolutePath())) {
				return Fail(error, "Could not prepare an app file directory.");
			}
		}
	}
	for (auto i = size_t(0); i != kAppFiles.size(); ++i) {
		const auto mask = quint32(1) << i;
		if (!(journal.files & mask)) {
			continue;
		}
		const auto name = kAppFiles[i];
		const auto target = Join(location.root, name);
		const auto staged = Join(location.stage, name);
		const auto replace = (journal.originals & mask) != 0;
		if (!CheckDirectoryChain(location, target, false)
			|| PathKind(target) != (replace ? Kind::File : Kind::Missing)
			|| PathKind(staged) != Kind::File
			|| !Rename(staged, target, replace)
			|| !SyncDirectory(QFileInfo(target).absolutePath())
			|| !SyncDirectory(QFileInfo(staged).absolutePath())) {
			return Fail(error, "Could not atomically replace an app file.");
		}
	}
	return true;
}

[[nodiscard]] bool RollBack(
		const Location &location,
		const Journal &journal,
		QString &error) {
	for (auto i = size_t(0); i != kAppFiles.size(); ++i) {
		const auto mask = quint32(1) << i;
		if (!(journal.files & mask)) {
			continue;
		}
		const auto name = kAppFiles[i];
		const auto staged = Join(location.stage, name);
		const auto stagedKind = PathKind(staged);
		if (stagedKind == Kind::File) {
			continue;
		} else if (stagedKind != Kind::Missing) {
			return Fail(error, "A staged app file changed type during the update.");
		}
		const auto target = Join(location.root, name);
		const auto targetKind = PathKind(target);
		const auto backup = Join(location.backup, name);
		const auto backupKind = PathKind(backup);
		if (!CheckDirectoryChain(location, target, true)
			|| (targetKind != Kind::File && targetKind != Kind::Missing)
			|| (backupKind != Kind::File && backupKind != Kind::Missing)) {
			return Fail(error, "An app file could not be safely restored.");
		}
		if (journal.originals & mask) {
			if ((backupKind == Kind::File && !Rename(backup, target, targetKind == Kind::File))
				|| (backupKind == Kind::Missing && targetKind != Kind::File)) {
				return Fail(error, "The backup app file could not be restored.");
			}
		} else if (targetKind == Kind::File && !QFile::remove(target)) {
			return Fail(error, "The replacement app file could not be removed.");
		}
		const auto parent = QFileInfo(target).absolutePath();
		if ((PathKind(parent) == Kind::Directory && !SyncDirectory(parent))
			|| ((journal.originals & mask) && !SyncDirectory(QFileInfo(backup).absolutePath()))) {
			return Fail(error, "The restored app file could not be synced.");
		}
	}
	for (auto i = kAppDirectories.size(); i != 0;) {
		--i;
		if (journal.directories & (quint32(1) << i)) {
			const auto path = Join(location.root, kAppDirectories[i]);
			const auto filters = QDir::AllEntries | QDir::NoDotAndDotDot
				| QDir::Hidden | QDir::System;
			if (PathKind(path) == Kind::Directory
				&& QDir(path).isEmpty(filters)
				&& (!QDir().rmdir(path) || !SyncDirectory(QFileInfo(path).absolutePath()))) {
				return Fail(error, "Could not remove an empty app file directory.");
			}
		}
	}
	return true;
}

#endif // Q_OS_MACOS

[[nodiscard]] bool Retire(const Location &location, QString &error) {
	const auto retired = Join(
		location.state,
		QStringLiteral("cleanup-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
	if (!Rename(location.transaction, retired, false) || !SyncDirectory(location.state)) {
		return Fail(error, "Could not retire the update journal.");
	}
	if (!QDir(retired).removeRecursively()) {
		error = QStringLiteral("Could not delete retired update files.");
	}
	return true;
}

[[nodiscard]] bool WaitForProbeExit(const Location &location, QString &error) {
	const auto path = Join(location.transaction, u"probe-pid");
	if (PathKind(path) == Kind::Missing) {
		return true;
	}
	const auto bytes = ReadFile(path, 32);
	auto valid = false;
	const auto pid = bytes ? bytes->toLongLong(&valid) : qint64(0);
	if (!valid || pid <= 1 || pid > (std::numeric_limits<int>::max)()) {
		return Fail(error, "The interrupted update probe has an invalid process ID.");
	}
	auto timer = QElapsedTimer();
	timer.start();
	while (ProcessAlive(pid) && timer.elapsed() < kReleaseTimeout) {
		QThread::msleep(kPollInterval);
	}
	return !ProcessAlive(pid)
		|| Fail(error, "The interrupted update probe did not exit.");
}

[[nodiscard]] bool Recover(
		const Location &location,
		QString &workDir,
		QString &key,
		QString &error) {
	if (!PrivateTree(location)) {
		return Fail(error, "The update journal directory is unsafe.");
	}
	const auto path = Join(location.transaction, kJournalName);
	if (PathKind(path) != Kind::Missing) {
		const auto journal = ReadJournal(location);
		if (!journal) {
			return Fail(error, "The update journal is invalid.");
		}
		workDir = journal->workDir;
		key = journal->key;
		if (journal->phase == Phase::Committed) {
			if (!SyncFile(path) || !SyncDirectory(location.transaction)) {
				return Fail(error, "Could not sync the committed update journal.");
			}
		} else if (journal->phase == Phase::Prepared) {
			if (!WriteState(Join(location.transaction, u"abort"), "abort")) {
				return Fail(error, "Could not signal the interrupted update probe to stop.");
			} else if (!WaitForProbeExit(location, error)
				|| !RollBack(location, *journal, error)) {
				if (error.isEmpty()) {
					error = QStringLiteral("Could not roll back the interrupted update.");
				}
				return false;
			}
		}
	}
	return Retire(location, error);
}

[[nodiscard]] bool ConfirmProbe(
		QProcess &probe,
		const Location &location,
		QString &error) {
	const auto ready = Join(location.transaction, u"ready");
	auto timer = QElapsedTimer();
	timer.start();
	while (probe.state() != QProcess::NotRunning && timer.elapsed() < kProbeTimeout) {
		if (PathKind(ready) == Kind::File) {
			return true;
		}
		if (probe.waitForFinished(kPollInterval)) {
			break;
		}
	}
	if (probe.state() == QProcess::NotRunning) {
		error = (probe.exitStatus() == QProcess::CrashExit)
			? QStringLiteral("The replacement app crashed or was stopped before readiness.")
			: QStringLiteral("The replacement app exited before readiness (status %1).")
				.arg(probe.exitCode());
		return false;
	}
	return Fail(error, "The replacement app timed out before readiness.");
}

[[nodiscard]] bool StopProbe(QProcess &probe, const Location &location) {
	if (probe.state() == QProcess::NotRunning) {
		return true;
	}
	const auto abort = WriteState(Join(location.transaction, u"abort"), "abort");
	if (abort && probe.waitForFinished(1000)) {
		return true;
	}
	probe.kill();
	return probe.waitForFinished(kReleaseTimeout) || probe.state() == QProcess::NotRunning;
}

[[nodiscard]] bool ProbeAndCommit(
		const Location &location,
		Journal &journal,
		quint64 targetVersion,
		QString &error) {
	auto probeArgs = AppArguments(journal.workDir, journal.key);
	probeArgs << QStringLiteral("-sleepy-update-probe") << location.transaction
		<< QStringLiteral("--sleepy-update-expected-version") << QString::number(targetVersion);
	auto probe = QProcess();
	probe.setProgram(location.executable);
	probe.setArguments(probeArgs);
	probe.setWorkingDirectory(journal.workDir);
	probe.setProcessChannelMode(QProcess::ForwardedChannels);
	probe.start();
	const auto started = probe.waitForStarted(5000);
	const auto pid = probe.processId();
	if (!started) {
		error = QStringLiteral("Could not start the replacement app (process error %1).")
			.arg(int(probe.error()));
	} else if (!pid
		|| !WriteState(Join(location.transaction, u"probe-pid"), QByteArray::number(pid))) {
		error = QStringLiteral("Could not record the replacement app process.");
	}
	if (!error.isEmpty() || !ConfirmProbe(probe, location, error)) {
		if (!StopProbe(probe, location)) {
			error += QStringLiteral(" The startup check process could not be stopped.");
		}
		return false;
	}
	if (!WriteState(Join(location.transaction, u"commit"), "commit")) {
		const auto stopped = StopProbe(probe, location);
		return stopped
			? Fail(error, "The update reached readiness but could not be committed.")
			: Fail(error, "The replacement app could not be stopped after the commit failure.");
	}
	if ((!probe.waitForFinished(kReleaseTimeout) && probe.state() != QProcess::NotRunning)
		|| probe.exitStatus() != QProcess::NormalExit
		|| probe.exitCode() != 0
		|| PathKind(Join(location.transaction, u"released")) != Kind::File) {
		const auto stopped = StopProbe(probe, location);
		return stopped
			? Fail(error, "The replacement app did not finish its startup check.")
			: Fail(error, "The replacement app could not be stopped.");
	}
	journal.phase = Phase::Committed;
	return WriteJournal(location, journal);
}

[[nodiscard]] bool Apply(
		const Location &location,
		const QString &package,
		const QString &workDir,
		quint64 runningVersion,
		quint64 targetVersion,
		const QString &key,
		QString &error) {
	if (!SafeInstallation(location) || !SafeWorkDir(location, workDir)) {
		return Fail(error, "This SleepyGram installation cannot update in place.");
	}
	if (!CreatePrivateDirectory(location.stage)
		|| !CreatePrivateDirectory(location.backup)) {
		return Fail(error, "Could not create private update staging on the installation volume.");
	}
	auto journal = Journal{ workDir, key };
	journal.phase = Phase::Prepared;
	return ExtractVerified(package, location.stage, runningVersion, targetVersion, error)
		&& Prepare(location, journal, error)
		&& WriteJournal(location, journal)
		&& Install(location, journal, error)
		&& ProbeAndCommit(location, journal, targetVersion, error);
}

} // namespace

QString SleepyUpdateTransactionPath(const QString &executable) {
	const auto location = GetLocation(executable);
	return location ? location->transaction : QString();
}

bool SleepyUpdateInstallationSupported(
		const QString &executable,
		const QString &workDir) {
	const auto location = GetLocation(executable);
	return location
		&& SafeInstallation(*location)
		&& SafeWorkDir(*location, Absolute(workDir))
		&& (PathKind(location->state) == Kind::Missing
			|| IsPrivateDirectory(location->state))
		&& PathKind(location->transaction) == Kind::Missing;
}

bool SleepyUpdateProbeDirectoryValid(const QString &executable, const QString &directory) {
	const auto location = GetLocation(executable);
	if (!location || Absolute(directory) != location->transaction || !PrivateTree(*location)) {
		return false;
	}
	const auto journal = ReadJournal(*location);
	return journal && journal->phase == Phase::Prepared;
}

QString CreateSleepyUpdateHelperDirectory() {
	const auto temp = TempDirectory();
	if (temp.isEmpty() || !SafeDirectories(temp)) {
		return {};
	}
	const auto path = Join(
		temp,
		QStringView(kTempPrefix).toString()
			+ QUuid::createUuid().toString(QUuid::WithoutBraces));
	return CreatePrivateDirectory(path) ? path : QString();
}

bool RemoveSleepyUpdateHelperDirectory(const QString &directory) {
	const auto path = Absolute(directory);
	if (!IsTemporaryHelperDirectory(path)) {
		return !path.isEmpty() && PathKind(path) == Kind::Missing;
	}
#ifdef Q_OS_WIN
	const auto updater = Join(path, u"Updater.exe");
#else // Q_OS_WIN
	const auto updater = Join(path, u"Updater");
#endif // Q_OS_WIN
	const auto manifest = Join(path, u"held-manifest");
	for (const auto &entry : { manifest, updater }) {
		const auto kind = PathKind(entry);
		if (kind != Kind::Missing && (kind != Kind::File || !QFile::remove(entry))) {
			return false;
		}
	}
	return QDir().rmdir(path);
}

int RunSleepyUpdater(const QStringList &args, QString &error) {
	const auto cleanup = HelperDirectoryCleanup{ QCoreApplication::applicationDirPath() };
#ifdef Q_OS_MACOS
	if (!args.isEmpty() && args.front() == QStringView(u"--restart")) {
		return Restart(args, error);
	}
#endif // Q_OS_MACOS
	const auto apply = !args.isEmpty() && args.front() == QStringView(u"--apply");
	const auto recover = !args.isEmpty() && args.front() == QStringView(u"--recover");
	if ((!apply && !recover) || args.size() < (apply ? 7 : 4)) {
		error = QStringLiteral("Invalid updater arguments.");
		return 1;
	}
	const auto location = GetLocation(args[apply ? 2 : 1]);
	auto workDir = Absolute(args[apply ? 3 : 2]);
	const auto package = apply ? Absolute(args[1]) : QString();
	auto validPid = false;
	const auto pid = args[apply ? 6 : 3].toLongLong(&validPid);
	if (!location
		|| !SafeDirectories(location->root)
		|| !validPid
		|| pid <= 1
		|| pid > (std::numeric_limits<int>::max)()
		|| pid == QCoreApplication::applicationPid()
		|| (apply && (package.isEmpty() || !SafeWorkDir(*location, workDir)))
		|| (recover && !args[2].isEmpty() && !SafeWorkDir(*location, workDir))
		|| (recover && !WaitForParent(pid))) {
		error = QStringLiteral("The installation or parent process was unavailable.");
		return 1;
	}
	const auto appArgs = args.mid(apply ? 7 : 4);
	auto key = appArgs.value(1);
	if (!appArgs.isEmpty()
		&& (appArgs.size() != 2
			|| appArgs.front() != QStringView(u"-key")
			|| key.isEmpty()
			|| !ValidKey(key))) {
		error = QStringLiteral("Invalid profile arguments.");
		return 1;
	}
	if (PathKind(location->state) == Kind::Missing) {
		if (!apply) {
			return Relaunch(*location, workDir, key) ? 0 : 1;
		}
		if (!CreatePrivateDirectory(location->state) || !SyncDirectory(location->root)) {
			error = QStringLiteral("Could not create the update journal directory.");
			if (PathKind(location->transaction) == Kind::Missing
				&& !Relaunch(*location, workDir, key)) {
				error = QStringLiteral("Could not create the journal or restart SleepyGram.");
			}
			return 1;
		}
	}
	if (!IsPrivateDirectory(location->state)) {
		error = QStringLiteral("The update journal directory is unsafe.");
		return 1;
	}
	auto lock = QLockFile(Join(location->state, u"lock"));
	lock.setStaleLockTime(0);
	if (!lock.tryLock()) {
		error = QStringLiteral("Another updater owns this installation.");
		return 1;
	}
	auto applied = false;
	if (apply && PathKind(location->transaction) == Kind::Missing) {
		if (!CreatePrivateDirectory(location->transaction)
			|| !SyncDirectory(location->state)
			|| !WriteJournal(*location, Journal{ workDir, key })) {
			error = QStringLiteral("Could not persist the update intent.");
			return 1;
		}
		if (!WaitForParent(pid)) {
			error = QStringLiteral("SleepyGram did not exit before the update deadline.");
			auto cleanupError = QString();
			if (!Retire(*location, cleanupError) || !cleanupError.isEmpty()) {
				error = cleanupError;
			}
			return 1;
		}
		auto validRunning = false;
		auto validTarget = false;
		const auto running = args[4].toUInt(&validRunning);
		const auto target = args[5].toUInt(&validTarget);
		if (validRunning && validTarget) {
			applied = Apply(*location, package, workDir, running, target, key, error);
		}
		if (!applied && error.isEmpty()) {
			error = QStringLiteral("Could not complete the requested update.");
		}
	} else if (apply && !WaitForParent(pid)) {
		error = QStringLiteral("SleepyGram did not exit before the recovery deadline.");
		return 1;
	}
	if (PathKind(location->transaction) != Kind::Missing) {
		auto recoveryError = QString();
		if (!Recover(*location, workDir, key, recoveryError)) {
			error = recoveryError;
			return 1;
		} else if (!recoveryError.isEmpty()) {
			error = recoveryError;
		}
	}
	if (!Relaunch(*location, workDir, key)) {
		error = QStringLiteral("Could not restart SleepyGram.");
		return 1;
	}
	if (apply && !applied) {
		error += QStringLiteral(" Restarted the installed app without confirming this update.");
		return 1;
	}
	return 0;
}

} // namespace Core::Updates
