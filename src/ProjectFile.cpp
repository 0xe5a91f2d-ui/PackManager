#include "ProjectFile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

constexpr double kMaxExactJsonInteger = 9007199254740991.0;

[[noreturn]] void invalidProject(const QString& detail)
{
    throw std::runtime_error(
        QString("Proje taslağı geçersiz: %1").arg(detail).toStdString());
}

quint64 readUnsignedInteger(const QJsonValue& value, const QString& key)
{
    if (!value.isDouble()) {
        invalidProject(QString("%1 alanı eksik.").arg(key));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || std::floor(number) != number
        || number > kMaxExactJsonInteger) {
        invalidProject(QString("%1 alanı geçersiz.").arg(key));
    }
    return static_cast<quint64>(number);
}

QString optionalString(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    if (!value.isUndefined() && !value.isString()) {
        invalidProject(QString("%1 alanı metin olmalı.").arg(key));
    }
    return value.toString();
}

QStringList optionalStringList(const QJsonObject& object, const QString& key)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isArray()) {
        invalidProject(QString("%1 alanı liste olmalı.").arg(key));
    }
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString() || item.toString().trimmed().isEmpty()) {
            invalidProject(QString("%1 listesinde geçersiz değer var.").arg(key));
        }
        if (!result.contains(item.toString())) {
            result.push_back(item.toString());
        }
    }
    return result;
}

} // namespace

QVector<VideoEntry> readProjectFile(const QString& path,
                                    PackageMetadata* metadata)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    if (file.size() > 16 * 1024 * 1024) {
        invalidProject(QStringLiteral("Dosya 16 MiB boyut sınırını aşıyor."));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        invalidProject(QStringLiteral("Dosya JSON biçiminde değil."));
    }
    const QJsonObject root = document.object();
    const quint64 projectVersion = readUnsignedInteger(
        root.value(QStringLiteral("version")), QStringLiteral("version"));
    if (root.value(QStringLiteral("format")).toString()
            != QStringLiteral("packmanager.project")
        || (projectVersion != 1 && projectVersion != 2)) {
        invalidProject(QStringLiteral("Desteklenmeyen taslak sürümü."));
    }
    const QJsonValue videosValue = root.value(QStringLiteral("videos"));
    if (!videosValue.isArray()) {
        invalidProject(QStringLiteral("Videolar listesi bulunamadı."));
    }
    if (metadata != nullptr) {
        const QJsonValue packageValue = root.value(QStringLiteral("package"));
        if (!packageValue.isUndefined() && !packageValue.isObject()) {
            invalidProject(QStringLiteral("Paket bilgileri geçersiz."));
        }
        const QJsonObject package = packageValue.toObject();
        metadata->title = package.value(QStringLiteral("title")).toString();
        metadata->description = package.value(QStringLiteral("description")).toString();
        metadata->version = package.value(QStringLiteral("version"))
            .toString(QStringLiteral("1.0"));
        metadata->createdAt = package.value(QStringLiteral("created_at")).toString();
    }

    QVector<VideoEntry> entries;
    const QJsonArray videos = videosValue.toArray();
    entries.reserve(videos.size());
    for (const QJsonValue& value : videos) {
        if (!value.isObject()) {
            invalidProject(QStringLiteral("Video girdisi geçersiz."));
        }
        const QJsonObject video = value.toObject();
        VideoEntry entry;
        entry.id = optionalString(video, QStringLiteral("id"));
        if (entry.id.isEmpty()) {
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        entry.title = optionalString(video, QStringLiteral("title"));
        entry.description = optionalString(video, QStringLiteral("description"));
        entry.category = optionalString(video, QStringLiteral("category"));
        entry.tags = optionalStringList(video, QStringLiteral("tags"));
        entry.collection = optionalString(video, QStringLiteral("collection"));
        entry.subtitlePath = optionalString(video, QStringLiteral("subtitle_path"));
        entry.videoSha256 = optionalString(video, QStringLiteral("video_sha256"));
        entry.coverSha256 = optionalString(video, QStringLiteral("cover_sha256"));
        entry.subtitleSha256 = optionalString(video, QStringLiteral("subtitle_sha256"));
        entry.videoPath = optionalString(video, QStringLiteral("video_path"));
        entry.coverPath = optionalString(video, QStringLiteral("cover_path"));
        entry.assetContainerPath = optionalString(video, QStringLiteral("asset_container_path"));
        const QJsonValue videoEmbedded = video.value(QStringLiteral("video_is_embedded"));
        const QJsonValue coverEmbedded = video.value(QStringLiteral("cover_is_embedded"));
        if ((!videoEmbedded.isUndefined() && !videoEmbedded.isBool())
            || (!coverEmbedded.isUndefined() && !coverEmbedded.isBool())) {
            invalidProject(QStringLiteral("Medya kaynağı işaretleri geçersiz."));
        }
        entry.videoIsEmbedded = videoEmbedded.toBool();
        entry.coverIsEmbedded = coverEmbedded.toBool();
        const QJsonValue subtitleEmbedded = video.value(QStringLiteral("subtitle_is_embedded"));
        if (!subtitleEmbedded.isUndefined() && !subtitleEmbedded.isBool()) {
            invalidProject(QStringLiteral("Altyazı kaynağı işareti geçersiz."));
        }
        entry.subtitleIsEmbedded = subtitleEmbedded.toBool();
        entry.videoOffset = readUnsignedInteger(
            video.value(QStringLiteral("video_offset")), QStringLiteral("video_offset"));
        entry.videoSize = readUnsignedInteger(
            video.value(QStringLiteral("video_size")), QStringLiteral("video_size"));
        entry.coverOffset = readUnsignedInteger(
            video.value(QStringLiteral("cover_offset")), QStringLiteral("cover_offset"));
        entry.coverSize = readUnsignedInteger(
            video.value(QStringLiteral("cover_size")), QStringLiteral("cover_size"));
        entry.subtitleOffset = readUnsignedInteger(
            video.value(QStringLiteral("subtitle_offset")), QStringLiteral("subtitle_offset"));
        entry.subtitleSize = readUnsignedInteger(
            video.value(QStringLiteral("subtitle_size")), QStringLiteral("subtitle_size"));
        const QJsonValue duration = video.value(QStringLiteral("duration_ms"));
        if (!duration.isUndefined() && !duration.isNull()) {
            entry.durationMs = static_cast<qint64>(
                readUnsignedInteger(duration, QStringLiteral("duration_ms")));
        }
        if (entry.title.trimmed().isEmpty() || entry.category.trimmed().isEmpty()
            || entry.videoPath.isEmpty()) {
            invalidProject(QStringLiteral("Video başlığı, kategorisi veya medya yolu eksik."));
        }
        if (entry.videoIsEmbedded
            && (entry.assetContainerPath.isEmpty() || entry.videoSize == 0)) {
            invalidProject(QStringLiteral("Paket içindeki video kaynağı geçersiz."));
        }
        if (entry.coverIsEmbedded
            && (entry.assetContainerPath.isEmpty() || entry.coverSize == 0)) {
            invalidProject(QStringLiteral("Paket içindeki kapak kaynağı geçersiz."));
        }
        if (entry.subtitleIsEmbedded
            && (entry.assetContainerPath.isEmpty() || entry.subtitleSize == 0)) {
            invalidProject(QStringLiteral("Paket içindeki altyazı kaynağı geçersiz."));
        }
        entries.push_back(entry);
    }
    return entries;
}

void writeProjectFile(const QVector<VideoEntry>& entries, const QString& path,
                      const PackageMetadata& metadata)
{
    if (path.trimmed().isEmpty()) {
        throw std::invalid_argument("Proje taslağının yolu boş olamaz.");
    }

    QJsonArray videos;
    for (const VideoEntry& entry : entries) {
        if (entry.title.trimmed().isEmpty() || entry.category.trimmed().isEmpty()
            || entry.videoPath.isEmpty()) {
            throw std::invalid_argument("Taslakta eksik video bilgisi var.");
        }
        QJsonObject video{
            {QStringLiteral("id"), entry.id.isEmpty()
                 ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                 : entry.id},
            {QStringLiteral("title"), entry.title},
            {QStringLiteral("description"), entry.description},
            {QStringLiteral("category"), entry.category},
            {QStringLiteral("video_path"), entry.videoPath},
            {QStringLiteral("cover_path"), entry.coverPath},
            {QStringLiteral("asset_container_path"), entry.assetContainerPath},
            {QStringLiteral("video_offset"), static_cast<double>(entry.videoOffset)},
            {QStringLiteral("video_size"), static_cast<double>(entry.videoSize)},
            {QStringLiteral("cover_offset"), static_cast<double>(entry.coverOffset)},
            {QStringLiteral("cover_size"), static_cast<double>(entry.coverSize)},
            {QStringLiteral("video_is_embedded"), entry.videoIsEmbedded},
            {QStringLiteral("cover_is_embedded"), entry.coverIsEmbedded},
            {QStringLiteral("duration_ms"), entry.durationMs < 0
                 ? QJsonValue(QJsonValue::Null)
                 : QJsonValue(static_cast<double>(entry.durationMs))}
            ,{QStringLiteral("tags"), QJsonArray::fromStringList(entry.tags)}
            ,{QStringLiteral("collection"), entry.collection}
            ,{QStringLiteral("subtitle_path"), entry.subtitlePath}
            ,{QStringLiteral("subtitle_offset"), static_cast<double>(entry.subtitleOffset)}
            ,{QStringLiteral("subtitle_size"), static_cast<double>(entry.subtitleSize)}
            ,{QStringLiteral("subtitle_is_embedded"), entry.subtitleIsEmbedded}
            ,{QStringLiteral("video_sha256"), entry.videoSha256}
            ,{QStringLiteral("cover_sha256"), entry.coverSha256}
            ,{QStringLiteral("subtitle_sha256"), entry.subtitleSha256}
        };
        videos.append(video);
    }
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("packmanager.project")},
        {QStringLiteral("version"), 2},
        {QStringLiteral("package"), QJsonObject{
             {QStringLiteral("title"), metadata.title},
             {QStringLiteral("description"), metadata.description},
             {QStringLiteral("version"), metadata.version},
             {QStringLiteral("created_at"), metadata.createdAt}
         }},
        {QStringLiteral("videos"), videos}
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    qint64 written = 0;
    while (written < bytes.size()) {
        const qint64 count = file.write(bytes.constData() + written, bytes.size() - written);
        if (count <= 0) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        written += count;
    }
    if (!file.commit()) {
        throw std::runtime_error(file.errorString().toStdString());
    }
}
