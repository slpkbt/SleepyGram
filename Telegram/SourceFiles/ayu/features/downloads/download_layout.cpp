/*
This file is part of SleepyGram Desktop,
an unofficial client based on AyuGram Desktop and Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/features/downloads/download_layout.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_photo.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_account.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>

#include <array>

namespace Ayu::Downloads {
namespace {

constexpr auto kMaxSegmentLength = 80;

[[nodiscard]] bool IsBidiOrControl(QChar ch) {
	const auto u = ch.unicode();
	if (u < 0x20 || u == 0x7F) {
		return true;
	}
	return (u >= 0x202A && u <= 0x202E)
		|| (u >= 0x2066 && u <= 0x2069)
		|| (u == 0x200E)
		|| (u == 0x200F)
		|| (u == 0x061C);
}

[[nodiscard]] bool IsReservedWindowsName(const QString &name) {
	const auto upper = name.section(u'.', 0, 0).trimmed().toUpper();
	static const auto kReserved = std::array{
		u"CON"_q, u"PRN"_q, u"AUX"_q, u"NUL"_q,
		u"COM1"_q, u"COM2"_q, u"COM3"_q, u"COM4"_q, u"COM5"_q,
		u"COM6"_q, u"COM7"_q, u"COM8"_q, u"COM9"_q,
		u"LPT1"_q, u"LPT2"_q, u"LPT3"_q, u"LPT4"_q, u"LPT5"_q,
		u"LPT6"_q, u"LPT7"_q, u"LPT8"_q, u"LPT9"_q,
		u"COM¹"_q, u"COM²"_q, u"COM³"_q, u"LPT¹"_q, u"LPT²"_q, u"LPT³"_q,
	};
	for (const auto &entry : kReserved) {
		if (upper == entry) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QStringList TypeSegments(
		DocumentData *document,
		PhotoData *photo) {
	if (photo) {
		return { u"Images"_q };
	}
	if (!document) {
		return {};
	}
	if (document->isImage()) {
		return { u"Images"_q };
	}
	if (document->isVideoFile() || document->isVideoMessage()) {
		return { u"Videos"_q };
	}
	if (document->isVoiceMessage() || document->isAudioFile()) {
		return { u"Audio"_q };
	}
	const auto filename = document->filename();
	if (filename.endsWith(u".pdf"_q, Qt::CaseInsensitive)) {
		return { u"Documents"_q, u"PDF"_q };
	}
	switch (Core::DetectNameType(filename)) {
	case Core::NameType::Image:
		return { u"Images"_q };
	case Core::NameType::Video:
		return { u"Videos"_q };
	case Core::NameType::Audio:
		return { u"Audio"_q };
	case Core::NameType::Archive:
		return { u"Archives"_q };
	default:
		return { u"Documents"_q };
	}
}

} // namespace

bool OrganizeByTopic() {
	return Core::App().settings().readPref<bool>(
		Core::kDownloadOrganizeByTopicKey,
		false);
}

bool OrganizeByType() {
	return Core::App().settings().readPref<bool>(
		Core::kDownloadOrganizeByTypeKey,
		false);
}

bool IsOrganizationEnabled() {
	return OrganizeByTopic() || OrganizeByType();
}

QString SanitizePathSegment(const QString &raw) {
	auto out = QString();
	out.reserve(raw.size());
	for (const auto ch : raw) {
		if (IsBidiOrControl(ch)
			|| ch == '/' || ch == '\\' || ch == ':' || ch == '*'
			|| ch == '?' || ch == '"' || ch == '<' || ch == '>'
			|| ch == '|') {
			out.append('_');
		} else {
			out.append(ch);
		}
	}
	out = out.simplified();
	while (!out.isEmpty() && (out.endsWith('.') || out.endsWith(' '))) {
		out.chop(1);
	}
	while (!out.isEmpty() && (out.startsWith('.') || out.startsWith(' '))) {
		out.remove(0, 1);
	}
	if (out.size() > kMaxSegmentLength) {
		const auto length = out.at(kMaxSegmentLength - 1).isHighSurrogate()
			? (kMaxSegmentLength - 1)
			: kMaxSegmentLength;
		out = out.left(length).trimmed();
		while (!out.isEmpty() && out.endsWith('.')) {
			out.chop(1);
		}
	}
	if (out.isEmpty()) {
		return QString();
	}
	if (IsReservedWindowsName(out)) {
		out.prepend('_');
	}
	return out;
}

QString ResolveDirectory(
		const QString &basePath,
		not_null<Main::Session*> session,
		const HistoryItem *item,
		DocumentData *document,
		PhotoData *photo) {
	if (basePath.isEmpty() || !IsOrganizationEnabled()) {
		return basePath;
	}
	const auto settingPath = Core::App().settings().downloadPath();
	if (settingPath == FileDialog::Tmp()
		&& basePath.startsWith(session->local().tempDirectory())) {
		return basePath;
	}

	auto segments = QStringList();
	if (OrganizeByTopic() && item) {
		auto topicName = QString();
		if (const auto topic = item->topic()) {
			topicName = topic->title();
		}
		if (topicName.isEmpty() && item->topicRootId()) {
			topicName = u"Topic "_q
				+ QString::number(item->topicRootId().bare);
		}
		const auto sanitized = SanitizePathSegment(topicName);
		if (!sanitized.isEmpty()) {
			segments.push_back(sanitized);
		}
	}
	if (OrganizeByType()) {
		for (const auto &seg : TypeSegments(document, photo)) {
			const auto sanitized = SanitizePathSegment(seg);
			if (!sanitized.isEmpty()) {
				segments.push_back(sanitized);
			}
		}
	}
	if (segments.isEmpty()) {
		return basePath;
	}

	auto target = QDir(basePath);
	for (const auto &seg : segments) {
		target = QDir(target.filePath(seg));
	}
	const auto resolved = target.absolutePath();
	if (!QDir().exists(resolved) && !QDir().mkpath(resolved)) {
		return basePath;
	}
	return resolved + '/';
}

} // namespace Ayu::Downloads
