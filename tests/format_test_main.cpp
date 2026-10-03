#include "PackageFormat.h"
#include "ProjectFile.h"

#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QStringList>

#include <exception>

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 3 || arguments.size() > 6) {
        return 2;
    }

    try {
        if (arguments.at(1) == QStringLiteral("roundtrip")) {
            PackageMetadata metadata;
            const auto entries = readSafeTensors(arguments.at(2), nullptr, &metadata);
            writeSafeTensors(entries, arguments.at(3), metadata);
        } else if (arguments.at(1) == QStringLiteral("project-roundtrip")) {
            PackageMetadata metadata;
            const auto entries = readProjectFile(arguments.at(2), &metadata);
            writeProjectFile(entries, arguments.at(3), metadata);
        } else if (arguments.at(1) == QStringLiteral("project-create")) {
            VideoEntry entry{
                QStringLiteral("Taslak video"),
                QStringLiteral("Proje açıklaması"),
                QStringLiteral("Eğitim"),
                arguments.at(2),
                arguments.at(3)
            };
            PackageMetadata metadata;
            metadata.title = QStringLiteral("Ornek paket");
            metadata.description = QStringLiteral("Paket açıklaması");
            metadata.version = QStringLiteral("2.3.1");
            metadata.createdAt = QStringLiteral("2026-05-12T10:30:00Z");
            writeProjectFile({entry}, arguments.at(4), metadata);
        } else if (arguments.at(1) == QStringLiteral("package-create")) {
            VideoEntry entry{
                QStringLiteral("Paket metaverisi testi"),
                QStringLiteral("Video açıklaması"),
                QStringLiteral("Test"),
                arguments.at(2),
                arguments.size() == 5 ? arguments.at(3) : QString()
            };
            PackageMetadata metadata;
            metadata.title = QStringLiteral("Örnek paket");
            metadata.description = QStringLiteral("Paket açıklaması");
            metadata.version = QStringLiteral("2.3.1");
            metadata.createdAt = QStringLiteral("2026-05-12T10:30:00Z");
            writeSafeTensors({entry},
                             arguments.size() == 5 ? arguments.at(4) : arguments.at(3),
                             metadata);
        } else if (arguments.at(1) == QStringLiteral("signed-create")) {
            VideoEntry entry{
                QStringLiteral("Signed video"),
                QStringLiteral("Signed package test"),
                QStringLiteral("Test"),
                arguments.at(2),
                QString()
            };
            PackageWriteOptions options;
            options.signingAuthor = QStringLiteral("PackManager test signer");
            writeSafeTensors({entry}, arguments.at(3), {}, {}, options);
        } else if (arguments.at(1) == QStringLiteral("external-create")) {
            VideoEntry entry{
                QStringLiteral("External video"),
                QStringLiteral("External media package test"),
                QStringLiteral("Test"),
                arguments.at(2),
                arguments.size() >= 5 ? arguments.at(3) : QString()
            };
            if (arguments.size() == 6) {
                entry.subtitlePath = arguments.at(4);
            }
            entry.tags = {QStringLiteral("test"), QStringLiteral("external")};
            entry.collection = QStringLiteral("compatibility");
            PackageWriteOptions options;
            options.embedMedia = false;
            writeSafeTensors(
                {entry}, arguments.at(arguments.size() - 1), {}, {}, options);
        } else if (arguments.at(1) == QStringLiteral("validate")) {
            SafeTensorsValidationReport report;
            PackageMetadata metadata;
            const auto entries = readSafeTensors(arguments.at(2), &report, &metadata);
            verifySafeTensorsIntegrity(arguments.at(2), entries, &report.issues);
            QTextStream(stdout) << (metadata.signatureVerified ? "signed-valid" : "unsigned")
                                << '\n';
            return report.issues.isEmpty() ? 0 : 4;
        } else if (arguments.at(1) == QStringLiteral("replace-video")) {
            QVector<VideoEntry> entries = readSafeTensors(arguments.at(2));
            if (entries.isEmpty()) {
                return 3;
            }
            entries[0].videoPath = arguments.at(3);
            entries[0].videoIsEmbedded = false;
            writeSafeTensors(entries, arguments.at(4));
        } else if (arguments.at(1) == QStringLiteral("replace-cover")) {
            QVector<VideoEntry> entries = readSafeTensors(arguments.at(2));
            if (entries.isEmpty()) {
                return 3;
            }
            entries[0].coverPath = arguments.at(3);
            entries[0].coverIsEmbedded = false;
            writeSafeTensors(entries, arguments.at(4));
        } else if (arguments.at(1) == QStringLiteral("extract")) {
            const QVector<VideoEntry> entries = readSafeTensors(arguments.at(2));
            if (entries.isEmpty()) {
                return 3;
            }
            extractVideo(entries.front(), arguments.at(3));
        } else if (arguments.at(1) == QStringLiteral("extract-cover")) {
            const QVector<VideoEntry> entries = readSafeTensors(arguments.at(2));
            if (entries.isEmpty()) {
                return 3;
            }
            extractCover(entries.front(), arguments.at(3));
        } else {
            VideoEntry entry{
                QStringLiteral("Örnek video"),
                QStringLiteral("Türkçe açıklama"),
                QStringLiteral("Eğitim"),
                arguments.at(1),
                arguments.at(2) == QStringLiteral("-") ? QString() : arguments.at(2)
            };
            writeSafeTensors({entry}, arguments.at(3));
        }
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    return 0;
}
