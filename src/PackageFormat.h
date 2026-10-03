#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct VideoEntry {
    QString title;
    QString description;
    QString category;
    QString videoPath;
    QString coverPath;
    QString assetContainerPath;
    quint64 videoOffset = 0;
    quint64 videoSize = 0;
    quint64 coverOffset = 0;
    quint64 coverSize = 0;
    bool videoIsEmbedded = false;
    bool coverIsEmbedded = false;
    QString id;
    qint64 durationMs = -1;
    QStringList tags;
    QString collection;
    QString subtitlePath;
    QString videoSha256;
    QString coverSha256;
    QString subtitleSha256;
    bool subtitleIsEmbedded = false;
    quint64 subtitleOffset = 0;
    quint64 subtitleSize = 0;
};

struct PackageMetadata {
    QString title;
    QString description;
    QString version = QStringLiteral("1.0");
    QString createdAt;
    QString signatureAuthor;
    QByteArray signaturePublicKey;
    QByteArray signatureValue;
    bool signaturePresent = false;
    bool signatureVerified = false;
};

struct PackageWriteOptions {
    bool embedMedia = true;
    QString signingAuthor;
};

struct SafeTensorsValidationReport {
    quint64 fileSize = 0;
    quint64 headerSize = 0;
    quint64 mediaSize = 0;
    int tensorCount = 0;
    int videoCount = 0;
    int issueCount = 0;
    QStringList issues;
};

struct PackageSignature {
    QByteArray publicKey;
    QByteArray signature;
};

void writeSafeTensors(const QVector<VideoEntry>& entries, const QString& destination,
                      const PackageMetadata& metadata = {},
                      const std::function<void(quint64, quint64)>& progress = {},
                      const PackageWriteOptions& options = {});
QByteArray readVideoCover(const VideoEntry& entry);
QByteArray readEntrySubtitle(const VideoEntry& entry);
void extractVideo(
    const VideoEntry& entry, const QString& destination,
    const std::function<void(quint64, quint64)>& progress = {});
void extractCover(const VideoEntry& entry, const QString& destination);
void extractSubtitle(const VideoEntry& entry, const QString& destination);
QVector<VideoEntry> readSafeTensors(
    const QString& source, SafeTensorsValidationReport* report = nullptr,
    PackageMetadata* metadata = nullptr);
void verifySafeTensorsIntegrity(
    const QString& source, const QVector<VideoEntry>& entries,
    QStringList* issues,
    const std::function<void(quint64, quint64)>& progress = {});
PackageSignature signPackageDigest(const QByteArray& digest);
bool verifyPackageDigest(const QByteArray& digest, const QByteArray& publicKey,
                         const QByteArray& signature);
QByteArray packageSigningDigest(QJsonObject manifest);
QString signingKeyPath();
