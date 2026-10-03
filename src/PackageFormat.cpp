#include "PackageFormat.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

constexpr quint64 kMaxWriterJsonInteger = 9007199254740991ULL;

struct TensorSource {
    QString name;
    QString path;
    quint64 offset;
    quint64 size;
};

QString mediaTypeForPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("mp4") || suffix == QStringLiteral("m4v")) {
        return QStringLiteral("video/mp4");
    }
    if (suffix == QStringLiteral("mkv")) return QStringLiteral("video/x-matroska");
    if (suffix == QStringLiteral("mov")) return QStringLiteral("video/quicktime");
    if (suffix == QStringLiteral("webm")) return QStringLiteral("video/webm");
    if (suffix == QStringLiteral("avi")) return QStringLiteral("video/x-msvideo");
    if (suffix == QStringLiteral("wmv")) return QStringLiteral("video/x-ms-wmv");
    if (suffix == QStringLiteral("png")) return QStringLiteral("image/png");
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) {
        return QStringLiteral("image/jpeg");
    }
    if (suffix == QStringLiteral("webp")) return QStringLiteral("image/webp");
    if (suffix == QStringLiteral("bmp")) return QStringLiteral("image/bmp");
    if (suffix == QStringLiteral("srt")) return QStringLiteral("application/x-subrip");
    if (suffix == QStringLiteral("vtt")) return QStringLiteral("text/vtt");
    return QStringLiteral("application/octet-stream");
}

struct ExternalFolderGuard {
    QString path;
    bool keep = false;

    ~ExternalFolderGuard()
    {
        if (!keep && !path.isEmpty()) {
            QDir(path).removeRecursively();
        }
    }
};

void writeAll(QIODevice& output, const char* data, qint64 size)
{
    qint64 written = 0;
    while (written < size) {
        const qint64 count = output.write(data + written, size - written);
        if (count <= 0) {
            throw std::runtime_error(output.errorString().toStdString());
        }
        written += count;
    }
}

void copyRange(QIODevice& output, const QString& sourcePath,
               quint64 offset, quint64 size,
               quint64& bytesWritten, quint64 totalBytes,
               const std::function<void(quint64, quint64)>& progress)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(source.errorString().toStdString());
    }
    if (offset > static_cast<quint64>(source.size())
        || size > static_cast<quint64>(source.size()) - offset
        || !source.seek(static_cast<qint64>(offset))) {
        throw std::runtime_error("Kaynak medya aralığı okunamıyor.");
    }

    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    quint64 remaining = size;
    while (remaining > 0) {
        const qint64 requested = static_cast<qint64>(
            std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
        const qint64 count = source.read(buffer.data(), requested);
        if (count < 0) {
            throw std::runtime_error(source.errorString().toStdString());
        }
        if (count == 0) {
            throw std::runtime_error("Kaynak medya beklenmedik biçimde sona erdi.");
        }
        writeAll(output, buffer.constData(), count);
        remaining -= static_cast<quint64>(count);
        bytesWritten += static_cast<quint64>(count);
        if (progress) {
            progress(bytesWritten, totalBytes);
        }
    }
}

QByteArray sha256Range(const QString& sourcePath, quint64 offset, quint64 size)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)
        || offset > static_cast<quint64>(source.size())
        || size > static_cast<quint64>(source.size()) - offset
        || !source.seek(static_cast<qint64>(offset))) {
        throw std::runtime_error("Medya özeti için kaynak dosya okunamıyor.");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    quint64 remaining = size;
    while (remaining > 0) {
        const qint64 requested = static_cast<qint64>(
            std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
        const qint64 count = source.read(buffer.data(), requested);
        if (count <= 0) {
            throw std::runtime_error(source.errorString().toStdString());
        }
        hash.addData(buffer.constData(), count);
        remaining -= static_cast<quint64>(count);
    }
    return hash.result().toHex();
}

QString canonicalDestinationPath(const QString& path)
{
    const QFileInfo destinationInfo(path);
    if (destinationInfo.exists()) {
        return destinationInfo.canonicalFilePath();
    }
    const QString parentPath = QFileInfo(destinationInfo.absolutePath()).canonicalFilePath();
    if (parentPath.isEmpty()) {
        return destinationInfo.absoluteFilePath();
    }
    return parentPath + QLatin1Char('/') + destinationInfo.fileName();
}

} // namespace

void writeSafeTensors(const QVector<VideoEntry>& entries, const QString& destination,
                      const PackageMetadata& metadata,
                      const std::function<void(quint64, quint64)>& progress,
                      const PackageWriteOptions& options)
{
    if (entries.isEmpty()) {
        throw std::invalid_argument("Pakete eklenecek en az bir video olmalı.");
    }
    if (destination.trimmed().isEmpty()) {
        throw std::invalid_argument("Paket dosyasının yolu boş olamaz.");
    }

    QVector<TensorSource> tensors;
    QJsonArray videos;
    QJsonArray assets;
    QStringList sourcePaths;
    const QString resolvedDestination = canonicalDestinationPath(destination);
    QString externalFolderName;
    QString externalFolderPath;
    ExternalFolderGuard externalFolderGuard;
    quint64 externalTotalBytes = 0;
    quint64 externalBytesCopied = 0;
    if (!options.embedMedia) {
        externalFolderName = QFileInfo(destination).completeBaseName()
            + QStringLiteral(".media-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        externalFolderPath = QDir(QFileInfo(destination).absolutePath())
                                 .filePath(externalFolderName);
        if (!QDir().mkpath(externalFolderPath)) {
            throw std::runtime_error("Harici medya klasörü oluşturulamadı.");
        }
        externalFolderGuard.path = externalFolderPath;
        for (const VideoEntry& entry : entries) {
            const auto fileSize = [](const QString& path) {
                return static_cast<quint64>(
                    std::max<qint64>(0, QFileInfo(path).size()));
            };
            externalTotalBytes += entry.videoIsEmbedded
                ? entry.videoSize : fileSize(entry.videoPath);
            if (entry.coverIsEmbedded) {
                externalTotalBytes += entry.coverSize;
            } else if (!entry.coverPath.isEmpty()) {
                externalTotalBytes += fileSize(entry.coverPath);
            }
            if (entry.subtitleIsEmbedded) {
                externalTotalBytes += entry.subtitleSize;
            } else if (!entry.subtitlePath.isEmpty()) {
                externalTotalBytes += fileSize(entry.subtitlePath);
            }
        }
    }

    for (qsizetype index = 0; index < entries.size(); ++index) {
        const VideoEntry& entry = entries.at(index);
        if (entry.title.trimmed().isEmpty()) {
            throw std::invalid_argument("Video başlığı boş olamaz.");
        }
        if (entry.category.trimmed().isEmpty()) {
            throw std::invalid_argument("Video kategorisi boş olamaz.");
        }

        const QFileInfo videoInfo(entry.videoPath);
        const QString videoTensor = QString("video_%1").arg(index, 4, 10, QLatin1Char('0'));
        QString videoSourcePath;
        quint64 videoSourceOffset = 0;
        quint64 videoSourceSize = 0;
        if (entry.videoIsEmbedded) {
            if (entry.assetContainerPath.isEmpty() || entry.videoSize == 0) {
                throw std::invalid_argument("Paket içindeki video kaynağı geçersiz.");
            }
            if (options.embedMedia) {
                tensors.push_back({videoTensor, entry.assetContainerPath,
                                   entry.videoOffset, entry.videoSize});
            }
            videoSourcePath = entry.assetContainerPath;
            videoSourceOffset = entry.videoOffset;
            videoSourceSize = entry.videoSize;
        } else {
            if (!videoInfo.isFile() || !videoInfo.isReadable()) {
                throw std::runtime_error(
                    QString("Video dosyası bulunamadı veya okunamıyor: %1")
                        .arg(entry.videoPath)
                        .toStdString());
            }
            const QString videoCanonicalPath = videoInfo.canonicalFilePath();
            if (videoCanonicalPath == resolvedDestination) {
                throw std::invalid_argument("Paket dosyası kaynak videonun üzerine yazılamaz.");
            }
            const quint64 videoSize = static_cast<quint64>(videoInfo.size());
            if (videoSize == 0) {
                throw std::invalid_argument("Video dosyası boş olamaz.");
            }
            if (options.embedMedia) {
                tensors.push_back({videoTensor, videoInfo.absoluteFilePath(), 0, videoSize});
            }
            sourcePaths.push_back(videoCanonicalPath);
            videoSourcePath = videoInfo.absoluteFilePath();
            videoSourceSize = videoSize;
        }
        const QString videoSha256 = QString::fromLatin1(
            sha256Range(videoSourcePath, videoSourceOffset, videoSourceSize));
        const QString videoFilename = QFileInfo(entry.videoPath).fileName().isEmpty()
            ? QStringLiteral("%1.mp4").arg(entry.title)
            : QFileInfo(entry.videoPath).fileName();
        QString videoExternalPath;
        if (!options.embedMedia) {
            QString suffix = QFileInfo(entry.videoPath).suffix().toLower();
            if (suffix.isEmpty() || suffix.size() > 8
                || !std::all_of(suffix.cbegin(), suffix.cend(),
                                [](QChar value) { return value.isLetterOrNumber(); })) {
                suffix = QStringLiteral("bin");
            }
            const QString filename = QStringLiteral("video_%1.%2")
                                         .arg(index, 4, 10, QLatin1Char('0'))
                                         .arg(suffix);
            const QString target = QDir(externalFolderPath).filePath(filename);
            extractVideo(entry, target, [progress, &externalBytesCopied,
                                         externalTotalBytes](quint64 done, quint64) {
                if (progress) progress(externalBytesCopied + done, externalTotalBytes);
            });
            externalBytesCopied += videoSourceSize;
            if (progress) progress(externalBytesCopied, externalTotalBytes);
            videoExternalPath = externalFolderName + QLatin1Char('/') + filename;
        }

        QString coverTensor;
        QString coverSha256;
        quint64 coverSourceSize = 0;
        if (entry.coverIsEmbedded) {
            if (entry.assetContainerPath.isEmpty() || entry.coverSize == 0) {
                throw std::invalid_argument("Paket içindeki kapak kaynağı geçersiz.");
            }
            coverTensor = QString("cover_%1").arg(index, 4, 10, QLatin1Char('0'));
            coverSourceSize = entry.coverSize;
            if (options.embedMedia) {
                tensors.push_back({coverTensor, entry.assetContainerPath,
                                   entry.coverOffset, entry.coverSize});
            }
            coverSha256 = QString::fromLatin1(
                sha256Range(entry.assetContainerPath, entry.coverOffset, entry.coverSize));
        } else if (!entry.coverPath.trimmed().isEmpty()) {
            const QFileInfo coverInfo(entry.coverPath);
            if (!coverInfo.isFile() || !coverInfo.isReadable()) {
                throw std::runtime_error(
                    QString("Kapak resmi bulunamadı veya okunamıyor: %1")
                        .arg(entry.coverPath)
                        .toStdString());
            }
            const QString coverCanonicalPath = coverInfo.canonicalFilePath();
            if (coverCanonicalPath == resolvedDestination) {
                throw std::invalid_argument("Paket dosyası kaynak kapak resminin üzerine yazılamaz.");
            }

            coverTensor = QString("cover_%1").arg(index, 4, 10, QLatin1Char('0'));
            const quint64 coverSize = static_cast<quint64>(coverInfo.size());
            coverSourceSize = coverSize;
            if (coverSize == 0) {
                throw std::invalid_argument("Kapak resmi dosyası boş olamaz.");
            }
            if (options.embedMedia) {
                tensors.push_back({coverTensor, coverInfo.absoluteFilePath(), 0, coverSize});
            }
            sourcePaths.push_back(coverCanonicalPath);
            coverSha256 = QString::fromLatin1(
                sha256Range(coverInfo.absoluteFilePath(), 0, coverSize));
        }
        QString coverExternalPath;
        if (!options.embedMedia && !coverTensor.isEmpty()) {
            QString suffix = QFileInfo(entry.coverPath).suffix().toLower();
            if (suffix.isEmpty() || suffix.size() > 8
                || !std::all_of(suffix.cbegin(), suffix.cend(),
                                [](QChar value) { return value.isLetterOrNumber(); })) {
                suffix = QStringLiteral("img");
            }
            const QString filename = QStringLiteral("cover_%1.%2")
                                         .arg(index, 4, 10, QLatin1Char('0'))
                                         .arg(suffix);
            const QString target = QDir(externalFolderPath).filePath(filename);
            extractCover(entry, target);
            externalBytesCopied += entry.coverIsEmbedded
                ? entry.coverSize
                : static_cast<quint64>(std::max<qint64>(
                      0, QFileInfo(entry.coverPath).size()));
            if (progress) progress(externalBytesCopied, externalTotalBytes);
            coverExternalPath = externalFolderName + QLatin1Char('/') + filename;
        }

        QString subtitleTensor;
        QString subtitleSha256;
        quint64 subtitleSourceSize = 0;
        if (entry.subtitleIsEmbedded) {
            if (entry.assetContainerPath.isEmpty() || entry.subtitleSize == 0) {
                throw std::invalid_argument("Paket içindeki altyazı kaynağı geçersiz.");
            }
            subtitleTensor = QString("subtitle_%1").arg(index, 4, 10, QLatin1Char('0'));
            subtitleSourceSize = entry.subtitleSize;
            if (options.embedMedia) {
                tensors.push_back({subtitleTensor, entry.assetContainerPath,
                                   entry.subtitleOffset, entry.subtitleSize});
            }
            subtitleSha256 = QString::fromLatin1(
                sha256Range(entry.assetContainerPath, entry.subtitleOffset,
                            entry.subtitleSize));
        } else if (!entry.subtitlePath.trimmed().isEmpty()) {
            const QFileInfo subtitleInfo(entry.subtitlePath);
            if (!subtitleInfo.isFile() || !subtitleInfo.isReadable()) {
                throw std::runtime_error(
                    QString("Altyazı dosyası bulunamadı veya okunamıyor: %1")
                        .arg(entry.subtitlePath).toStdString());
            }
            subtitleTensor = QString("subtitle_%1").arg(index, 4, 10, QLatin1Char('0'));
            const quint64 subtitleSize = static_cast<quint64>(subtitleInfo.size());
            subtitleSourceSize = subtitleSize;
            if (subtitleSize == 0) {
                throw std::invalid_argument("Altyazı dosyası boş olamaz.");
            }
            if (options.embedMedia) {
                tensors.push_back({subtitleTensor, subtitleInfo.absoluteFilePath(), 0,
                                   subtitleSize});
            }
            sourcePaths.push_back(subtitleInfo.canonicalFilePath());
            subtitleSha256 = QString::fromLatin1(
                sha256Range(subtitleInfo.absoluteFilePath(), 0, subtitleSize));
        }
        QString subtitleExternalPath;
        if (!options.embedMedia && !subtitleTensor.isEmpty()) {
            QString suffix = QFileInfo(entry.subtitlePath).suffix().toLower();
            if (suffix != QStringLiteral("srt") && suffix != QStringLiteral("vtt")) {
                suffix = QStringLiteral("srt");
            }
            const QString filename = QStringLiteral("subtitle_%1.%2")
                                         .arg(index, 4, 10, QLatin1Char('0'))
                                         .arg(suffix);
            const QString target = QDir(externalFolderPath).filePath(filename);
            extractSubtitle(entry, target);
            externalBytesCopied += entry.subtitleIsEmbedded
                ? entry.subtitleSize
                : static_cast<quint64>(std::max<qint64>(
                      0, QFileInfo(entry.subtitlePath).size()));
            if (progress) progress(externalBytesCopied, externalTotalBytes);
            subtitleExternalPath = externalFolderName + QLatin1Char('/') + filename;
        }

        QJsonObject video{
            {QStringLiteral("title"), entry.title.trimmed()},
            {QStringLiteral("description"), entry.description.trimmed()},
            {QStringLiteral("category"), entry.category.trimmed()},
            {QStringLiteral("id"), entry.id.isEmpty()
                 ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                 : entry.id},
            {QStringLiteral("video_filename"), videoFilename},
            {QStringLiteral("video_tensor"),
             options.embedMedia ? QJsonValue(videoTensor) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("cover_tensor"),
             coverTensor.isEmpty() || !options.embedMedia
                 ? QJsonValue(QJsonValue::Null) : QJsonValue(coverTensor)},
            {QStringLiteral("video_sha256"), videoSha256},
            {QStringLiteral("cover_sha256"), coverSha256},
            {QStringLiteral("subtitle_tensor"),
             subtitleTensor.isEmpty() || !options.embedMedia
                 ? QJsonValue(QJsonValue::Null) : QJsonValue(subtitleTensor)},
            {QStringLiteral("subtitle_sha256"), subtitleSha256},
            {QStringLiteral("tags"), QJsonArray::fromStringList(entry.tags)},
            {QStringLiteral("collection"), entry.collection}
        };
        if (!options.embedMedia) {
            video.insert(QStringLiteral("video_external_path"), videoExternalPath);
            video.insert(QStringLiteral("cover_external_path"), coverExternalPath);
            video.insert(QStringLiteral("subtitle_external_path"), subtitleExternalPath);
        }
        if (entry.durationMs >= 0) {
            video.insert(QStringLiteral("duration_ms"), static_cast<double>(entry.durationMs));
        }
        if (!coverTensor.isEmpty()) {
            const QString coverFilename = QFileInfo(entry.coverPath).fileName().isEmpty()
                ? QStringLiteral("%1-cover.img").arg(entry.title)
                : QFileInfo(entry.coverPath).fileName();
            video.insert(QStringLiteral("cover_filename"), coverFilename);
        }
        if (!subtitleTensor.isEmpty()) {
            const QString subtitleFilename = QFileInfo(entry.subtitlePath).fileName().isEmpty()
                ? QStringLiteral("%1.srt").arg(entry.title)
                : QFileInfo(entry.subtitlePath).fileName();
            video.insert(QStringLiteral("subtitle_filename"), subtitleFilename);
        }
        const QString videoId = video.value(QStringLiteral("id")).toString();
        const QString videoStorage = options.embedMedia
            ? QStringLiteral("tensor") : QStringLiteral("external");
        assets.append(QJsonObject{
            {QStringLiteral("id"), videoId + QStringLiteral(":video")},
            {QStringLiteral("role"), QStringLiteral("video")},
            {QStringLiteral("tensor"), options.embedMedia
                 ? QJsonValue(videoTensor) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("filename"), videoFilename},
            {QStringLiteral("media_type"), mediaTypeForPath(videoFilename)},
            {QStringLiteral("size_bytes"), static_cast<double>(videoSourceSize)},
            {QStringLiteral("sha256"), videoSha256},
            {QStringLiteral("storage"), videoStorage}
        });
        if (!coverTensor.isEmpty()) {
            const QString coverFilename = video.value(
                QStringLiteral("cover_filename")).toString();
            assets.append(QJsonObject{
                {QStringLiteral("id"), videoId + QStringLiteral(":cover")},
                {QStringLiteral("role"), QStringLiteral("cover")},
                {QStringLiteral("tensor"), options.embedMedia
                     ? QJsonValue(coverTensor) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("filename"), coverFilename},
                {QStringLiteral("media_type"), mediaTypeForPath(coverFilename)},
                {QStringLiteral("size_bytes"), static_cast<double>(coverSourceSize)},
                {QStringLiteral("sha256"), coverSha256},
                {QStringLiteral("storage"), videoStorage}
            });
        }
        if (!subtitleTensor.isEmpty()) {
            const QString subtitleFilename = video.value(
                QStringLiteral("subtitle_filename")).toString();
            assets.append(QJsonObject{
                {QStringLiteral("id"), videoId + QStringLiteral(":subtitle")},
                {QStringLiteral("role"), QStringLiteral("subtitle")},
                {QStringLiteral("tensor"), options.embedMedia
                     ? QJsonValue(subtitleTensor) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("filename"), subtitleFilename},
                {QStringLiteral("media_type"), mediaTypeForPath(subtitleFilename)},
                {QStringLiteral("size_bytes"), static_cast<double>(subtitleSourceSize)},
                {QStringLiteral("sha256"), subtitleSha256},
                {QStringLiteral("storage"), videoStorage}
            });
        }
        videos.append(video);
    }

    if (sourcePaths.contains(resolvedDestination)) {
        throw std::invalid_argument("Paket dosyası kaynak medya dosyasıyla aynı olamaz.");
    }

    QJsonObject manifest{
        {QStringLiteral("format"), QStringLiteral("packmanager.video-pack")},
        {QStringLiteral("version"), 2},
        {QStringLiteral("media_storage"),
         options.embedMedia ? QStringLiteral("embedded") : QStringLiteral("external")},
        {QStringLiteral("package"), QJsonObject{
             {QStringLiteral("title"), metadata.title},
             {QStringLiteral("description"), metadata.description},
             {QStringLiteral("version"), metadata.version},
             {QStringLiteral("created_at"), metadata.createdAt.isEmpty()
                  ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                  : metadata.createdAt}
         }},
        {QStringLiteral("videos"), videos},
        {QStringLiteral("assets"), assets}
    };
    if (!options.embedMedia) {
        manifest.insert(QStringLiteral("media_folder"), externalFolderName);
    }
    if (!options.signingAuthor.trimmed().isEmpty()) {
        const PackageSignature signature = signPackageDigest(packageSigningDigest(manifest));
        QJsonObject package = manifest.value(QStringLiteral("package")).toObject();
        package.insert(QStringLiteral("signature_author"), options.signingAuthor.trimmed());
        package.insert(QStringLiteral("signature_public_key"),
                       QString::fromLatin1(signature.publicKey.toBase64()));
        package.insert(QStringLiteral("signature"),
                       QString::fromLatin1(signature.signature.toBase64()));
        manifest.insert(QStringLiteral("package"), package);
    }
    const QString manifestText = QString::fromUtf8(
        QJsonDocument(manifest).toJson(QJsonDocument::Compact));

    QJsonObject header;
    header.insert(QStringLiteral("__metadata__"),
                  QJsonObject{
                      {QStringLiteral("format"), QStringLiteral("packmanager.video-pack")},
                      {QStringLiteral("packmanager_manifest"), manifestText}
                  });
    quint64 offset = 0;
    for (const TensorSource& tensor : tensors) {
        if (tensor.size > kMaxWriterJsonInteger
            || offset > kMaxWriterJsonInteger
            || tensor.size > kMaxWriterJsonInteger - offset) {
            throw std::overflow_error("Paket boyutu çok büyük.");
        }
        header.insert(tensor.name,
                      QJsonObject{
                          {QStringLiteral("dtype"), QStringLiteral("U8")},
                          {QStringLiteral("shape"),
                           QJsonArray{static_cast<double>(tensor.size)}},
                          {QStringLiteral("data_offsets"),
                           QJsonArray{static_cast<double>(offset),
                                      static_cast<double>(offset + tensor.size)}}
                      });
        offset += tensor.size;
    }

    QByteArray headerBytes = QJsonDocument(header).toJson(QJsonDocument::Compact);
    const qsizetype padding = (8 - (headerBytes.size() % 8)) % 8;
    headerBytes.append(QByteArray(padding, ' '));
    if (headerBytes.size() > 100 * 1024 * 1024) {
        throw std::overflow_error("SafeTensors başlığı 100 MiB sınırını aşıyor.");
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(output.errorString().toStdString());
    }

    const quint64 headerSize = static_cast<quint64>(headerBytes.size());
    char prefix[8];
    for (int byte = 0; byte < 8; ++byte) {
        prefix[byte] = static_cast<char>((headerSize >> (byte * 8)) & 0xff);
    }
    writeAll(output, prefix, sizeof(prefix));
    writeAll(output, headerBytes.constData(), headerBytes.size());
    quint64 totalBytes = 0;
    for (const TensorSource& tensor : tensors) {
        totalBytes += tensor.size;
    }
    quint64 bytesWritten = 0;
    for (const TensorSource& tensor : tensors) {
        copyRange(output, tensor.path, tensor.offset, tensor.size,
                  bytesWritten, totalBytes, progress);
    }
    if (!output.commit()) {
        throw std::runtime_error(output.errorString().toStdString());
    }
    externalFolderGuard.keep = true;
}

QByteArray readVideoCover(const VideoEntry& entry)
{
    if (!entry.coverIsEmbedded) {
        QFile image(entry.coverPath);
        if (entry.coverPath.isEmpty()) {
            return {};
        }
        if (!image.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(
                QString("Kapak resmi okunamıyor: %1").arg(image.errorString()).toStdString());
        }
        if (image.size() > 64 * 1024 * 1024) {
            throw std::runtime_error("Kapak resmi 64 MiB boyut sınırını aşıyor.");
        }
        return image.readAll();
    }
    if (entry.coverSize > 64ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("Kapak resmi 64 MiB boyut sınırını aşıyor.");
    }

    QFile source(entry.assetContainerPath);
    if (!source.open(QIODevice::ReadOnly)
        || entry.coverOffset > static_cast<quint64>(source.size())
        || entry.coverSize > static_cast<quint64>(source.size()) - entry.coverOffset
        || !source.seek(static_cast<qint64>(entry.coverOffset))) {
        throw std::runtime_error("Paket içindeki kapak resmi okunamıyor.");
    }
    const QByteArray image = source.read(static_cast<qint64>(entry.coverSize));
    if (static_cast<quint64>(image.size()) != entry.coverSize) {
        throw std::runtime_error("Paket içindeki kapak resmi eksik okunabildi.");
    }
    return image;
}

QByteArray readEntrySubtitle(const VideoEntry& entry)
{
    QString path = entry.subtitlePath;
    quint64 offset = 0;
    quint64 size = 0;
    if (entry.subtitleIsEmbedded) {
        path = entry.assetContainerPath;
        offset = entry.subtitleOffset;
        size = entry.subtitleSize;
    } else {
        if (path.isEmpty()) {
            return {};
        }
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable()) {
            throw std::runtime_error("Altyazı dosyası bulunamadı veya okunamıyor.");
        }
        size = static_cast<quint64>(info.size());
    }
    if (size > 32ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error("Altyazı dosyası 32 MiB boyut sınırını aşıyor.");
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)
        || offset > static_cast<quint64>(source.size())
        || size > static_cast<quint64>(source.size()) - offset
        || !source.seek(static_cast<qint64>(offset))) {
        throw std::runtime_error("Altyazı kaynağı okunamıyor.");
    }
    const QByteArray bytes = source.read(static_cast<qint64>(size));
    if (static_cast<quint64>(bytes.size()) != size) {
        throw std::runtime_error("Altyazı kaynağı eksik okunabildi.");
    }
    return bytes;
}

void extractVideo(
    const VideoEntry& entry, const QString& destination,
    const std::function<void(quint64, quint64)>& progress)
{
    QString sourcePath = entry.videoPath;
    quint64 offset = 0;
    quint64 size = 0;
    if (entry.videoIsEmbedded) {
        sourcePath = entry.assetContainerPath;
        offset = entry.videoOffset;
        size = entry.videoSize;
    } else {
        const QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.isFile() || !sourceInfo.isReadable()) {
            throw std::runtime_error("Video dosyası bulunamadı veya okunamıyor.");
        }
        size = static_cast<quint64>(sourceInfo.size());
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(output.errorString().toStdString());
    }
    quint64 bytesWritten = 0;
    copyRange(output, sourcePath, offset, size, bytesWritten, size, progress);
    if (!output.commit()) {
        throw std::runtime_error(output.errorString().toStdString());
    }
}

void extractCover(const VideoEntry& entry, const QString& destination)
{
    const QByteArray image = readVideoCover(entry);
    if (image.isEmpty()) {
        throw std::runtime_error("Video kapağı bulunamadı veya boş.");
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(output.errorString().toStdString());
    }
    writeAll(output, image.constData(), image.size());
    if (!output.commit()) {
        throw std::runtime_error(output.errorString().toStdString());
    }
}

void extractSubtitle(const VideoEntry& entry, const QString& destination)
{
    const QByteArray bytes = readEntrySubtitle(entry);
    if (bytes.isEmpty()) {
        throw std::runtime_error("Altyazı bulunamadı veya boş.");
    }
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(output.errorString().toStdString());
    }
    writeAll(output, bytes.constData(), bytes.size());
    if (!output.commit()) {
        throw std::runtime_error(output.errorString().toStdString());
    }
}

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

constexpr quint64 kMaxHeaderSize = 100ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxExactJsonInteger = 9007199254740991ULL;

struct TensorRange {
    quint64 offset;
    quint64 size;
};

[[noreturn]] void invalidPackage(const QString& detail)
{
    throw std::runtime_error(
        QString("Geçersiz PackManager paketi: %1").arg(detail).toStdString());
}

quint64 jsonUnsignedInteger(const QJsonValue& value, const QString& field)
{
    if (!value.isDouble()) {
        invalidPackage(QString("%1 sayı değil.").arg(field));
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0.0
        || std::floor(number) != number
        || number > static_cast<double>(kMaxExactJsonInteger)) {
        invalidPackage(QString("%1 geçerli bir tam sayı değil.").arg(field));
    }
    return static_cast<quint64>(number);
}

QString requiredString(const QJsonObject& object, const QString& field)
{
    const QJsonValue value = object.value(field);
    if (!value.isString() || value.toString().trimmed().isEmpty()) {
        invalidPackage(QString("%1 metaverisi eksik veya geçersiz.").arg(field));
    }
    return value.toString();
}

QString optionalString(const QJsonObject& object, const QString& field)
{
    const QJsonValue value = object.value(field);
    if (value.isUndefined() || value.isNull()) {
        return {};
    }
    if (!value.isString()) {
        invalidPackage(QString("%1 metaverisi metin değil.").arg(field));
    }
    return value.toString();
}

QString resolveExternalAssetPath(const QString& packagePath, const QString& relativePath,
                                 const QString& field)
{
    if (relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)
        || relativePath.contains(QLatin1Char(':'))) {
        invalidPackage(QString("%1 yolu geçersiz.").arg(field));
    }
    const QStringList segments = relativePath.split(
        QRegularExpression(QStringLiteral("[/\\\\]")), Qt::KeepEmptyParts);
    if (std::any_of(segments.cbegin(), segments.cend(), [](const QString& segment) {
            return segment.isEmpty() || segment == QStringLiteral(".")
                || segment == QStringLiteral("..");
        })) {
        invalidPackage(QString("%1 yolu paket klasörünün dışına çıkıyor.").arg(field));
    }
    const QString packageDirectory = QFileInfo(packagePath).absolutePath();
    const QString resolved = QDir::cleanPath(QDir(packageDirectory).absoluteFilePath(relativePath));
    const QFileInfo assetInfo(resolved);
    if (assetInfo.exists()) {
        const QString canonicalDirectory = QDir::fromNativeSeparators(
            QFileInfo(packageDirectory).canonicalFilePath());
        const QString canonicalAsset = QDir::fromNativeSeparators(
            assetInfo.canonicalFilePath());
        const QString relativeAsset =
            QDir(canonicalDirectory).relativeFilePath(canonicalAsset);
        if (canonicalDirectory.isEmpty()
            || canonicalAsset.isEmpty()
            || relativeAsset == QStringLiteral("..")
            || relativeAsset.startsWith(QStringLiteral("../"))
            || QDir::isAbsolutePath(relativeAsset)) {
            invalidPackage(QString("%1 yolu paket klasörünün dışına yöneliyor.").arg(field));
        }
    }
    return resolved;
}

QStringList optionalStringList(const QJsonObject& object, const QString& field)
{
    const QJsonValue value = object.value(field);
    if (value.isUndefined()) {
        return {};
    }
    if (!value.isArray()) {
        invalidPackage(QString("%1 metaverisi liste değil.").arg(field));
    }
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString() || item.toString().trimmed().isEmpty()) {
            invalidPackage(QString("%1 metaverisinde geçersiz öğe.").arg(field));
        }
        if (!result.contains(item.toString())) {
            result.push_back(item.toString());
        }
    }
    return result;
}

TensorRange tensorRange(const QJsonObject& header, const QString& name)
{
    const QJsonValue tensorValue = header.value(name);
    if (!tensorValue.isObject()) {
        invalidPackage(QString("'%1' tensor'u bulunamadı.").arg(name));
    }
    const QJsonObject tensor = tensorValue.toObject();
    if (tensor.value(QStringLiteral("dtype")).toString() != QStringLiteral("U8")) {
        invalidPackage(QString("'%1' tensor'u U8 biçiminde değil.").arg(name));
    }

    const QJsonValue shapeValue = tensor.value(QStringLiteral("shape"));
    const QJsonValue offsetsValue = tensor.value(QStringLiteral("data_offsets"));
    if (!shapeValue.isArray() || !offsetsValue.isArray()) {
        invalidPackage(QString("'%1' tensor boyutları eksik.").arg(name));
    }
    const QJsonArray shape = shapeValue.toArray();
    const QJsonArray offsets = offsetsValue.toArray();
    if (shape.size() != 1 || offsets.size() != 2) {
        invalidPackage(QString("'%1' tensor boyutları desteklenmiyor.").arg(name));
    }

    const quint64 size = jsonUnsignedInteger(shape.at(0), QStringLiteral("shape"));
    const quint64 begin = jsonUnsignedInteger(offsets.at(0), QStringLiteral("data_offsets"));
    const quint64 end = jsonUnsignedInteger(offsets.at(1), QStringLiteral("data_offsets"));
    if (end < begin || end - begin != size) {
        invalidPackage(QString("'%1' tensor boyutu ile veri aralığı uyuşmuyor.").arg(name));
    }
    return {begin, size};
}

void readExact(QFile& file, char* target, qint64 size)
{
    qint64 read = 0;
    while (read < size) {
        const qint64 count = file.read(target + read, size - read);
        if (count <= 0) {
            invalidPackage(QString("Dosya beklenmedik biçimde sona erdi: %1")
                               .arg(file.errorString()));
        }
        read += count;
    }
}

} // namespace

QVector<VideoEntry> readSafeTensors(
    const QString& source, SafeTensorsValidationReport* report,
    PackageMetadata* packageMetadata)
{
    if (report != nullptr) {
        *report = {};
    }
    if (packageMetadata != nullptr) {
        *packageMetadata = {};
    }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toStdString());
    }
    if (file.size() < 8) {
        invalidPackage(QStringLiteral("Dosya SafeTensors başlığı içermiyor."));
    }

    char prefix[8];
    readExact(file, prefix, sizeof(prefix));
    quint64 headerSize = 0;
    for (int byte = 0; byte < 8; ++byte) {
        headerSize |= static_cast<quint64>(
                          static_cast<unsigned char>(prefix[byte]))
                      << (byte * 8);
    }
    if (headerSize == 0 || headerSize % 8 != 0 || headerSize > kMaxHeaderSize
        || headerSize > static_cast<quint64>(file.size() - 8)) {
        invalidPackage(QStringLiteral("Başlık boyutu geçersiz."));
    }

    const QByteArray headerBytes = file.read(static_cast<qint64>(headerSize));
    if (static_cast<quint64>(headerBytes.size()) != headerSize) {
        invalidPackage(QStringLiteral("Başlık eksik okunabildi."));
    }
    QJsonParseError parseError;
    const QJsonDocument headerDocument = QJsonDocument::fromJson(headerBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !headerDocument.isObject()) {
        invalidPackage(QStringLiteral("Başlık JSON biçiminde değil."));
    }
    const QJsonObject header = headerDocument.object();

    const QJsonValue metadataValue = header.value(QStringLiteral("__metadata__"));
    if (!metadataValue.isObject()) {
        invalidPackage(QStringLiteral("PackManager JSON metaverisi bulunamadı."));
    }
    const QJsonValue manifestValue =
        metadataValue.toObject().value(QStringLiteral("packmanager_manifest"));
    if (!manifestValue.isString()) {
        invalidPackage(QStringLiteral("PackManager JSON listesi bulunamadı."));
    }
    const QByteArray manifestBytes = manifestValue.toString().toUtf8();
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !manifestDocument.isObject()) {
        invalidPackage(QStringLiteral("PackManager JSON listesi geçersiz."));
    }
    const QJsonObject manifest = manifestDocument.object();
    const quint64 manifestVersion = jsonUnsignedInteger(
        manifest.value(QStringLiteral("version")), QStringLiteral("version"));
    if (manifest.value(QStringLiteral("format")).toString()
            != QStringLiteral("packmanager.video-pack")
        || (manifestVersion != 1 && manifestVersion != 2)) {
        invalidPackage(QStringLiteral("Desteklenmeyen paket biçimi veya sürümü."));
    }
    const QJsonValue packageValue = manifest.value(QStringLiteral("package"));
    if (!packageValue.isUndefined() && !packageValue.isObject()) {
        invalidPackage(QStringLiteral("Paket bilgileri geçersiz."));
    }
    const QJsonObject package = packageValue.toObject();
    const QString signatureAuthor =
        package.value(QStringLiteral("signature_author")).toString();
    const QByteArray signaturePublicKey = QByteArray::fromBase64(
        package.value(QStringLiteral("signature_public_key")).toString().toLatin1());
    const QByteArray signatureValue = QByteArray::fromBase64(
        package.value(QStringLiteral("signature")).toString().toLatin1());
    const bool signaturePresent = !signatureAuthor.isEmpty()
        || !signaturePublicKey.isEmpty() || !signatureValue.isEmpty();
    const bool signatureVerified = signaturePresent
        && !signatureAuthor.trimmed().isEmpty()
        && !signaturePublicKey.isEmpty() && !signatureValue.isEmpty()
        && verifyPackageDigest(packageSigningDigest(manifest),
                               signaturePublicKey, signatureValue);
    if (signaturePresent && !signatureVerified && report != nullptr) {
        report->issues.push_back(
            QStringLiteral("Paket dijital imzası geçersiz veya eksik."));
    }
    if (packageMetadata != nullptr) {
        packageMetadata->title = package.value(QStringLiteral("title")).toString();
        packageMetadata->description =
            package.value(QStringLiteral("description")).toString();
        packageMetadata->version = package.value(QStringLiteral("version"))
            .toString(QStringLiteral("1.0"));
        packageMetadata->createdAt =
            package.value(QStringLiteral("created_at")).toString();
        packageMetadata->signatureAuthor = signatureAuthor;
        packageMetadata->signaturePublicKey = signaturePublicKey;
        packageMetadata->signatureValue = signatureValue;
        packageMetadata->signaturePresent = signaturePresent;
        packageMetadata->signatureVerified = signatureVerified;
    }
    const QJsonValue videosValue = manifest.value(QStringLiteral("videos"));
    if (!videosValue.isArray() || videosValue.toArray().isEmpty()) {
        invalidPackage(QStringLiteral("Pakette video bulunamadı."));
    }

    const quint64 dataStart = 8 + headerSize;
    const quint64 dataSize = static_cast<quint64>(file.size()) - dataStart;

    QVector<TensorRange> allRanges;
    allRanges.reserve(header.size() - 1);
    for (auto it = header.constBegin(); it != header.constEnd(); ++it) {
        if (it.key() == QStringLiteral("__metadata__")) {
            continue;
        }
        const TensorRange range = tensorRange(header, it.key());
        if (range.offset > dataSize || range.size > dataSize - range.offset) {
            invalidPackage(QString("'%1' tensor dosya sınırlarının dışında.")
                               .arg(it.key()));
        }
        allRanges.push_back(range);
    }
    std::sort(allRanges.begin(), allRanges.end(),
              [](const TensorRange& left, const TensorRange& right) {
                  return left.offset < right.offset;
              });
    quint64 expectedOffset = 0;
    for (const TensorRange& range : allRanges) {
        if (range.offset != expectedOffset) {
            invalidPackage(QStringLiteral("Tensor veri aralıkları eksik veya çakışıyor."));
        }
        expectedOffset += range.size;
    }
    if (expectedOffset != dataSize) {
        invalidPackage(QStringLiteral("Paketin tensor veri boyutu geçersiz."));
    }

    QVector<VideoEntry> entries;
    QSet<QString> referencedTensors;
    const QJsonArray videos = videosValue.toArray();
    entries.reserve(videos.size());
    for (qsizetype index = 0; index < videos.size(); ++index) {
        if (!videos.at(index).isObject()) {
            invalidPackage(QStringLiteral("Video metaverisi geçersiz."));
        }
        const QJsonObject video = videos.at(index).toObject();
        const QJsonValue videoTensorValue = video.value(QStringLiteral("video_tensor"));
        const QString videoExternalPath =
            optionalString(video, QStringLiteral("video_external_path"));
        TensorRange videoRange{};
        if (videoTensorValue.isString() && !videoTensorValue.toString().isEmpty()) {
            const QString videoTensor = videoTensorValue.toString();
            if (referencedTensors.contains(videoTensor)) {
                invalidPackage(QStringLiteral("Bir video tensor'u birden çok kez kullanılmış."));
            }
            referencedTensors.insert(videoTensor);
            videoRange = tensorRange(header, videoTensor);
            if (videoRange.size == 0) {
                invalidPackage(QStringLiteral("Video tensor'u boş."));
            }
        } else if (!videoTensorValue.isNull() && !videoTensorValue.isUndefined()) {
            invalidPackage(QStringLiteral("Video tensor'u geçersiz."));
        } else if (videoExternalPath.isEmpty()) {
            invalidPackage(QStringLiteral("Video tensor'u veya harici video yolu bulunamadı."));
        }

        VideoEntry entry;
        entry.title = requiredString(video, QStringLiteral("title"));
        const QJsonValue descriptionValue = video.value(QStringLiteral("description"));
        if (!descriptionValue.isString()) {
            invalidPackage(QStringLiteral("description metaverisi eksik veya geçersiz."));
        }
        entry.description = descriptionValue.toString();
        entry.category = requiredString(video, QStringLiteral("category"));
        entry.tags = optionalStringList(video, QStringLiteral("tags"));
        entry.collection = optionalString(video, QStringLiteral("collection"));
        entry.videoSha256 = optionalString(video, QStringLiteral("video_sha256"));
        entry.coverSha256 = optionalString(video, QStringLiteral("cover_sha256"));
        entry.subtitleSha256 = optionalString(video, QStringLiteral("subtitle_sha256"));
        const QJsonValue idValue = video.value(QStringLiteral("id"));
        if (!idValue.isUndefined() && !idValue.isString()) {
            invalidPackage(QStringLiteral("id metaverisi geçersiz."));
        }
        entry.id = idValue.toString();
        if (entry.id.isEmpty()) {
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        const QJsonValue durationValue = video.value(QStringLiteral("duration_ms"));
        if (!durationValue.isUndefined() && !durationValue.isNull()) {
            entry.durationMs = static_cast<qint64>(
                jsonUnsignedInteger(durationValue, QStringLiteral("duration_ms")));
        }
        const QJsonValue videoFilenameValue = video.value(QStringLiteral("video_filename"));
        if (!videoFilenameValue.isUndefined() && !videoFilenameValue.isString()) {
            invalidPackage(QStringLiteral("video_filename metaverisi geçersiz."));
        }
        entry.videoPath = videoFilenameValue.toString();
        if (entry.videoPath.isEmpty()) entry.videoPath = QString("%1.mp4").arg(entry.title);
        entry.assetContainerPath = QFileInfo(source).absoluteFilePath();
        if (videoTensorValue.isString() && !videoTensorValue.toString().isEmpty()) {
            entry.videoOffset = dataStart + videoRange.offset;
            entry.videoSize = videoRange.size;
            entry.videoIsEmbedded = true;
        } else {
            entry.videoPath = resolveExternalAssetPath(
                source, videoExternalPath, QStringLiteral("Harici video"));
            entry.videoIsEmbedded = false;
            entry.videoSize = static_cast<quint64>(
                std::max<qint64>(0, QFileInfo(entry.videoPath).size()));
        }

        const QJsonValue coverTensorValue = video.value(QStringLiteral("cover_tensor"));
        const QString coverExternalPath =
            optionalString(video, QStringLiteral("cover_external_path"));
        if (!coverTensorValue.isNull() && !coverTensorValue.isUndefined()) {
            if (!coverTensorValue.isString() || coverTensorValue.toString().isEmpty()) {
                invalidPackage(QStringLiteral("Kapak tensor'u geçersiz."));
            }
            const QString coverTensor = coverTensorValue.toString();
            if (referencedTensors.contains(coverTensor)) {
                invalidPackage(QStringLiteral("Bir kapak tensor'u birden çok kez kullanılmış."));
            }
            referencedTensors.insert(coverTensor);
            const TensorRange coverRange = tensorRange(header, coverTensor);
            if (coverRange.size == 0) {
                invalidPackage(QStringLiteral("Kapak tensor'u boş."));
            }
            const QJsonValue coverFilenameValue = video.value(QStringLiteral("cover_filename"));
            if (!coverFilenameValue.isUndefined() && !coverFilenameValue.isString()) {
                invalidPackage(QStringLiteral("cover_filename metaverisi geçersiz."));
            }
            entry.coverPath = coverFilenameValue.toString();
            if (entry.coverPath.isEmpty()) {
                entry.coverPath = QString("%1.img").arg(entry.title);
            }
            entry.coverOffset = dataStart + coverRange.offset;
            entry.coverSize = coverRange.size;
            entry.coverIsEmbedded = true;
        } else if (!coverExternalPath.isEmpty()) {
            entry.coverPath = resolveExternalAssetPath(
                source, coverExternalPath, QStringLiteral("Harici kapak"));
            entry.coverIsEmbedded = false;
            entry.coverSize = static_cast<quint64>(
                std::max<qint64>(0, QFileInfo(entry.coverPath).size()));
        }
        const QJsonValue subtitleTensorValue = video.value(QStringLiteral("subtitle_tensor"));
        const QString subtitleExternalPath =
            optionalString(video, QStringLiteral("subtitle_external_path"));
        if (!subtitleTensorValue.isNull() && !subtitleTensorValue.isUndefined()) {
            if (!subtitleTensorValue.isString() || subtitleTensorValue.toString().isEmpty()) {
                invalidPackage(QStringLiteral("Altyazı tensor'u geçersiz."));
            }
            const QString subtitleTensor = subtitleTensorValue.toString();
            if (referencedTensors.contains(subtitleTensor)) {
                invalidPackage(QStringLiteral("Altyazı tensor'u birden çok kez kullanılmış."));
            }
            referencedTensors.insert(subtitleTensor);
            const TensorRange subtitleRange = tensorRange(header, subtitleTensor);
            if (subtitleRange.size == 0 || subtitleRange.size > 32ULL * 1024ULL * 1024ULL) {
                invalidPackage(QStringLiteral("Altyazı tensor boyutu geçersiz."));
            }
            entry.subtitlePath = optionalString(video, QStringLiteral("subtitle_filename"));
            if (entry.subtitlePath.isEmpty()) {
                entry.subtitlePath = QStringLiteral("%1.srt").arg(entry.title);
            }
            entry.subtitleOffset = dataStart + subtitleRange.offset;
            entry.subtitleSize = subtitleRange.size;
            entry.subtitleIsEmbedded = true;
        } else if (!subtitleExternalPath.isEmpty()) {
            entry.subtitlePath = resolveExternalAssetPath(
                source, subtitleExternalPath, QStringLiteral("Harici altyazı"));
            entry.subtitleIsEmbedded = false;
            entry.subtitleSize = static_cast<quint64>(
                std::max<qint64>(0, QFileInfo(entry.subtitlePath).size()));
        }
        entries.push_back(entry);
    }

    if (referencedTensors.size() != allRanges.size()) {
        invalidPackage(QStringLiteral("Paket kullanılmayan veya listelenmemiş tensor içeriyor."));
    }
    if (report != nullptr) {
        report->fileSize = static_cast<quint64>(file.size());
        report->headerSize = headerSize;
        report->mediaSize = dataSize;
        report->tensorCount = allRanges.size();
        report->videoCount = entries.size();
        report->issueCount = report->issues.size();
    }
    file.close();
    return entries;
}

void verifySafeTensorsIntegrity(
    const QString& source, const QVector<VideoEntry>& entries, QStringList* issues,
    const std::function<void(quint64, quint64)>& progress)
{
    quint64 expectedAssets = 0;
    for (const VideoEntry& entry : entries) {
        expectedAssets += !entry.videoSha256.isEmpty();
        expectedAssets += !entry.coverSha256.isEmpty();
        expectedAssets += !entry.subtitleSha256.isEmpty();
    }
    quint64 completed = 0;
    const auto verify = [issues, progress, expectedAssets, &completed](
                            const QString& path, quint64 offset, quint64 size,
                            const QString& expected, const QString& label) {
        if (expected.isEmpty()) {
            return;
        }
        if (expected.size() != 64
            || !std::all_of(expected.cbegin(), expected.cend(),
                            [](QChar value) { return value.isDigit()
                                || (value.toLower() >= QLatin1Char('a')
                                    && value.toLower() <= QLatin1Char('f')); })) {
            issues->push_back(QStringLiteral("%1 SHA-256 alanı geçersiz.").arg(label));
            ++completed;
            if (progress) progress(completed, expectedAssets);
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)
            || offset > static_cast<quint64>(file.size())
            || size > static_cast<quint64>(file.size()) - offset
            || !file.seek(static_cast<qint64>(offset))) {
            issues->push_back(QStringLiteral("%1 dosyası bulunamadı veya okunamıyor.")
                                  .arg(label));
            ++completed;
            if (progress) progress(completed, expectedAssets);
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        quint64 remaining = size;
        while (remaining > 0) {
            const qint64 requested = static_cast<qint64>(
                std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
            const qint64 count = file.read(buffer.data(), requested);
            if (count <= 0) {
                throw std::runtime_error(file.errorString().toStdString());
            }
            hash.addData(buffer.constData(), count);
            remaining -= static_cast<quint64>(count);
        }
        if (QString::fromLatin1(hash.result().toHex()).compare(
                expected, Qt::CaseInsensitive) != 0) {
            issues->push_back(QStringLiteral("%1 içeriği değişmiş veya bozulmuş.")
                                  .arg(label));
        }
        ++completed;
        if (progress) {
            progress(completed, expectedAssets);
        }
    };
    for (const VideoEntry& entry : entries) {
        verify(entry.videoIsEmbedded ? source : entry.videoPath,
               entry.videoIsEmbedded ? entry.videoOffset : 0,
               entry.videoSize, entry.videoSha256,
               QStringLiteral("Video '%1'").arg(entry.title));
        verify(entry.coverIsEmbedded ? source : entry.coverPath,
               entry.coverIsEmbedded ? entry.coverOffset : 0,
               entry.coverSize, entry.coverSha256,
               QStringLiteral("Kapak '%1'").arg(entry.title));
        verify(entry.subtitleIsEmbedded ? source : entry.subtitlePath,
               entry.subtitleIsEmbedded ? entry.subtitleOffset : 0,
               entry.subtitleSize, entry.subtitleSha256,
               QStringLiteral("Altyazı '%1'").arg(entry.title));
    }
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <stdexcept>

namespace {

[[noreturn]] void cryptoFailure(const char* message)
{
    throw std::runtime_error(message);
}

QString protectedKeyPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        throw std::runtime_error("AppData klasörü çözümlenemedi.");
    }
    return QDir(base).filePath(QStringLiteral("signing-key.dpapi"));
}

QByteArray unprotectKey(const QByteArray& encrypted)
{
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        cryptoFailure("İmza anahtarı bu Windows kullanıcısı için açılamadı.");
    }
    const QByteArray result(reinterpret_cast<const char*>(output.pbData),
                            static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

QByteArray loadOrCreatePrivateKey()
{
    const QString keyPath = protectedKeyPath();
    QFile existing(keyPath);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly) || existing.size() > 1024 * 1024) {
            cryptoFailure("İmza anahtarı okunamadı.");
        }
        return unprotectKey(existing.readAll());
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0
        || BCryptGenerateKeyPair(algorithm, &key, 256, 0) < 0
        || BCryptFinalizeKeyPair(key, 0) < 0) {
        if (key != nullptr) BCryptDestroyKey(key);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        cryptoFailure("ECDSA imza anahtarı üretilemedi.");
    }
    ULONG privateSize = 0;
    NTSTATUS status = BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                                      nullptr, 0, &privateSize, 0);
    QByteArray privateBlob(static_cast<qsizetype>(privateSize), Qt::Uninitialized);
    if (status < 0
        || BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                           reinterpret_cast<PUCHAR>(privateBlob.data()), privateSize,
                           &privateSize, 0) < 0) {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        cryptoFailure("ECDSA imza anahtarı dışa aktarılamadı.");
    }
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    DATA_BLOB input{static_cast<DWORD>(privateBlob.size()),
                    reinterpret_cast<BYTE*>(privateBlob.data())};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"PackManager package signing key", nullptr,
                          nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        SecureZeroMemory(privateBlob.data(), static_cast<SIZE_T>(privateBlob.size()));
        cryptoFailure("İmza anahtarı güvenli biçimde korunamadı.");
    }
    const QByteArray protectedBlob(reinterpret_cast<const char*>(output.pbData),
                                   static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    SecureZeroMemory(privateBlob.data(), static_cast<SIZE_T>(privateBlob.size()));

    if (!QDir().mkpath(QFileInfo(keyPath).absolutePath())) {
        cryptoFailure("İmza anahtarı klasörü oluşturulamadı.");
    }
    QSaveFile outputFile(keyPath);
    if (!outputFile.open(QIODevice::WriteOnly)
        || outputFile.write(protectedBlob) != protectedBlob.size()
        || !outputFile.commit()) {
        cryptoFailure("Korunmuş imza anahtarı kaydedilemedi.");
    }
    return unprotectKey(protectedBlob);
}

} // namespace

QString signingKeyPath()
{
    return protectedKeyPath();
}

QByteArray packageSigningDigest(QJsonObject manifest)
{
    QJsonObject package = manifest.value(QStringLiteral("package")).toObject();
    package.remove(QStringLiteral("signature_author"));
    package.remove(QStringLiteral("signature_public_key"));
    package.remove(QStringLiteral("signature"));
    manifest.insert(QStringLiteral("package"), package);
    return QCryptographicHash::hash(
        QJsonDocument(manifest).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}

PackageSignature signPackageDigest(const QByteArray& digest)
{
    if (digest.size() != 32) {
        cryptoFailure("İmza özeti SHA-256 biçiminde olmalı.");
    }
    const QByteArray privateBlob = loadOrCreatePrivateKey();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE privateKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0
        || BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPRIVATE_BLOB,
                               &privateKey,
                               reinterpret_cast<PUCHAR>(
                                   const_cast<char*>(privateBlob.constData())),
                               static_cast<ULONG>(privateBlob.size()), 0) < 0) {
        if (privateKey != nullptr) BCryptDestroyKey(privateKey);
        if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
        cryptoFailure("ECDSA imza anahtarı yüklenemedi.");
    }
    ULONG publicSize = 0;
    ULONG signatureSize = 0;
    NTSTATUS status = BCryptExportKey(privateKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                      nullptr, 0, &publicSize, 0);
    PackageSignature result;
    result.publicKey.resize(static_cast<qsizetype>(publicSize));
    if (status < 0
        || BCryptExportKey(privateKey, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                           reinterpret_cast<PUCHAR>(result.publicKey.data()), publicSize,
                           &publicSize, 0) < 0
        || BCryptSignHash(privateKey, nullptr,
                          reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())),
                          static_cast<ULONG>(digest.size()), nullptr, 0,
                          &signatureSize, 0) < 0) {
        BCryptDestroyKey(privateKey);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        cryptoFailure("Paket imza bileşenleri üretilemedi.");
    }
    result.signature.resize(static_cast<qsizetype>(signatureSize));
    status = BCryptSignHash(
        privateKey, nullptr,
        reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())),
        static_cast<ULONG>(digest.size()),
        reinterpret_cast<PUCHAR>(result.signature.data()), signatureSize,
        &signatureSize, 0);
    BCryptDestroyKey(privateKey);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        cryptoFailure("Paket imzası üretilemedi.");
    }
    return result;
}

bool verifyPackageDigest(const QByteArray& digest, const QByteArray& publicKey,
                         const QByteArray& signature)
{
    if (digest.size() != 32 || publicKey.isEmpty() || signature.size() != 64) {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM,
                                    nullptr, 0) < 0) {
        cryptoFailure("ECDSA doğrulama algoritması başlatılamadı.");
    }
    const NTSTATUS imported = BCryptImportKeyPair(
        algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
        reinterpret_cast<PUCHAR>(const_cast<char*>(publicKey.constData())),
        static_cast<ULONG>(publicKey.size()), 0);
    if (imported < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    const NTSTATUS verified = BCryptVerifySignature(
        key, nullptr, reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())),
        static_cast<ULONG>(digest.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(signature.constData())),
        static_cast<ULONG>(signature.size()), 0);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return verified >= 0;
}
