#include "PackManagerWindow.h"
#include "ProjectFile.h"
#include "PackageFormat.h"
#include "VideoListWidget.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QAction>
#include <QActionGroup>
#include <QAudioOutput>
#include <QAbstractItemModel>
#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QImage>
#include <QIcon>
#include <QFormLayout>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidgetItem>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QProcess>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QStyle>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QSortFilterProxyModel>
#include <QThreadPool>
#include <QUuid>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVBoxLayout>
#include <QWidget>
#include <QUrl>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrentRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <QMap>
#include <QSet>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>

namespace {

QWidget* pathRow(QLineEdit*& field, const QString& buttonText,
                 const std::function<void()>& browseAction, QWidget* parent,
                 const QString& secondaryButtonText = QString(),
                 const std::function<void()>& secondaryAction = {})
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    field = new QLineEdit(row);
    field->setReadOnly(true);
    auto* browse = new QPushButton(buttonText, row);
    layout->addWidget(field, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, row, browseAction);
    if (!secondaryButtonText.isEmpty() && secondaryAction) {
        auto* secondary = new QPushButton(secondaryButtonText, row);
        layout->addWidget(secondary);
        QObject::connect(secondary, &QPushButton::clicked, row, secondaryAction);
    }
    return row;
}

QString humanSize(quint64 bytes)
{
    static const QStringList units{
        QStringLiteral("B"), QStringLiteral("KiB"),
        QStringLiteral("MiB"), QStringLiteral("GiB"),
        QStringLiteral("TiB")
    };
    double amount = static_cast<double>(bytes);
    int unit = 0;
    while (amount >= 1024.0 && unit + 1 < units.size()) {
        amount /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(amount, 0, 'f', unit == 0 ? 0 : 1).arg(units.at(unit));
}

QString durationText(qint64 durationMs)
{
    if (durationMs < 0) {
        return QStringLiteral("süre bilinmiyor");
    }
    const qint64 totalSeconds = durationMs / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString canonicalPath(const QString& path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath())
                               : canonical;
}

quint64 entryMediaSize(const VideoEntry& entry)
{
    quint64 size = entry.videoIsEmbedded
        ? entry.videoSize
        : static_cast<quint64>(std::max<qint64>(0, QFileInfo(entry.videoPath).size()));
    const quint64 coverSize = entry.coverIsEmbedded
        ? entry.coverSize
        : static_cast<quint64>(std::max<qint64>(0, QFileInfo(entry.coverPath).size()));
    if (coverSize <= std::numeric_limits<quint64>::max() - size) {
        size += coverSize;
    }
    return size;
}

void createPackageBackup(const QString& packagePath)
{
    const QFileInfo packageInfo(packagePath);
    if (!packageInfo.isFile()) {
        return;
    }
    const QString historyPath = packagePath + QStringLiteral(".history");
    if (!QDir().mkpath(historyPath)) {
        throw std::runtime_error("Paket sürüm geçmişi klasörü oluşturulamadı.");
    }
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString backupPath = QDir(historyPath).filePath(
        QStringLiteral("%1.%2.bak").arg(packageInfo.fileName(), timestamp));
    if (QFileInfo::exists(backupPath)) {
        backupPath += QLatin1Char('-')
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (!QFile::copy(packagePath, backupPath)) {
        throw std::runtime_error("Mevcut paket sürüm geçmişine yedeklenemedi.");
    }
    const QVector<VideoEntry> entries = readSafeTensors(packagePath);
    const QString packageDirectory = QFileInfo(packagePath).absolutePath();
    const QString backupDirectory = QFileInfo(backupPath).absolutePath();
    QSet<QString> copiedMedia;
    for (const VideoEntry& entry : entries) {
        const QStringList mediaPaths{
            entry.videoIsEmbedded ? QString() : entry.videoPath,
            entry.coverIsEmbedded ? QString() : entry.coverPath,
            entry.subtitleIsEmbedded ? QString() : entry.subtitlePath
        };
        for (const QString& mediaPath : mediaPaths) {
            if (mediaPath.isEmpty()) {
                continue;
            }
            const QString relativePath = QDir(packageDirectory).relativeFilePath(mediaPath);
            const QString target = QDir(backupDirectory).filePath(relativePath);
            const QString targetKey = QDir::cleanPath(target).toCaseFolded();
            if (copiedMedia.contains(targetKey)) {
                continue;
            }
            if (QFileInfo::exists(target)) {
                copiedMedia.insert(targetKey);
                continue;
            }
            if (!QDir().mkpath(QFileInfo(target).absolutePath())
                || !QFile::copy(mediaPath, target)) {
                throw std::runtime_error(
                    "Harici medya, paket sürüm geçmişine yedeklenemedi.");
            }
            copiedMedia.insert(targetKey);
        }
    }
    QDir historyDirectory(historyPath);
    const QFileInfoList backups = historyDirectory.entryInfoList(
        {QStringLiteral("*.bak")}, QDir::Files, QDir::Name | QDir::Reversed);
    for (int index = 20; index < backups.size(); ++index) {
        if (!QFile::remove(backups.at(index).absoluteFilePath())) {
            throw std::runtime_error("Eski paket yedeği temizlenemedi.");
        }
    }
}

void copyFileAtomically(const QString& sourcePath, const QString& destinationPath)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(source.errorString().toStdString());
    }
    QSaveFile destination(destinationPath);
    if (!destination.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(destination.errorString().toStdString());
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    while (true) {
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0) {
            destination.cancelWriting();
            throw std::runtime_error(source.errorString().toStdString());
        }
        if (count == 0) {
            break;
        }
        qint64 written = 0;
        while (written < count) {
            const qint64 outputCount =
                destination.write(buffer.constData() + written, count - written);
            if (outputCount <= 0) {
                destination.cancelWriting();
                throw std::runtime_error(destination.errorString().toStdString());
            }
            written += outputCount;
        }
    }
    if (!destination.commit()) {
        throw std::runtime_error(destination.errorString().toStdString());
    }
}

QString entryId(VideoEntry& entry)
{
    if (entry.id.isEmpty()) {
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return entry.id;
}

QString appDataProjectPath()
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    return QDir(directory).filePath(QStringLiteral("packmanager-autosave.pmp"));
}

bool isVideoFile(const QString& path)
{
    static const QSet<QString> extensions{
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("m4v"),
        QStringLiteral("wmv"), QStringLiteral("mpeg"), QStringLiteral("mpg")
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

bool isImageFile(const QString& path)
{
    static const QSet<QString> extensions{
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("webp"), QStringLiteral("bmp")
    };
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QByteArray hashMediaRange(const QString& path, quint64 offset, quint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)
        || offset > static_cast<quint64>(file.size())
        || size > static_cast<quint64>(file.size()) - offset
        || !file.seek(static_cast<qint64>(offset))) {
        throw std::runtime_error("Paket medyası özet için okunamıyor.");
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    quint64 remaining = size;
    while (remaining > 0) {
        const qint64 requested = static_cast<qint64>(
            std::min<quint64>(remaining, static_cast<quint64>(buffer.size())));
        const qint64 count = file.read(buffer.data(), requested);
        if (count <= 0) {
            throw std::runtime_error(
                QString("Paket medyası okunamadı: %1").arg(file.errorString()).toStdString());
        }
        hash.addData(buffer.constData(), count);
        remaining -= static_cast<quint64>(count);
    }
    return hash.result();
}

QString materializeVideoForTool(const VideoEntry& entry, QTemporaryDir& temporaryDirectory)
{
    if (!entry.videoIsEmbedded) {
        if (!QFileInfo(entry.videoPath).isFile()) {
            throw std::runtime_error("Video dosyası bulunamadı.");
        }
        return entry.videoPath;
    }
    if (!temporaryDirectory.isValid()) {
        throw std::runtime_error("Video araçları için geçici klasör oluşturulamadı.");
    }
    QString suffix = QFileInfo(entry.videoPath).suffix();
    if (suffix.isEmpty()) {
        suffix = QStringLiteral("mp4");
    }
    const QString path = QDir(temporaryDirectory.path()).filePath(
        QStringLiteral("input.%1").arg(suffix));
    extractVideo(entry, path);
    return path;
}

QByteArray runMediaTool(const QString& executable, const QStringList& arguments)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(10000)) {
        throw std::runtime_error(
            QString("Medya aracı başlatılamadı: %1").arg(process.errorString()).toStdString());
    }
    if (!process.waitForFinished(-1)) {
        throw std::runtime_error(
            QString("Medya aracı çalışırken hata oluştu: %1")
                .arg(process.errorString()).toStdString());
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (detail.isEmpty()) {
            detail = QStringLiteral("Çıkış kodu: %1").arg(process.exitCode());
        }
        throw std::runtime_error(detail.toStdString());
    }
    return process.readAllStandardOutput();
}

} // namespace

PackManagerWindow::PackManagerWindow(QWidget* parent)
    : QMainWindow(parent)
{
    QSettings settings;
    language_ = settings.value(QStringLiteral("ui/language"), QStringLiteral("tr")).toString();
    theme_ = settings.value(QStringLiteral("ui/theme"), QStringLiteral("light")).toString();
    setWindowTitle(QStringLiteral("PackManager"));
    const QIcon appIcon(QCoreApplication::applicationDirPath()
                        + QStringLiteral("/packmanager.ico"));
    setWindowIcon(appIcon.isNull()
                      ? style()->standardIcon(QStyle::SP_MediaPlay)
                      : appIcon);
    resize(960, 640);
    setMinimumSize(800, 540);
    setAcceptDrops(true);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("Düzen"));
    undoAction_ = editMenu->addAction(QStringLiteral("Geri al"));
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = editMenu->addAction(QStringLiteral("Yinele"));
    redoAction_->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    QAction* copyAction = editMenu->addAction(QStringLiteral("Kopyala"));
    copyAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(copyAction, &QAction::triggered,
            this, &PackManagerWindow::copySelectedEntries);
    QAction* pasteAction = editMenu->addAction(QStringLiteral("Yapıştır"));
    pasteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(pasteAction, &QAction::triggered,
            this, &PackManagerWindow::pasteEntries);
    QAction* duplicateAction = editMenu->addAction(QStringLiteral("Çoğalt"));
    duplicateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    connect(duplicateAction, &QAction::triggered,
            this, &PackManagerWindow::duplicateSelectedEntries);
    editMenu->addSeparator();
    editMenu->addAction(QStringLiteral("Kategorileri yönet..."),
                        this, &PackManagerWindow::editCategories);
    auto* packageMenu = menuBar()->addMenu(QStringLiteral("Paket"));
    packageMenu->addAction(QStringLiteral("Paket bilgileri..."),
                           this, &PackManagerWindow::editPackageMetadata);
    packageMenu->addAction(QStringLiteral("Sürüm geçmişi..."),
                           this, &PackManagerWindow::showPackageHistory);
    packageMenu->addAction(QStringLiteral("Seçilenleri ayrı paket olarak kaydet..."),
                           this, &PackManagerWindow::selectEntriesForExport);
    packageMenu->addAction(QStringLiteral("İçeriği klasöre çıkar..."),
                           this, &PackManagerWindow::extractSelectedEntries);
    packageMenu->addAction(QStringLiteral("Eksik medya dosyalarını bul..."),
                           this, &PackManagerWindow::findMissingMedia);
    packageMenu->addAction(QStringLiteral("Altyazıyı görüntüle..."),
                           this, &PackManagerWindow::showSelectedSubtitle);
    packageMenu->addAction(QStringLiteral("Video teknik bilgileri..."),
                           this, &PackManagerWindow::showVideoTechnicalInfo);
    packageMenu->addAction(QStringLiteral("Videoyu dönüştür..."),
                           this, &PackManagerWindow::convertSelectedVideo);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("Görünüm"));
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    auto* lightAction = viewMenu->addAction(QStringLiteral("Açık tema"));
    auto* darkAction = viewMenu->addAction(QStringLiteral("Koyu tema"));
    lightAction->setCheckable(true);
    darkAction->setCheckable(true);
    themeGroup->addAction(lightAction);
    themeGroup->addAction(darkAction);
    lightAction->setChecked(theme_ != QStringLiteral("dark"));
    darkAction->setChecked(theme_ == QStringLiteral("dark"));
    connect(lightAction, &QAction::triggered, this,
            [this] { changeTheme(QStringLiteral("light")); });
    connect(darkAction, &QAction::triggered, this,
            [this] { changeTheme(QStringLiteral("dark")); });
    viewMenu->addSeparator();
    auto* languageGroup = new QActionGroup(this);
    languageGroup->setExclusive(true);
    auto* turkishAction = viewMenu->addAction(QStringLiteral("Türkçe"));
    auto* englishAction = viewMenu->addAction(QStringLiteral("English"));
    turkishAction->setCheckable(true);
    englishAction->setCheckable(true);
    languageGroup->addAction(turkishAction);
    languageGroup->addAction(englishAction);
    turkishAction->setChecked(language_ == QStringLiteral("tr"));
    englishAction->setChecked(language_ == QStringLiteral("en"));
    connect(turkishAction, &QAction::triggered, this,
            [this] { changeLanguage(QStringLiteral("tr")); });
    connect(englishAction, &QAction::triggered, this,
            [this] { changeLanguage(QStringLiteral("en")); });
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Ayarlar..."),
                        this, &PackManagerWindow::editSettings);
    viewMenu->addAction(QStringLiteral("Güncellemeleri denetle..."),
                        this, [this] { checkForUpdates(false); });
    viewMenu->addSeparator();
    viewMenu->addAction(QStringLiteral("Hakkında..."), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("PackManager"),
            QStringLiteral("PackManager %1\n\nC++17 ve Qt ile geliştirilmiş SafeTensors video paket yöneticisi.")
                .arg(QCoreApplication::applicationVersion()));
    });
    const auto addShortcut = [this](const QKeySequence& shortcut,
                                    const std::function<void()>& callback) {
        auto* action = new QAction(this);
        action->setShortcut(shortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, callback);
    };
    addShortcut(QKeySequence(QStringLiteral("Ctrl+N")),
                [this] { createNewProject(); });
    addShortcut(QKeySequence(QStringLiteral("Ctrl+O")),
                [this] { openPackage(); });
    addShortcut(QKeySequence(QStringLiteral("Ctrl+S")),
                [this] { savePackage(); });
    addShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")),
                [this] { saveProjectDraft(); });
    addShortcut(QKeySequence(QStringLiteral("Ctrl+F")),
                [this] { searchField_->setFocus(); searchField_->selectAll(); });
    addShortcut(QKeySequence(Qt::Key_F5),
                [this] { togglePlayback(); });

    auto* central = new QWidget(this);
    central->setAcceptDrops(true);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(16, 16, 16, 16);

    auto* heading = new QLabel(QStringLiteral("PackManager"), central);
    QFont headingFont = heading->font();
    headingFont.setPointSize(20);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    auto* headingRow = new QHBoxLayout;
    headingRow->addWidget(heading);
    headingRow->addStretch();
    auto* newButton = new QPushButton(QStringLiteral("Yeni"), central);
    auto* openProjectButton = new QPushButton(QStringLiteral("Taslak aç"), central);
    auto* saveProjectButton = new QPushButton(QStringLiteral("Taslağı kaydet"), central);
    auto* saveProjectAsButton = new QPushButton(QStringLiteral("Taslağı farklı kaydet"), central);
    auto* openButton = new QPushButton(QStringLiteral("Paket aç"), central);
    auto* validateButton = new QPushButton(QStringLiteral("Paket doğrula"), central);
    for (QPushButton* button : {newButton, openProjectButton, saveProjectButton,
                                saveProjectAsButton, openButton, validateButton}) {
        headingRow->addWidget(button);
    }
    rootLayout->addLayout(headingRow);

    auto* content = new QHBoxLayout;
    rootLayout->addLayout(content, 1);

    auto* listingGroup = new QGroupBox(QStringLiteral("Paket videoları"), central);
    auto* listingLayout = new QVBoxLayout(listingGroup);
    auto* searchRow = new QHBoxLayout;
    searchField_ = new QLineEdit(listingGroup);
    searchField_->setPlaceholderText(QStringLiteral("Başlık, açıklama veya kategori ara..."));
    categoryFilter_ = new QComboBox(listingGroup);
    categoryFilter_->addItem(QStringLiteral("Tüm kategoriler"));
    searchRow->addWidget(searchField_, 2);
    searchRow->addWidget(categoryFilter_, 1);
    listingLayout->addLayout(searchRow);
    auto* listOptions = new QHBoxLayout;
    sortOrder_ = new QComboBox(listingGroup);
    sortOrder_->addItems({QStringLiteral("Paket sırası"), QStringLiteral("Başlığa göre"),
                          QStringLiteral("Kategoriye göre"), QStringLiteral("Boyuta göre"),
                          QStringLiteral("Süreye göre")});
    secondarySortOrder_ = new QComboBox(listingGroup);
    secondarySortOrder_->addItem(QStringLiteral("İkincil sıralama: yok"), 0);
    secondarySortOrder_->addItem(QStringLiteral("Başlık"), 1);
    secondarySortOrder_->addItem(QStringLiteral("Kategori"), 2);
    secondarySortOrder_->addItem(QStringLiteral("Boyut"), 3);
    secondarySortOrder_->addItem(QStringLiteral("Süre"), 4);
    listViewMode_ = new QComboBox(listingGroup);
    listViewMode_->addItems({QStringLiteral("Ayrıntılı liste"),
                             QStringLiteral("Küçük resimler")});
    auto* categoryManageButton = new QPushButton(QStringLiteral("Kategoriler..."), listingGroup);
    listOptions->addWidget(sortOrder_, 1);
    listOptions->addWidget(secondarySortOrder_);
    listOptions->addWidget(listViewMode_);
    listOptions->addWidget(categoryManageButton);
    listingLayout->addLayout(listOptions);
    videoList_ = new VideoListWidget(listingGroup);
    videoList_->setIconSize(QSize(64, 48));
    videoList_->setDragEnabled(true);
    videoList_->setAcceptDrops(true);
    videoList_->setDropIndicatorShown(true);
    videoList_->setDragDropMode(QAbstractItemView::InternalMove);
    videoList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    videoList_->setDefaultDropAction(Qt::MoveAction);
    videoList_->setToolTip(QStringLiteral("Sıralamayı değiştirmek için videoları sürükleyin."));
    listingLayout->addWidget(videoList_, 1);
    auto* addVideosButton = new QPushButton(QStringLiteral("Videoları toplu ekle..."), listingGroup);
    listingLayout->addWidget(addVideosButton, 0, Qt::AlignLeft);
    auto* removeButton = new QPushButton(QStringLiteral("Seçileni sil"), listingGroup);
    listingLayout->addWidget(removeButton, 0, Qt::AlignLeft);
    auto* bulkActions = new QHBoxLayout;
    auto* bulkCategoryButton = new QPushButton(QStringLiteral("Kategori"), listingGroup);
    auto* bulkCoverButton = new QPushButton(QStringLiteral("Seçilenlere kapak uygula"), listingGroup);
    auto* bulkDeleteButton = new QPushButton(QStringLiteral("Seçilenleri sil"), listingGroup);
    bulkActions->addWidget(bulkCategoryButton);
    bulkActions->addWidget(bulkCoverButton);
    bulkActions->addWidget(bulkDeleteButton);
    listingLayout->addLayout(bulkActions);
    content->addWidget(listingGroup, 1);

    auto* editorGroup = new QGroupBox(QStringLiteral("Video bilgileri"), central);
    auto* form = new QFormLayout(editorGroup);
    videoPath_ = nullptr;
    form->addRow(QStringLiteral("Video dosyası"),
                 pathRow(videoPath_, QStringLiteral("Video değiştir..."),
                         [this] { chooseVideo(); }, editorGroup));
    coverPath_ = nullptr;
    form->addRow(QStringLiteral("Kapak resmi"),
                 pathRow(coverPath_, QStringLiteral("Kapak değiştir..."),
                         [this] { chooseCover(); }, editorGroup,
                         QStringLiteral("Kapağı kaldır"),
                         [this] { removeCover(); }));

    title_ = new QLineEdit(editorGroup);
    form->addRow(QStringLiteral("Başlık"), title_);

    category_ = new QComboBox(editorGroup);
    category_->setEditable(true);
    category_->addItems({
        QStringLiteral("Genel"), QStringLiteral("Eğitim"),
        QStringLiteral("Eğlence"), QStringLiteral("Oyun"),
        QStringLiteral("Müzik"), QStringLiteral("Teknoloji"),
        QStringLiteral("Diğer")
    });
    form->addRow(QStringLiteral("Kategori"), category_);

    description_ = new QTextEdit(editorGroup);
    description_->setMinimumHeight(130);
    form->addRow(QStringLiteral("Açıklama"), description_);
    tags_ = new QLineEdit(editorGroup);
    tags_->setPlaceholderText(QStringLiteral("virgülle ayrılmış etiketler"));
    form->addRow(QStringLiteral("Etiketler"), tags_);
    collection_ = new QLineEdit(editorGroup);
    form->addRow(QStringLiteral("Koleksiyon"), collection_);
    subtitlePath_ = nullptr;
    form->addRow(QStringLiteral("Altyazı"),
                 pathRow(subtitlePath_, QStringLiteral("SRT/VTT seç..."),
                         [this] { chooseSubtitle(); }, editorGroup));

    auto* formActions = new QHBoxLayout;
    auto* clearButton = new QPushButton(QStringLiteral("Formu temizle"), editorGroup);
    saveEntryButton_ = new QPushButton(QStringLiteral("Yeni videoyu listeye ekle"), editorGroup);
    editStatus_ = new QLabel(QStringLiteral("Yeni bir video ekleyebilir veya listeden video seçip düzenleyebilirsiniz."), editorGroup);
    editStatus_->setWordWrap(true);
    formActions->addWidget(editStatus_, 1);
    formActions->addWidget(clearButton);
    formActions->addWidget(saveEntryButton_);
    form->addRow(QString(), formActions);

    auto* previewGroup = new QGroupBox(QStringLiteral("Paket içeriği önizleme"), editorGroup);
    auto* previewLayout = new QHBoxLayout(previewGroup);
    coverPreview_ = new QLabel(QStringLiteral("Kapak yok"), previewGroup);
    coverPreview_->setAlignment(Qt::AlignCenter);
    coverPreview_->setMinimumSize(180, 130);
    coverPreview_->setMaximumSize(260, 180);
    coverPreview_->setStyleSheet(QStringLiteral("QLabel { background: #202020; color: #dddddd; }"));
    previewLayout->addWidget(coverPreview_);
    videoWidget_ = new QVideoWidget(previewGroup);
    videoWidget_->setMinimumSize(240, 135);
    previewLayout->addWidget(videoWidget_, 1);
    form->addRow(previewGroup);

    auto* playerControls = new QHBoxLayout;
    playbackButton_ = new QPushButton(QStringLiteral("Videoyu oynat"), editorGroup);
    auto* stopButton = new QPushButton(QStringLiteral("Durdur"), editorGroup);
    auto* captureCoverButton = new QPushButton(QStringLiteral("Bu kareyi kapak yap"), editorGroup);
    playbackSlider_ = new QSlider(Qt::Horizontal, editorGroup);
    playbackSlider_->setRange(0, 0);
    playbackStatus_ = new QLabel(QStringLiteral("Bir video seçin."), editorGroup);
    playerControls->addWidget(playbackButton_);
    playerControls->addWidget(stopButton);
    playerControls->addWidget(captureCoverButton);
    playerControls->addWidget(playbackSlider_, 1);
    playerControls->addWidget(playbackStatus_);
    form->addRow(playerControls);
    content->addWidget(editorGroup, 2);

    auto* footer = new QHBoxLayout;
    footer->addWidget(new QLabel(
        QStringLiteral("Video ve kapak dosyaları paket içinde saklanır."), central));
    footer->addStretch();
    summaryLabel_ = new QLabel(central);
    summaryLabel_->setWordWrap(true);
    footer->addWidget(summaryLabel_, 1);
    savePackageButton_ = new QPushButton(QStringLiteral("Paketi kaydet (.safetensors)"), central);
    footer->addWidget(savePackageButton_);
    rootLayout->addLayout(footer);

    setCentralWidget(central);

    mediaPlayer_ = new QMediaPlayer(this);
    auto* audioOutput = new QAudioOutput(this);
    mediaPlayer_->setAudioOutput(audioOutput);
    mediaPlayer_->setVideoOutput(videoWidget_);
    videoSink_ = videoWidget_->videoSink();

    connect(openButton, &QPushButton::clicked,
            this, &PackManagerWindow::openPackage);
    connect(validateButton, &QPushButton::clicked,
            this, &PackManagerWindow::validatePackage);
    connect(editMenu->actions().at(0), &QAction::triggered,
            this, &PackManagerWindow::undo);
    connect(editMenu->actions().at(1), &QAction::triggered,
            this, &PackManagerWindow::redo);
    connect(categoryManageButton, &QPushButton::clicked,
            this, &PackManagerWindow::editCategories);
    connect(bulkCategoryButton, &QPushButton::clicked,
            this, &PackManagerWindow::bulkEditCategory);
    connect(bulkCoverButton, &QPushButton::clicked,
            this, &PackManagerWindow::bulkApplyCover);
    connect(bulkDeleteButton, &QPushButton::clicked,
            this, &PackManagerWindow::deleteSelectedEntries);
    connect(searchField_, &QLineEdit::textChanged,
            this, &PackManagerWindow::applyFilters);
    connect(categoryFilter_, &QComboBox::currentTextChanged,
            this, &PackManagerWindow::applyFilters);
    connect(sortOrder_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &PackManagerWindow::changeSortOrder);
    connect(secondarySortOrder_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] {
                if (sortOrder_->currentIndex() > 0) {
                    changeSortOrder(sortOrder_->currentIndex());
                }
            });
    connect(listViewMode_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int mode) {
                videoList_->setIconSize(mode == 0 ? QSize(64, 48) : QSize(160, 100));
                videoList_->setViewMode(mode == 0
                    ? QListView::ListMode : QListView::IconMode);
                videoList_->setGridSize(mode == 0 ? QSize() : QSize(190, 150));
                for (int row = 0; row < videoList_->count(); ++row) {
                    updateListRow(row);
                }
            });
    connect(newButton, &QPushButton::clicked,
            this, &PackManagerWindow::createNewProject);
    connect(openProjectButton, &QPushButton::clicked,
            this, &PackManagerWindow::openProjectDraft);
    connect(saveProjectButton, &QPushButton::clicked,
            this, [this] { saveProjectDraft(false); });
    connect(saveProjectAsButton, &QPushButton::clicked,
            this, [this] { saveProjectDraft(true); });
    connect(addVideosButton, &QPushButton::clicked,
            this, &PackManagerWindow::chooseVideos);
    connect(videoList_->model(), &QAbstractItemModel::rowsMoved,
            this, [this] { reorderEntries(); });
    connect(videoList_, &VideoListWidget::filesDropped,
            this, &PackManagerWindow::importDroppedFiles);
    connect(videoList_, &QListWidget::currentRowChanged,
            this, &PackManagerWindow::loadSelectedEntry);
    connect(removeButton, &QPushButton::clicked,
            this, &PackManagerWindow::removeSelectedEntry);
    connect(clearButton, &QPushButton::clicked,
            this, &PackManagerWindow::clearForm);
    connect(saveEntryButton_, &QPushButton::clicked,
            this, &PackManagerWindow::saveEntry);
    connect(savePackageButton_, &QPushButton::clicked,
            this, &PackManagerWindow::savePackage);
    connect(captureCoverButton, &QPushButton::clicked,
            this, &PackManagerWindow::captureVideoFrame);
    connect(playbackButton_, &QPushButton::clicked,
            this, &PackManagerWindow::togglePlayback);
    connect(stopButton, &QPushButton::clicked, mediaPlayer_, &QMediaPlayer::stop);
    connect(mediaPlayer_, &QMediaPlayer::positionChanged,
            this, &PackManagerWindow::updatePlaybackPosition);
    connect(mediaPlayer_, &QMediaPlayer::durationChanged,
            this, &PackManagerWindow::updatePlaybackDuration);
    connect(videoSink_, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) {
                if (frame.isValid()) {
                    const QImage image = frame.toImage();
                    if (!image.isNull()) {
                        lastVideoFrame_ = image;
                    }
                }
                if (frameCapturePending_ && !lastVideoFrame_.isNull()) {
                    frameCapturePending_ = false;
                    mediaPlayer_->pause();
                    QTimer::singleShot(0, this, &PackManagerWindow::captureVideoFrame);
                }
            });
    connect(mediaPlayer_, &QMediaPlayer::errorOccurred,
            this, [this] { showPlayerError(); });
    connect(mediaPlayer_, &QMediaPlayer::playbackStateChanged,
            this, [this](QMediaPlayer::PlaybackState state) {
                playbackButton_->setText(
                    state == QMediaPlayer::PlayingState
                        ? QStringLiteral("Duraklat")
                        : QStringLiteral("Videoyu oynat"));
            });
    connect(playbackSlider_, &QSlider::sliderMoved,
            this, &PackManagerWindow::seekVideo);
    connect(title_, &QLineEdit::textEdited,
            this, [this] { markFormDirty(); });
    connect(category_, &QComboBox::currentTextChanged,
            this, [this] { markFormDirty(); });
    connect(description_, &QTextEdit::textChanged,
            this, [this] { markFormDirty(); });
    connect(tags_, &QLineEdit::textEdited,
            this, [this] { markFormDirty(); });
    connect(collection_, &QLineEdit::textEdited,
            this, [this] { markFormDirty(); });
    recoveryTimer_ = new QTimer(this);
    recoveryTimer_->setSingleShot(true);
    recoveryTimer_->setInterval(
        settings.value(QStringLiteral("recovery/intervalSeconds"), 3).toInt() * 1000);
    connect(recoveryTimer_, &QTimer::timeout,
            this, &PackManagerWindow::saveRecoveryDraft);
    updateEditorState();
    updateSummary();
    applyCurrentTheme();
    updateLocalizedUi();
    restoreRecoveryDraft();
    if (settings.value(QStringLiteral("updates/autoCheck"), true).toBool()) {
        QTimer::singleShot(2000, this, [this] { checkForUpdates(true); });
    }
}

PackManagerWindow::~PackManagerWindow()
{
    if (packageWriteWatcher_ != nullptr && packageWriteWatcher_->isRunning()) {
        packageWriteWatcher_->waitForFinished();
    }
    if (playbackExtractionWatcher_ != nullptr
        && playbackExtractionWatcher_->isRunning()) {
        playbackExtractionWatcher_->waitForFinished();
    }
    if (extractionWatcher_ != nullptr && extractionWatcher_->isRunning()) {
        extractionWatcher_->waitForFinished();
    }
    if (validationWatcher_ != nullptr && validationWatcher_->isRunning()) {
        validationWatcher_->waitForFinished();
    }
    if (packageReadWatcher_ != nullptr && packageReadWatcher_->isRunning()) {
        packageReadWatcher_->waitForFinished();
    }
    if (videoToolWatcher_ != nullptr && videoToolWatcher_->isRunning()) {
        videoToolWatcher_->waitForFinished();
    }
    mediaPlayer_->stop();
    mediaPlayer_->setSource(QUrl());
}

void PackManagerWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void PackManagerWindow::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    importDroppedFiles(paths);
    event->acceptProposedAction();
}

void PackManagerWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscardChanges(QStringLiteral("uygulamayı kapat"))) {
        QFile::remove(appDataProjectPath());
        event->accept();
        return;
    }
    event->ignore();
}

void PackManagerWindow::chooseVideo()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Video seç"), QString(),
        QStringLiteral("Video dosyaları (*.mp4 *.mkv *.mov *.webm *.avi *.m4v);;Tüm dosyalar (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    videoPath_->setText(path);
    if (title_->text().trimmed().isEmpty()) {
        title_->setText(QFileInfo(path).completeBaseName());
    }
    markFormDirty();
}

void PackManagerWindow::chooseVideos()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Videoları toplu ekle"), QString(),
        QStringLiteral("Video dosyaları (*.mp4 *.mkv *.mov *.webm *.avi *.m4v *.wmv *.mpeg *.mpg)"));
    if (paths.isEmpty()) {
        return;
    }
    importDroppedFiles(paths);
}

void PackManagerWindow::chooseCover()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Kapak resmi seç"), QString(),
        QStringLiteral("Resim dosyaları (*.png *.jpg *.jpeg *.webp *.bmp);;Tüm dosyalar (*.*)"));
    if (!path.isEmpty()) {
        coverPath_->setText(path);
        VideoEntry previewEntry;
        previewEntry.coverPath = path;
        updateCoverPreview(previewEntry);
        markFormDirty();
    }
}

void PackManagerWindow::chooseSubtitle()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Altyazı dosyası seç"), QString(),
        QStringLiteral("Altyazılar (*.srt *.vtt);;Tüm dosyalar (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    subtitlePath_->setText(path);
    markFormDirty();
}

void PackManagerWindow::showSelectedSubtitle()
{
    QByteArray subtitle;
    try {
        const int row = editingIndex_ >= 0 ? editingIndex_ : videoList_->currentRow();
        if (row >= 0 && row < entries_.size()
            && entries_.at(row).subtitlePath == subtitlePath_->text()) {
            subtitle = readEntrySubtitle(entries_.at(row));
        } else if (!subtitlePath_->text().trimmed().isEmpty()) {
            QFile file(subtitlePath_->text());
            if (!file.open(QIODevice::ReadOnly)) {
                throw std::runtime_error(file.errorString().toStdString());
            }
            if (file.size() > 32 * 1024 * 1024) {
                throw std::runtime_error("Altyazı dosyası 32 MiB sınırını aşıyor.");
            }
            subtitle = file.readAll();
        }
        if (subtitle.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Altyazı yok"),
                                     QStringLiteral("Seçili video için altyazı bulunamadı."));
            return;
        }
    } catch (const std::exception& error) {
        QMessageBox::warning(this, QStringLiteral("Altyazı açılamadı"),
                             QString::fromUtf8(error.what()));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Altyazı önizlemesi"));
    dialog.resize(720, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setPlainText(QString::fromUtf8(subtitle));
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void PackManagerWindow::showVideoTechnicalInfo()
{
    if (activeProgress_ != nullptr) {
        return;
    }
    const int row = editingIndex_ >= 0 ? editingIndex_ : videoList_->currentRow();
    if (row < 0 || row >= entries_.size()) {
        QMessageBox::information(this, QStringLiteral("Video seçilmedi"),
                                 QStringLiteral("Önce bir video seçin."));
        return;
    }
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("FFprobe bulunamadı"),
            QStringLiteral("Teknik video bilgileri için FFprobe'u kurup PATH'e ekleyin."));
        return;
    }
    const VideoEntry entry = entries_.at(row);
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Video teknik bilgileri okunuyor..."), QString(), 0, 0, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    auto* watcher = new QFutureWatcher<QString>(this);
    videoToolWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        if (activeProgress_ != nullptr) {
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Video teknik bilgileri"));
            dialog.resize(620, 460);
            auto* layout = new QVBoxLayout(&dialog);
            auto* details = new QPlainTextEdit(&dialog);
            details->setReadOnly(true);
            details->setPlainText(watcher->result());
            layout->addWidget(details);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog.exec();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Teknik bilgiler okunamadı"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (videoToolWatcher_ == watcher) {
            videoToolWatcher_ = nullptr;
        }
    });
    videoToolWatcher_->setFuture(QtConcurrent::run([entry, ffprobe] {
        QTemporaryDir temporaryDirectory;
        const QString input =
            materializeVideoForTool(entry, temporaryDirectory);
        const QByteArray result = runMediaTool(
            ffprobe,
            {QStringLiteral("-v"), QStringLiteral("error"),
             QStringLiteral("-show_entries"),
             QStringLiteral("format=duration:stream=codec_type,codec_name,width,height,r_frame_rate,channels,sample_rate"),
             QStringLiteral("-of"), QStringLiteral("json"), input});
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(result, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            throw std::runtime_error("FFprobe geçerli JSON bilgisi döndürmedi.");
        }
        return QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    }));
}

void PackManagerWindow::convertSelectedVideo()
{
    if (activeProgress_ != nullptr) {
        return;
    }
    const int row = editingIndex_ >= 0 ? editingIndex_ : videoList_->currentRow();
    if (row < 0 || row >= entries_.size()) {
        QMessageBox::information(this, QStringLiteral("Video seçilmedi"),
                                 QStringLiteral("Önce dönüştürülecek videoyu seçin."));
        return;
    }
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QMessageBox::warning(
            this, QStringLiteral("FFmpeg bulunamadı"),
            QStringLiteral("Video dönüştürmek için FFmpeg'i kurup PATH'e ekleyin."));
        return;
    }
    const VideoEntry entry = entries_.at(row);
    QString outputPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Dönüştürülmüş videoyu kaydet"),
        QFileInfo(entry.videoPath).completeBaseName() + QStringLiteral("-converted.mp4"),
        QStringLiteral("MP4 video (*.mp4)"));
    if (outputPath.isEmpty()) {
        return;
    }
    if (!outputPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)) {
        outputPath += QStringLiteral(".mp4");
    }
    if (canonicalPath(outputPath) == canonicalPath(packagePath_)
        || (entry.videoIsEmbedded
            && canonicalPath(outputPath) == canonicalPath(entry.assetContainerPath))
        || (!entry.videoIsEmbedded
            && canonicalPath(outputPath) == canonicalPath(entry.videoPath))) {
        QMessageBox::warning(this, QStringLiteral("Geçersiz hedef"),
                             QStringLiteral("Çıktı dosyası kaynak paketin/video dosyasının üzerine yazılamaz."));
        return;
    }
    const QStringList qualityOptions{
        QStringLiteral("Daha küçük dosya (CRF 28)"),
        QStringLiteral("Dengeli (CRF 23)"),
        QStringLiteral("Daha yüksek kalite (CRF 18)")
    };
    bool accepted = false;
    const QString quality = QInputDialog::getItem(
        this, QStringLiteral("Dönüştürme kalitesi"),
        QStringLiteral("H.264/MP4 kalite ayarını seçin:"),
        qualityOptions, 1, false, &accepted);
    if (!accepted) {
        return;
    }
    const QString crf = quality == qualityOptions.at(0)
        ? QStringLiteral("28")
        : quality == qualityOptions.at(2) ? QStringLiteral("18")
                                          : QStringLiteral("23");
    const QString id = entry.id;
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Video FFmpeg ile dönüştürülüyor..."), QString(), 0, 0, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    auto* watcher = new QFutureWatcher<QString>(this);
    videoToolWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, id] {
        if (activeProgress_ != nullptr) {
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            const QString convertedPath = watcher->result();
            const auto found = std::find_if(
                entries_.begin(), entries_.end(), [&id](const VideoEntry& item) {
                    return item.id == id;
                });
            if (found != entries_.end()) {
                recordUndoState();
                found->videoPath = convertedPath;
                found->videoIsEmbedded = false;
                found->videoOffset = 0;
                found->videoSize = 0;
                found->videoSha256.clear();
                found->durationMs = -1;
                markDirty();
                refreshList();
            }
            QMessageBox::information(
                this, QStringLiteral("Dönüştürme tamamlandı"),
                QStringLiteral("Dönüştürülmüş video kaydedildi:\n%1").arg(convertedPath));
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Video dönüştürülemedi"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (videoToolWatcher_ == watcher) {
            videoToolWatcher_ = nullptr;
        }
    });
    videoToolWatcher_->setFuture(QtConcurrent::run(
        [entry, outputPath, ffmpeg, crf] {
            QTemporaryDir temporaryDirectory;
            const QString input =
                materializeVideoForTool(entry, temporaryDirectory);
            QTemporaryDir stagingDirectory(
                QDir(QFileInfo(outputPath).absolutePath())
                    .filePath(QStringLiteral(".packmanager-convert-XXXXXX")));
            if (!stagingDirectory.isValid()) {
                throw std::runtime_error("Dönüştürme için geçici klasör oluşturulamadı.");
            }
            const QString stagingPath = QDir(stagingDirectory.path()).filePath(
                QFileInfo(outputPath).fileName());
            runMediaTool(
                ffmpeg,
                {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                 QStringLiteral("error"), QStringLiteral("-i"), input,
                 QStringLiteral("-c:v"), QStringLiteral("libx264"),
                 QStringLiteral("-preset"), QStringLiteral("medium"),
                 QStringLiteral("-crf"), crf, QStringLiteral("-c:a"),
                 QStringLiteral("aac"), QStringLiteral("-b:a"),
                 QStringLiteral("128k"), QStringLiteral("-movflags"),
                 QStringLiteral("+faststart"), QStringLiteral("-y"), stagingPath});
            const QFileInfo output(stagingPath);
            if (!output.isFile() || output.size() <= 0) {
                throw std::runtime_error("FFmpeg başarılı oldu ancak çıktı dosyası oluşmadı.");
            }
            copyFileAtomically(stagingPath, outputPath);
            return QFileInfo(outputPath).absoluteFilePath();
        }));
}

void PackManagerWindow::removeCover()
{
    coverPath_->clear();
    coverPreview_->setPixmap(QPixmap());
    coverPreview_->setText(QStringLiteral("Kapak yok"));
    markFormDirty();
}

void PackManagerWindow::importDroppedFiles(const QStringList& paths)
{
    QStringList videoPaths;
    QStringList imagePaths;
    for (const QString& path : paths) {
        if (!QFileInfo(path).isFile()) {
            continue;
        }
        if (isVideoFile(path)) {
            videoPaths.push_back(QFileInfo(path).absoluteFilePath());
        } else if (isImageFile(path)) {
            imagePaths.push_back(QFileInfo(path).absoluteFilePath());
        }
    }
    if (videoPaths.isEmpty()) {
        if (imagePaths.size() == 1 && editingIndex_ >= 0) {
            coverPath_->setText(imagePaths.front());
            VideoEntry cover;
            cover.coverPath = imagePaths.front();
            updateCoverPreview(cover);
            markFormDirty();
            return;
        }
        QMessageBox::information(
            this, QStringLiteral("Video bulunamadı"),
            QStringLiteral("Sürükleyip bıraktığınız dosyalarda desteklenen bir video bulunamadı."));
        return;
    }

    QSet<QString> existingVideos;
    for (const VideoEntry& entry : entries_) {
        existingVideos.insert(canonicalPath(entry.videoIsEmbedded
            ? entry.assetContainerPath
            : entry.videoPath)
            + (entry.videoIsEmbedded
                   ? QLatin1Char('#') + QString::number(entry.videoOffset)
                   : QString()));
    }
    QMap<QString, QString> coversByName;
    for (const QString& path : imagePaths) {
        coversByName.insert(QFileInfo(path).completeBaseName().toCaseFolded(), path);
    }

    QStringList newVideoPaths;
    for (const QString& path : videoPaths) {
        if (existingVideos.contains(canonicalPath(path))) {
            continue;
        }
        existingVideos.insert(canonicalPath(path));
        newVideoPaths.push_back(path);
    }
    if (newVideoPaths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Yeni video yok"),
                                 QStringLiteral("Seçilen videolar pakette zaten bulunuyor."));
        return;
    }
    recordUndoState();
    int addedCount = 0;
    for (const QString& path : newVideoPaths) {
        VideoEntry entry;
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.title = QFileInfo(path).completeBaseName();
        entry.description.clear();
        entry.category = QStringLiteral("Genel");
        entry.videoPath = path;
        const QString pairedCover = coversByName.value(
            QFileInfo(path).completeBaseName().toCaseFolded());
        if (!pairedCover.isEmpty()) {
            entry.coverPath = pairedCover;
        } else if (videoPaths.size() == 1 && imagePaths.size() == 1) {
            entry.coverPath = imagePaths.front();
        }
        entries_.push_back(entry);
        if (!manualOrder_.isEmpty()) {
            manualOrder_.push_back(entry.id);
        }
        ++addedCount;
    }
    markDirty();
    refreshList();
    videoList_->setCurrentRow(entries_.size() - addedCount);
    playbackStatus_->setText(
        QStringLiteral("%1 video eklendi").arg(addedCount));
}

void PackManagerWindow::openPackage()
{
    if (activeProgress_ != nullptr) {
        QMessageBox::information(this, QStringLiteral("İşlem sürüyor"),
                                 QStringLiteral("Mevcut arka plan işlemi bitince paket açabilirsiniz."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("SafeTensors paketini aç"), QString(),
        QStringLiteral("SafeTensors paketleri (*.safetensors);;Tüm dosyalar (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Paket başlığı ve medya listesi okunuyor..."), QString(),
        0, 0, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    const QPointer<QProgressDialog> progressDialog = activeProgress_;
    auto* watcher = new QFutureWatcher<PackageValidationResult>(this);
    packageReadWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<PackageValidationResult>::finished, this,
            [this, watcher, progressDialog] {
        if (progressDialog != nullptr) {
            progressDialog->deleteLater();
            if (activeProgress_ == progressDialog) {
                activeProgress_ = nullptr;
            }
        }
        try {
            PackageValidationResult result = watcher->result();
            if (confirmDiscardChanges(QStringLiteral("paketi aç"))) {
                recoveryTimer_->stop();
                QFile::remove(appDataProjectPath());
                mediaPlayer_->stop();
                entries_ = std::move(result.entries);
                packageMetadata_ = result.metadata;
                manualOrder_.clear();
                for (VideoEntry& entry : entries_) {
                    manualOrder_.push_back(entryId(entry));
                }
                packagePath_ = QFileInfo(result.path).absoluteFilePath();
                projectPath_.clear();
                undoHistory_.clear();
                redoHistory_.clear();
                undoAction_->setEnabled(false);
                redoAction_->setEnabled(false);
                dirty_ = false;
                formDirty_ = false;
                refreshList();
                clearForm();
                playbackStatus_->setText(
                    QStringLiteral("Doğrulandı · %1 video · %2")
                        .arg(result.report.videoCount).arg(humanSize(result.report.fileSize)));
                if (result.report.issueCount > 0) {
                    QMessageBox warning(this);
                    warning.setIcon(QMessageBox::Warning);
                    warning.setWindowTitle(QStringLiteral("Paket doğrulama uyarıları"));
                    warning.setText(QStringLiteral(
                        "Paket açıldı, ancak bütünlük/imza kontrolünde uyarılar bulundu."));
                    warning.setDetailedText(result.report.issues.join(QLatin1Char('\n')));
                    warning.exec();
                }
                setWindowTitle(
                    QStringLiteral("PackManager — %1")
                        .arg(QFileInfo(packagePath_).fileName()));
                updateSummary();
            }
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Paket açılamadı"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (packageReadWatcher_ == watcher) {
            packageReadWatcher_ = nullptr;
        }
    });
    watcher->setFuture(QtConcurrent::run([this, path, progressDialog] {
        PackageValidationResult result;
        result.path = path;
        result.entries = readSafeTensors(path, &result.report, &result.metadata);
        verifySafeTensorsIntegrity(
            path, result.entries, &result.report.issues,
            [this, progressDialog](quint64 completed, quint64 total) {
                const int value = total == 0 ? 1000 : static_cast<int>(
                    std::min(1000.0, static_cast<double>(completed) * 1000.0
                                              / static_cast<double>(total)));
                QMetaObject::invokeMethod(this, [this, progressDialog, value, completed, total] {
                    if (progressDialog != nullptr) {
                        progressDialog->setRange(0, 1000);
                        progressDialog->setValue(value);
                        progressDialog->setLabelText(
                            QStringLiteral("Dosya bütünlüğü doğrulanıyor: %1 / %2")
                                .arg(completed).arg(total));
                    }
                }, Qt::QueuedConnection);
            });
        result.report.issueCount = result.report.issues.size();
        return result;
    }));
}

void PackManagerWindow::openProjectDraft()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Proje taslağını aç"), QString(),
        QStringLiteral("PackManager taslağı (*.pmp);;JSON dosyaları (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    try {
        PackageMetadata loadedMetadata;
        QVector<VideoEntry> loadedEntries = readProjectFile(path, &loadedMetadata);
        if (!confirmDiscardChanges(QStringLiteral("proje taslağını aç"))) {
            return;
        }
        recoveryTimer_->stop();
        QFile::remove(appDataProjectPath());
        mediaPlayer_->stop();
        entries_ = std::move(loadedEntries);
        packageMetadata_ = loadedMetadata;
        manualOrder_.clear();
        for (VideoEntry& entry : entries_) {
            manualOrder_.push_back(entryId(entry));
        }
        projectPath_ = QFileInfo(path).absoluteFilePath();
        packagePath_.clear();
        undoHistory_.clear();
        redoHistory_.clear();
        undoAction_->setEnabled(false);
        redoAction_->setEnabled(false);
        dirty_ = false;
        formDirty_ = false;
        refreshList();
        clearForm();
        setWindowTitle(QStringLiteral("PackManager — %1").arg(QFileInfo(path).fileName()));
        updateSummary();
        playbackStatus_->setText(
            QStringLiteral("Taslak yüklendi; kaynak medya yolları kontrol ediliyor."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Taslak açılamadı"),
                              QString::fromUtf8(error.what()));
    }
}

void PackManagerWindow::saveProjectDraft(bool saveAs)
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    QString path = saveAs ? QString() : projectPath_;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Proje taslağını kaydet"),
            QStringLiteral("video_projesi.pmp"),
            QStringLiteral("PackManager taslağı (*.pmp)"));
    }
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".pmp"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".pmp");
    }
    try {
        writeProjectFile(entries_, path, packageMetadata_);
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Taslak kaydedilemedi"),
                              QString::fromUtf8(error.what()));
        return;
    }
    projectPath_ = QFileInfo(path).absoluteFilePath();
    dirty_ = false;
    recoveryTimer_->stop();
    QFile::remove(appDataProjectPath());
    setWindowTitle(QStringLiteral("PackManager — %1").arg(QFileInfo(path).fileName()));
    playbackStatus_->setText(QStringLiteral("Proje taslağı kaydedildi."));
    updateSummary();
}

bool PackManagerWindow::savePackage()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return false;
    }
    return exportPackage();
}

bool PackManagerWindow::saveCurrentWorkspace()
{
    if (!projectPath_.isEmpty()) {
        saveProjectDraft(false);
        return !dirty_ && !formDirty_;
    }
    return savePackage();
}

bool PackManagerWindow::confirmDiscardChanges(const QString& action)
{
    if (!hasUnsavedChanges()) {
        return true;
    }
    QMessageBox prompt(QMessageBox::Warning, QStringLiteral("Kaydedilmemiş değişiklikler"),
                       QStringLiteral("%1 işleminden önce değişiklikleri kaydetmek ister misiniz?")
                           .arg(action),
                       QMessageBox::NoButton, this);
    QPushButton* saveButton = prompt.addButton(QStringLiteral("Kaydet"),
                                               QMessageBox::AcceptRole);
    QPushButton* discardButton = prompt.addButton(QStringLiteral("Kaydetmeden devam et"),
                                                  QMessageBox::DestructiveRole);
    QPushButton* cancelButton = prompt.addButton(QStringLiteral("İptal"),
                                                 QMessageBox::RejectRole);
    prompt.setDefaultButton(saveButton);
    prompt.exec();
    if (prompt.clickedButton() == saveButton) {
        return saveCurrentWorkspace();
    }
    Q_UNUSED(cancelButton);
    if (prompt.clickedButton() != discardButton) {
        return false;
    }
    recoveryTimer_->stop();
    QFile::remove(appDataProjectPath());
    return true;
}

void PackManagerWindow::createNewProject()
{
    if (!confirmDiscardChanges(QStringLiteral("yeni proje oluştur"))) {
        return;
    }
    recoveryTimer_->stop();
    QFile::remove(appDataProjectPath());
    mediaPlayer_->stop();
    recordUndoState();
    entries_.clear();
    manualOrder_.clear();
    packageMetadata_ = {};
    packagePath_.clear();
    projectPath_.clear();
    dirty_ = false;
    formDirty_ = false;
    refreshList();
    clearForm();
    setWindowTitle(QStringLiteral("PackManager"));
    updateSummary();
}

void PackManagerWindow::validatePackage()
{
    if (activeProgress_ != nullptr) {
        QMessageBox::information(this, QStringLiteral("İşlem sürüyor"),
                                 QStringLiteral("Diğer işlem bitince doğrulamayı başlatabilirsiniz."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Doğrulanacak paketi seç"), packagePath_,
        QStringLiteral("SafeTensors paketleri (*.safetensors);;Tüm dosyalar (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Paket ve medya verileri doğrulanıyor..."), QString(), 0, 1, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    auto* watcher = new QFutureWatcher<PackageValidationResult>(this);
    validationWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<PackageValidationResult>::finished, this,
            [this, watcher] {
        if (activeProgress_ != nullptr) {
            activeProgress_->setValue(activeProgress_->maximum());
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            const PackageValidationResult result = watcher->result();
            QMap<QString, int> categories;
            for (const VideoEntry& entry : result.entries) {
                ++categories[entry.category];
            }
            QStringList categoryLines;
            for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
                categoryLines.push_back(QStringLiteral("• %1: %2")
                                            .arg(it.key()).arg(it.value()));
            }
            QMessageBox dialog(QMessageBox::Information,
                               QStringLiteral("Paket doğrulama raporu"),
                               QStringLiteral("SafeTensors yapısı ve medya aralıkları geçerli.\n\n"
                                              "Videolar: %1\nTensor'lar: %2\n"
                                              "Medya verisi: %3\nDosya boyutu: %4\n\n"
                                              "Kategori dağılımı:\n%5\n\n"
                                              "Uyarı: %6")
                                   .arg(result.report.videoCount)
                                   .arg(result.report.tensorCount)
                                   .arg(humanSize(result.report.mediaSize),
                                        humanSize(result.report.fileSize),
                                        categoryLines.join(QLatin1Char('\n')))
                                   .arg(result.report.issueCount),
                               QMessageBox::Ok, this);
            dialog.setDetailedText(
                result.report.issues.isEmpty()
                    ? QStringLiteral("Doğrulama uyarısı bulunmadı.")
                    : result.report.issues.join(QLatin1Char('\n')));
            dialog.exec();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Paket doğrulaması başarısız"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (validationWatcher_ == watcher) {
            validationWatcher_ = nullptr;
        }
    });
    validationWatcher_->setFuture(QtConcurrent::run([this, path] {
        PackageValidationResult result;
        result.path = path;
        result.entries = readSafeTensors(path, &result.report);
        verifySafeTensorsIntegrity(
            path, result.entries, &result.report.issues,
            [this](quint64 completed, quint64 total) {
                QMetaObject::invokeMethod(this, [this, completed, total] {
                    if (activeProgress_ != nullptr) {
                        activeProgress_->setRange(0, static_cast<int>(total));
                        activeProgress_->setValue(static_cast<int>(completed));
                    }
                }, Qt::QueuedConnection);
            });
        QHash<QByteArray, QString> mediaHashes;
        QSet<QString> identifiers;
        int completed = 0;
        const int total = std::max(1, static_cast<int>(result.entries.size()) * 2);
        for (const VideoEntry& entry : result.entries) {
            if (identifiers.contains(entry.id)) {
                result.report.issues.push_back(
                    QStringLiteral("Tekrarlanan video kimliği: %1 (%2)")
                        .arg(entry.id, entry.title));
            }
            identifiers.insert(entry.id);
            const auto checkAsset = [&](quint64 offset, quint64 size,
                                        const QString& description) {
                const QByteArray digest = hashMediaRange(
                    QFileInfo(path).absoluteFilePath(), offset, size);
                const auto previous = mediaHashes.constFind(digest);
                if (previous != mediaHashes.cend()) {
                    result.report.issues.push_back(
                        QStringLiteral("Yinelenen medya içeriği: %1 ve %2")
                            .arg(previous.value(), description));
                } else {
                    mediaHashes.insert(digest, description);
                }
                ++completed;
                QMetaObject::invokeMethod(this, [this, completed, total] {
                    if (activeProgress_ != nullptr) {
                        activeProgress_->setRange(0, total);
                        activeProgress_->setValue(completed);
                        activeProgress_->setLabelText(
                            QStringLiteral("Medya doğrulanıyor: %1 / %2")
                                .arg(completed).arg(total));
                    }
                }, Qt::QueuedConnection);
            };
            checkAsset(entry.videoOffset, entry.videoSize,
                       QStringLiteral("Video: %1").arg(entry.title));
            if (entry.coverIsEmbedded) {
                checkAsset(entry.coverOffset, entry.coverSize,
                           QStringLiteral("Kapak: %1").arg(entry.title));
            } else {
                ++completed;
            }
        }
        result.report.issueCount = result.report.issues.size();
        return result;
    }));
}

void PackManagerWindow::editSettings()
{
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(language_ == QStringLiteral("en")
                              ? QStringLiteral("Settings")
                              : QStringLiteral("Ayarlar"));
    auto* layout = new QFormLayout(&dialog);
    auto* autoSave = new QCheckBox(
        language_ == QStringLiteral("en")
            ? QStringLiteral("Enable recovery autosave")
            : QStringLiteral("Kurtarma için otomatik kaydetmeyi etkinleştir"),
        &dialog);
    autoSave->setChecked(settings.value(QStringLiteral("recovery/enabled"), true).toBool());
    auto* updateCheck = new QCheckBox(
        language_ == QStringLiteral("en")
            ? QStringLiteral("Check GitHub releases for updates at startup")
            : QStringLiteral("Başlangıçta GitHub sürümlerinde güncelleme ara"),
        &dialog);
    updateCheck->setChecked(settings.value(QStringLiteral("updates/autoCheck"), true).toBool());
    auto* interval = new QSpinBox(&dialog);
    interval->setRange(1, 600);
    interval->setSuffix(language_ == QStringLiteral("en")
                            ? QStringLiteral(" sec")
                            : QStringLiteral(" sn"));
    interval->setValue(settings.value(QStringLiteral("recovery/intervalSeconds"), 3).toInt());
    layout->addRow(autoSave);
    layout->addRow(updateCheck);
    layout->addRow(language_ == QStringLiteral("en")
                       ? QStringLiteral("Autosave delay:")
                       : QStringLiteral("Otomatik kaydetme gecikmesi:"), interval);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings.setValue(QStringLiteral("recovery/enabled"), autoSave->isChecked());
    settings.setValue(QStringLiteral("recovery/intervalSeconds"), interval->value());
    settings.setValue(QStringLiteral("updates/autoCheck"), updateCheck->isChecked());
    recoveryTimer_->setInterval(interval->value() * 1000);
    if (!autoSave->isChecked()) {
        recoveryTimer_->stop();
    } else if (dirty_ || formDirty_) {
        scheduleRecoverySave();
    }
}

void PackManagerWindow::checkForUpdates(bool silent)
{
    if (updateCheckRunning_) {
        return;
    }
    updateCheckRunning_ = true;
    auto* manager = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(
        QStringLiteral("https://api.github.com/repos/0xe5a91f2d-ui/PackManager/releases/latest")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "PackManager");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = manager->get(request);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(15000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, timeout, silent] {
        timeout->stop();
        updateCheckRunning_ = false;
        const QByteArray response = reply->readAll();
        QString errorMessage;
        QJsonObject release;
        if (reply->error() != QNetworkReply::NoError) {
            const int statusCode =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            errorMessage = statusCode == 404
                ? QStringLiteral("GitHub deposunda henüz yayımlanmış bir Release yok.")
                : QStringLiteral("GitHub Releases denetlenemedi: %1")
                      .arg(reply->errorString());
        } else {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                errorMessage = QStringLiteral("GitHub geçerli bir release yanıtı döndürmedi.");
            } else {
                release = document.object();
            }
        }
        reply->deleteLater();
        manager->deleteLater();
        if (!errorMessage.isEmpty()) {
            statusBar()->showMessage(errorMessage, 10000);
            if (!silent) {
                QMessageBox::warning(this, QStringLiteral("Güncelleme denetimi başarısız"),
                                     errorMessage);
            }
            return;
        }

        const QString tag = release.value(QStringLiteral("tag_name")).toString().trimmed();
        QString versionText = tag;
        if (versionText.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
            versionText.remove(0, 1);
        }
        const QVersionNumber latest = QVersionNumber::fromString(versionText);
        const QVersionNumber current =
            QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if (latest.isNull() || current.isNull()) {
            const QString message = QStringLiteral("Kurulu veya yayınlanan sürüm numarası okunamadı.");
            statusBar()->showMessage(message, 10000);
            if (!silent) {
                QMessageBox::warning(this, QStringLiteral("Sürüm bilgisi geçersiz"), message);
            }
            return;
        }
        if (QVersionNumber::compare(latest, current) <= 0) {
            const QString message = QStringLiteral("PackManager güncel (%1).")
                                        .arg(QCoreApplication::applicationVersion());
            statusBar()->showMessage(message, 10000);
            if (!silent) {
                QMessageBox::information(this, QStringLiteral("Güncelleme denetimi"), message);
            }
            return;
        }

        const QString urlText = release.value(QStringLiteral("html_url")).toString();
        const QUrl releaseUrl(urlText);
        QMessageBox updateBox(QMessageBox::Information,
                              QStringLiteral("PackManager güncellemesi"),
                              QStringLiteral("Yeni sürüm bulundu: %1 (kurulu sürüm: %2)")
                                  .arg(tag, QCoreApplication::applicationVersion()),
                              QMessageBox::Open | QMessageBox::Close, this);
        updateBox.setDetailedText(release.value(QStringLiteral("body")).toString());
        updateBox.setButtonText(QMessageBox::Open, QStringLiteral("Sürüm sayfası"));
        updateBox.exec();
        if (updateBox.clickedButton() == updateBox.button(QMessageBox::Open)
            && releaseUrl.scheme() == QStringLiteral("https")
            && releaseUrl.host() == QStringLiteral("github.com")) {
            QDesktopServices::openUrl(releaseUrl);
        }
    });
}

void PackManagerWindow::showPackageHistory()
{
    if (packagePath_.isEmpty() || !QFileInfo::exists(packagePath_)) {
        QMessageBox::information(
            this, QStringLiteral("Sürüm geçmişi"),
            QStringLiteral("Önce diskte kayıtlı bir paketi açın veya kaydedin."));
        return;
    }
    const QString historyPath = packagePath_ + QStringLiteral(".history");
    const QFileInfoList backups = QDir(historyPath).entryInfoList(
        {QStringLiteral("*.bak")}, QDir::Files, QDir::Name | QDir::Reversed);
    if (backups.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Sürüm geçmişi"),
                                 QStringLiteral("Bu paket için henüz yedek sürüm yok."));
        return;
    }
    QStringList choices;
    for (const QFileInfo& backup : backups) {
        choices.push_back(backup.fileName());
    }
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, QStringLiteral("Paket sürüm geçmişi"),
        QStringLiteral("Geri yüklenecek sürümü seçin:"),
        choices, 0, false, &accepted);
    if (!accepted || selected.isEmpty() || !confirmDiscardChanges(QStringLiteral("sürümü geri yükle"))) {
        return;
    }
    const QString backupPath = QDir(historyPath).filePath(selected);
    try {
        PackageValidationResult restored;
        restored.path = backupPath;
        restored.entries = readSafeTensors(backupPath, &restored.report,
                                            &restored.metadata);
        verifySafeTensorsIntegrity(backupPath, restored.entries,
                                   &restored.report.issues);
        if (!restored.report.issues.isEmpty()) {
            throw std::runtime_error("Yedek sürüm bütünlük kontrolünü geçemedi.");
        }
        copyFileAtomically(backupPath, packagePath_);
        entries_ = std::move(restored.entries);
        packageMetadata_ = restored.metadata;
        manualOrder_.clear();
        for (VideoEntry& entry : entries_) {
            manualOrder_.push_back(entryId(entry));
        }
        undoHistory_.clear();
        redoHistory_.clear();
        dirty_ = false;
        formDirty_ = false;
        recoveryTimer_->stop();
        QFile::remove(appDataProjectPath());
        refreshList();
        clearForm();
        updateSummary();
        QMessageBox::information(
            this, QStringLiteral("Sürüm geri yüklendi"),
            QStringLiteral("Seçilen paket sürümü geri yüklendi."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Geri yükleme başarısız"),
                              QString::fromUtf8(error.what()));
    }
}

void PackManagerWindow::editPackageMetadata()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Paket bilgileri"));
    auto* form = new QFormLayout(&dialog);
    auto* title = new QLineEdit(packageMetadata_.title, &dialog);
    auto* description = new QTextEdit(&dialog);
    description->setPlainText(packageMetadata_.description);
    description->setMaximumHeight(120);
    auto* version = new QLineEdit(packageMetadata_.version, &dialog);
    auto* createdAt = new QLineEdit(packageMetadata_.createdAt, &dialog);
    createdAt->setPlaceholderText(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    form->addRow(QStringLiteral("Paket adı"), title);
    form->addRow(QStringLiteral("Açıklama"), description);
    form->addRow(QStringLiteral("Sürüm"), version);
    form->addRow(QStringLiteral("Oluşturulma tarihi (ISO 8601)"), createdAt);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (title->text().trimmed().isEmpty() || version->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Eksik paket bilgisi"),
                             QStringLiteral("Paket adı ve sürümü boş bırakılamaz."));
        return;
    }
    recordUndoState();
    packageMetadata_.title = title->text().trimmed();
    packageMetadata_.description = description->toPlainText().trimmed();
    packageMetadata_.version = version->text().trimmed();
    packageMetadata_.createdAt = createdAt->text().trimmed();
    markDirty();
    updateSummary();
}

void PackManagerWindow::captureVideoFrame()
{
    if (editingIndex_ < 0 || editingIndex_ >= entries_.size()) {
        QMessageBox::information(this, QStringLiteral("Video seçilmedi"),
                                 QStringLiteral("Önce kapak eklenecek videoyu seçin."));
        return;
    }
    if (lastVideoFrame_.isNull()) {
        if (mediaPlayer_->source().isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Video karesi hazır değil"),
                QStringLiteral("Önce videoyu oynatıp kapak yapmak istediğiniz karede duraklatın."));
            return;
        }
        frameCapturePending_ = true;
        mediaPlayer_->play();
        playbackStatus_->setText(QStringLiteral("Video karesi alınıyor..."));
        return;
    }
    const QString destination = QFileDialog::getSaveFileName(
        this, QStringLiteral("Video karesini kapak olarak kaydet"),
        QStringLiteral("video_kapagi.jpg"),
        QStringLiteral("JPEG resmi (*.jpg *.jpeg);;PNG resmi (*.png)"));
    if (destination.isEmpty()) {
        return;
    }
    QString output = destination;
    if (QFileInfo(output).suffix().isEmpty()) {
        output += QStringLiteral(".jpg");
    }
    if (!lastVideoFrame_.save(output)) {
        QMessageBox::critical(this, QStringLiteral("Kapak kaydedilemedi"),
                              QStringLiteral("Video karesi resim dosyasına yazılamadı."));
        return;
    }
    coverPath_->setText(output);
    VideoEntry entry = entries_.at(editingIndex_);
    entry.coverPath = output;
    entry.coverIsEmbedded = false;
    entry.coverOffset = 0;
    entry.coverSize = 0;
    updateCoverPreview(entry);
    markFormDirty();
}

void PackManagerWindow::findMissingMedia()
{
    QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Eksik medya dosyalarının aranacağı klasör"));
    if (root.isEmpty()) {
        return;
    }
    QSet<QString> missingPaths;
    for (const VideoEntry& entry : entries_) {
        if (entry.videoIsEmbedded && !QFileInfo(entry.assetContainerPath).isFile()) {
            missingPaths.insert(entry.assetContainerPath);
        } else if (!entry.videoIsEmbedded && !QFileInfo(entry.videoPath).isFile()) {
            missingPaths.insert(entry.videoPath);
        }
        if (entry.coverIsEmbedded && !QFileInfo(entry.assetContainerPath).isFile()) {
            missingPaths.insert(entry.assetContainerPath);
        } else if (!entry.coverIsEmbedded && !entry.coverPath.isEmpty()
                   && !QFileInfo(entry.coverPath).isFile()) {
            missingPaths.insert(entry.coverPath);
        }
    }
    if (missingPaths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Medya kaynakları"),
                                 QStringLiteral("Eksik video veya kapak dosyası bulunamadı."));
        return;
    }
    QHash<QString, QString> resolved;
    for (const QString& missingPath : missingPaths) {
        const QString wantedName = QFileInfo(missingPath).fileName();
        if (wantedName.isEmpty()) {
            continue;
        }
        QDirIterator iterator(root, QStringList{wantedName},
                              QDir::Files, QDirIterator::Subdirectories);
        if (iterator.hasNext()) {
            resolved.insert(missingPath, QFileInfo(iterator.next()).absoluteFilePath());
        }
    }
    if (resolved.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Dosyalar bulunamadı"),
                             QStringLiteral("Eksik medya dosyaları seçilen klasörde bulunamadı."));
        return;
    }
    recordUndoState();
    int restored = 0;
    for (VideoEntry& entry : entries_) {
        if (entry.videoIsEmbedded || entry.coverIsEmbedded) {
            const auto container = resolved.constFind(entry.assetContainerPath);
            if (container != resolved.cend()) {
                entry.assetContainerPath = container.value();
                ++restored;
            }
        }
        if (!entry.videoIsEmbedded) {
            const auto video = resolved.constFind(entry.videoPath);
            if (video != resolved.cend()) {
                entry.videoPath = video.value();
                ++restored;
            }
        }
        if (!entry.coverIsEmbedded) {
            const auto cover = resolved.constFind(entry.coverPath);
            if (cover != resolved.cend()) {
                entry.coverPath = cover.value();
                ++restored;
            }
        }
    }
    markDirty();
    refreshList();
    QMessageBox::information(
        this, QStringLiteral("Medya kaynakları bulundu"),
        QStringLiteral("%1 dosyanın bağlantısı güncellendi. Taslağı veya paketi kaydedin.")
            .arg(restored));
}

void PackManagerWindow::changeLanguage(const QString& language)
{
    language_ = language;
    QSettings().setValue(QStringLiteral("ui/language"), language_);
    updateLocalizedUi();
}

void PackManagerWindow::changeTheme(const QString& theme)
{
    theme_ = theme;
    QSettings().setValue(QStringLiteral("ui/theme"), theme_);
    applyCurrentTheme();
}

void PackManagerWindow::applyCurrentTheme()
{
    if (theme_ == QStringLiteral("dark")) {
        setStyleSheet(QStringLiteral(
            "QWidget { background: #232323; color: #eeeeee; }"
            "QLineEdit, QTextEdit, QPlainTextEdit, QListWidget, QComboBox {"
            " background: #303030; color: #f0f0f0; border: 1px solid #555; }"
            "QPushButton { background: #383838; border: 1px solid #666;"
            " padding: 5px 8px; } QPushButton:hover { background: #484848; }"
            "QGroupBox { border: 1px solid #555; margin-top: 8px; }"
            "QMenuBar, QMenu { background: #292929; color: #eee; }"
            "QMenu::item:selected { background: #505050; }"));
        coverPreview_->setStyleSheet(
            QStringLiteral("QLabel { background: #161616; color: #ddd; }"));
    } else {
        setStyleSheet(QString());
        coverPreview_->setStyleSheet(
            QStringLiteral("QLabel { background: #202020; color: #ddd; }"));
    }
}

void PackManagerWindow::updateLocalizedUi()
{
    const bool english = language_ == QStringLiteral("en");
    const QHash<QString, QString> turkishToEnglish{
            {"Yeni", "New"}, {"Taslak aç", "Open project"}, {"Taslağı kaydet", "Save project"},
            {"Taslağı farklı kaydet", "Save project as"}, {"Paket aç", "Open package"},
            {"Paket doğrula", "Validate package"}, {"Videoları toplu ekle...", "Add videos..."},
            {"Seçileni sil", "Delete selected"}, {"Seçilenleri sil", "Delete selected"},
            {"Seçilenlere kapak uygula", "Apply cover to selected"},
            {"Video değiştir...", "Replace video..."}, {"Kapak değiştir...", "Replace cover..."},
            {"Kapağı kaldır", "Remove cover"}, {"Başlık", "Title"}, {"Kategori", "Category"},
            {"Açıklama", "Description"}, {"Formu temizle", "Clear form"},
            {"Yeni videoyu listeye ekle", "Add video to list"},
            {"Değişiklikleri kaydet", "Save changes"}, {"Videoyu oynat", "Play video"},
            {"Duraklat", "Pause"}, {"Durdur", "Stop"},
            {"Bu kareyi kapak yap", "Use frame as cover"},
            {"Paketi kaydet (.safetensors)", "Save package (.safetensors)"},
            {"Düzen", "Edit"}, {"Görünüm", "View"}, {"Paket", "Package"},
            {"Geri al", "Undo"}, {"Yinele", "Redo"},
            {"Kopyala", "Copy"}, {"Yapıştır", "Paste"}, {"Çoğalt", "Duplicate"},
            {"Kategorileri yönet...", "Manage categories..."},
            {"Paket bilgileri...", "Package information..."},
            {"Sürüm geçmişi...", "Version history..."},
            {"Ayarlar...", "Settings..."},
            {"Altyazıyı görüntüle...", "View subtitles..."},
            {"Video teknik bilgileri...", "Video technical information..."},
            {"Videoyu dönüştür...", "Convert video..."},
            {"Güncellemeleri denetle...", "Check for updates..."},
            {"Etiketler", "Tags"}, {"Koleksiyon", "Collection"},
            {"Altyazı", "Subtitles"}, {"SRT/VTT seç...", "Choose SRT/VTT..."},
            {"Seçilenleri ayrı paket olarak kaydet...", "Export selected as a package..."},
            {"İçeriği klasöre çıkar...", "Extract contents..."},
            {"Medya tensorlarını göm (Hugging Face)", "Embed media tensors (Hugging Face)"},
            {"Eksik medya dosyalarını bul...", "Relink missing media..."},
            {"Açık tema", "Light theme"}, {"Koyu tema", "Dark theme"},
            {"Türkçe", "Turkish"}, {"Tüm kategoriler", "All categories"},
            {"Paket sırası", "Package order"}, {"Başlığa göre", "By title"},
            {"İkincil sıralama: yok", "Secondary sort: none"},
            {"Kategoriye göre", "By category"}, {"Boyuta göre", "By size"},
            {"Süreye göre", "By duration"}, {"Ayrıntılı liste", "Detailed list"},
            {"Küçük resimler", "Thumbnails"}, {"Kategoriler...", "Categories..."},
            {"Kategori", "Category"}, {"Bir video seçin.", "Select a video."},
            {"Kapak yok", "No cover"}, {"Devam et", "Resume"},
            {"Video dosyası", "Video file"}, {"Kapak resmi", "Cover image"},
            {"Video ve kapak dosyaları paket içinde saklanır.",
             "Video and cover files are stored inside the package."},
            {"Paket videoları", "Videos"}, {"Video bilgileri", "Video details"},
            {"Paket içeriği önizleme", "Media preview"},
            {"Düzenleniyor:", "Editing:"}, {"Paket", "Package"},
            {"English", "English"}, {"Süreye göre", "By duration"},
            {"Başlık", "Title"}, {"Açıklama", "Description"},
            {"Hakkında...", "About..."}
        };
    QHash<QString, QString> englishToTurkish;
    for (auto it = turkishToEnglish.cbegin(); it != turkishToEnglish.cend(); ++it) {
        if (!englishToTurkish.contains(it.value())) {
            englishToTurkish.insert(it.value(), it.key());
        }
    }
    const auto translate = [&turkishToEnglish, &englishToTurkish, english](
                               const QString& text) {
        const auto& dictionary = english ? turkishToEnglish : englishToTurkish;
        return dictionary.value(text, text);
    };
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setText(translate(button->text()));
    }
    for (QLabel* label : findChildren<QLabel*>()) {
        label->setText(translate(label->text()));
    }
    for (QGroupBox* group : findChildren<QGroupBox*>()) {
        group->setTitle(translate(group->title()));
    }
    for (QComboBox* combo : findChildren<QComboBox*>()) {
        const QSignalBlocker blocker(combo);
        const QString currentText = translate(combo->currentText());
        for (int row = 0; row < combo->count(); ++row) {
            combo->setItemText(row, translate(combo->itemText(row)));
        }
        if (combo->isEditable()) {
            combo->setEditText(currentText);
        } else {
            combo->setCurrentText(currentText);
        }
    }
    for (QAction* action : menuBar()->findChildren<QAction*>()) {
        action->setText(translate(action->text()));
    }
    if (english) {
        searchField_->setPlaceholderText(QStringLiteral("Search title, description or category..."));
        tags_->setPlaceholderText(QStringLiteral("comma-separated tags"));
    } else {
        searchField_->setPlaceholderText(QStringLiteral("Başlık, açıklama veya kategori ara..."));
        tags_->setPlaceholderText(QStringLiteral("virgülle ayrılmış etiketler"));
    }
}
void PackManagerWindow::reorderEntries()
{
    const QString selectedId = videoList_->currentItem() == nullptr
        ? QString()
        : videoList_->currentItem()->data(Qt::UserRole).toString();
    QMap<QString, VideoEntry> byId;
    for (VideoEntry& entry : entries_) {
        byId.insert(entryId(entry), entry);
    }
    QVector<VideoEntry> reordered;
    reordered.reserve(entries_.size());
    for (int row = 0; row < videoList_->count(); ++row) {
        const QString id = videoList_->item(row)->data(Qt::UserRole).toString();
        const auto it = byId.constFind(id);
        if (it == byId.cend()) {
            refreshList();
            return;
        }
        reordered.push_back(it.value());
    }
    if (reordered.size() != entries_.size()) {
        refreshList();
        return;
    }
    bool sameOrder = reordered.size() == entries_.size();
    for (qsizetype index = 0; sameOrder && index < reordered.size(); ++index) {
        sameOrder = reordered.at(index).id == entries_.at(index).id;
    }
    if (sameOrder) {
        return;
    }
    recordUndoState();
    entries_ = std::move(reordered);
    manualOrder_.clear();
    {
        const QSignalBlocker blocker(sortOrder_);
        sortOrder_->setCurrentIndex(0);
    }
    refreshList();
    if (!selectedId.isEmpty()) {
        for (int row = 0; row < entries_.size(); ++row) {
            if (entries_[row].id == selectedId) {
                videoList_->setCurrentRow(row);
                break;
            }
        }
    }
    markDirty();
}

void PackManagerWindow::bulkEditCategory()
{
    const QList<QListWidgetItem*> selected = videoList_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Seçim yok"),
                                 QStringLiteral("Önce düzenlenecek videoları seçin."));
        return;
    }
    QStringList categories;
    for (int index = 0; index < category_->count(); ++index) {
        const QString value = category_->itemText(index).trimmed();
        if (!value.isEmpty() && !categories.contains(value)) {
            categories.push_back(value);
        }
    }
    for (const VideoEntry& entry : entries_) {
        if (!categories.contains(entry.category)) {
            categories.push_back(entry.category);
        }
    }
    bool accepted = false;
    const QString category = QInputDialog::getItem(
        this, QStringLiteral("Toplu kategori düzenleme"),
        QStringLiteral("%1 videoya uygulanacak kategori:").arg(selected.size()),
        categories, 0, true, &accepted);
    if (!accepted || category.trimmed().isEmpty()) {
        return;
    }
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    recordUndoState();
    for (QListWidgetItem* item : selected) {
        const QString id = item->data(Qt::UserRole).toString();
        for (VideoEntry& entry : entries_) {
            if (entry.id == id) {
                entry.category = category.trimmed();
                break;
            }
        }
    }
    markDirty();
    refreshList();
}

void PackManagerWindow::bulkApplyCover()
{
    const QList<QListWidgetItem*> selected = videoList_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Seçim yok"),
                                 QStringLiteral("Önce kapak uygulanacak videoları seçin."));
        return;
    }
    const QString cover = QFileDialog::getOpenFileName(
        this, QStringLiteral("Seçilen videolara kapak uygula"), QString(),
        QStringLiteral("Resim dosyaları (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (cover.isEmpty()) {
        return;
    }
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    recordUndoState();
    for (QListWidgetItem* item : selected) {
        const QString id = item->data(Qt::UserRole).toString();
        for (VideoEntry& entry : entries_) {
            if (entry.id == id) {
                entry.coverPath = cover;
                entry.coverIsEmbedded = false;
                entry.coverOffset = 0;
                entry.coverSize = 0;
                break;
            }
        }
    }
    markDirty();
    refreshList();
}

void PackManagerWindow::deleteSelectedEntries()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    QList<QListWidgetItem*> selected = videoList_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Seçim yok"),
                                 QStringLiteral("Önce silinecek videoları seçin."));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Videoları sil"),
            QStringLiteral("%1 video paket listesinden silinsin mi?")
                .arg(selected.size()))
        != QMessageBox::Yes) {
        return;
    }
    QStringList ids;
    for (QListWidgetItem* item : selected) {
        ids.push_back(item->data(Qt::UserRole).toString());
    }
    recordUndoState();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&ids](const VideoEntry& entry) {
                                      return ids.contains(entry.id);
                                  }),
                   entries_.end());
    manualOrder_.erase(
        std::remove_if(manualOrder_.begin(), manualOrder_.end(),
                       [&ids](const QString& id) { return ids.contains(id); }),
        manualOrder_.end());
    markDirty();
    refreshList();
    clearForm();
}

void PackManagerWindow::editCategories()
{
    QStringList categories;
    for (int index = 0; index < category_->count(); ++index) {
        categories.push_back(category_->itemText(index));
    }
    for (const VideoEntry& entry : entries_) {
        if (!categories.contains(entry.category)) {
            categories.push_back(entry.category);
        }
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Kategorileri yönet"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new QPlainTextEdit(&dialog);
    editor->setPlaceholderText(QStringLiteral("Her satıra bir kategori yazın"));
    editor->setPlainText(categories.join(QLatin1Char('\n')));
    layout->addWidget(new QLabel(
        QStringLiteral("Kategorileri düzenleyin; kullanılmayan satırları kaldırın."), &dialog));
    layout->addWidget(editor);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList updated;
    for (const QString& line : editor->toPlainText().split(QLatin1Char('\n'))) {
        const QString category = line.trimmed();
        if (!category.isEmpty() && !updated.contains(category)) {
            updated.push_back(category);
        }
    }
    if (updated.isEmpty()) {
        updated.push_back(QStringLiteral("Genel"));
    }
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }

    const QStringList oldCategories = [&] {
        QStringList result;
        for (int index = 0; index < category_->count(); ++index) {
            result.push_back(category_->itemText(index));
        }
        return result;
    }();
    QStringList removed;
    for (const QString& category : oldCategories) {
        if (!updated.contains(category)) {
            removed.push_back(category);
        }
    }
    if (!removed.isEmpty() && !entries_.isEmpty()) {
        QStringList replacements = updated;
        bool accepted = false;
        const QString fallback = QInputDialog::getItem(
            this, QStringLiteral("Silinen kategoriler"),
            QStringLiteral("Bu kategorilerdeki videolar hangi kategoriye taşınsın?\n%1")
                .arg(removed.join(QStringLiteral(", "))),
            replacements, 0, false, &accepted);
        if (!accepted) {
            return;
        }
        recordUndoState();
        for (VideoEntry& entry : entries_) {
            if (removed.contains(entry.category)) {
                entry.category = fallback;
            }
        }
        markDirty();
    }

    {
        const QSignalBlocker blocker(category_);
        category_->clear();
        category_->addItems(updated);
    }
    categoryFilter_->clear();
    categoryFilter_->addItem(language_ == QStringLiteral("en")
                                 ? QStringLiteral("All categories")
                                 : QStringLiteral("Tüm kategoriler"));
    categoryFilter_->addItems(updated);
    applyFilters();
}

void PackManagerWindow::applyFilters()
{
    const QString search = searchField_->text().trimmed().toCaseFolded();
    const QString category = categoryFilter_->currentIndex() <= 0
        ? QString()
        : categoryFilter_->currentText();
    for (int index = 0; index < entries_.size(); ++index) {
        const VideoEntry& entry = entries_.at(index);
        const bool matchesSearch = search.isEmpty()
            || entry.title.toCaseFolded().contains(search)
            || entry.description.toCaseFolded().contains(search)
            || entry.category.toCaseFolded().contains(search)
            || entry.collection.toCaseFolded().contains(search)
            || std::any_of(entry.tags.cbegin(), entry.tags.cend(),
                           [&search](const QString& tag) {
                               return tag.toCaseFolded().contains(search);
                           });
        const bool matchesCategory = category.isEmpty() || entry.category == category;
        videoList_->setRowHidden(index, !matchesSearch || !matchesCategory);
    }
}

void PackManagerWindow::changeSortOrder(int index)
{
    if (entries_.size() < 2) {
        return;
    }
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    if (index == 0) {
        if (manualOrder_.isEmpty()) {
            return;
        }
        QMap<QString, VideoEntry> byId;
        for (const VideoEntry& entry : entries_) {
            byId.insert(entry.id, entry);
        }
        QVector<VideoEntry> restored;
        restored.reserve(entries_.size());
        for (const QString& id : manualOrder_) {
            const auto it = byId.constFind(id);
            if (it != byId.cend()) {
                restored.push_back(it.value());
                byId.remove(id);
            }
        }
        for (const VideoEntry& entry : entries_) {
            if (byId.contains(entry.id)) {
                restored.push_back(entry);
            }
        }
        manualOrder_.clear();
        entries_ = std::move(restored);
        markDirty();
        refreshList();
        return;
    }
    recordUndoState();
    if (manualOrder_.isEmpty()) {
        for (const VideoEntry& entry : entries_) {
            manualOrder_.push_back(entry.id);
        }
    }
    const int secondaryCriterion = secondarySortOrder_->currentData().toInt();
    const auto compareByCriterion = [](const VideoEntry& left,
                                       const VideoEntry& right, int criterion) {
        switch (criterion) {
        case 1:
            return left.title.compare(right.title, Qt::CaseInsensitive);
        case 2:
            return left.category.compare(right.category, Qt::CaseInsensitive);
        case 3: {
            const quint64 leftSize = entryMediaSize(left);
            const quint64 rightSize = entryMediaSize(right);
            return leftSize == rightSize ? 0 : (leftSize > rightSize ? -1 : 1);
        }
        case 4:
            return left.durationMs == right.durationMs
                ? 0 : (left.durationMs > right.durationMs ? -1 : 1);
        default:
            return 0;
        }
    };
    std::stable_sort(entries_.begin(), entries_.end(),
                     [index, secondaryCriterion, &compareByCriterion](
                         const VideoEntry& left, const VideoEntry& right) {
                         const int primary = compareByCriterion(left, right, index);
                         if (primary != 0) {
                             return primary < 0;
                         }
                         const int secondary =
                             compareByCriterion(left, right, secondaryCriterion);
                         if (secondary != 0) {
                             return secondary < 0;
                         }
                         return left.title.compare(
                                    right.title, Qt::CaseInsensitive) < 0;
                     });
    markDirty();
    refreshList();
}

void PackManagerWindow::undo()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    if (undoHistory_.isEmpty()) {
        return;
    }
    redoHistory_.push_back({entries_, packageMetadata_, manualOrder_});
    const EditSnapshot state = undoHistory_.takeLast();
    entries_ = state.entries;
    packageMetadata_ = state.metadata;
    manualOrder_ = state.manualOrder;
    editingIndex_ = -1;
    formDirty_ = false;
    markDirty();
    refreshList();
    clearForm();
    undoAction_->setEnabled(!undoHistory_.isEmpty());
    redoAction_->setEnabled(true);
}

void PackManagerWindow::redo()
{
    if (redoHistory_.isEmpty()) {
        return;
    }
    undoHistory_.push_back({entries_, packageMetadata_, manualOrder_});
    const EditSnapshot state = redoHistory_.takeLast();
    entries_ = state.entries;
    packageMetadata_ = state.metadata;
    manualOrder_ = state.manualOrder;
    editingIndex_ = -1;
    formDirty_ = false;
    markDirty();
    refreshList();
    clearForm();
    undoAction_->setEnabled(true);
    redoAction_->setEnabled(!redoHistory_.isEmpty());
}

void PackManagerWindow::copySelectedEntries()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    entryClipboard_.clear();
    QSet<QString> selectedIds;
    for (QListWidgetItem* item : videoList_->selectedItems()) {
        selectedIds.insert(item->data(Qt::UserRole).toString());
    }
    for (const VideoEntry& entry : entries_) {
        if (selectedIds.contains(entry.id)) {
            entryClipboard_.push_back(entry);
        }
    }
    if (entryClipboard_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Seçim yok"),
                                 QStringLiteral("Önce kopyalanacak videoları seçin."));
    }
}

void PackManagerWindow::pasteEntries()
{
    if (entryClipboard_.isEmpty() || (formDirty_ && !saveCurrentEntry())) {
        return;
    }
    recordUndoState();
    if (manualOrder_.isEmpty()) {
        for (const VideoEntry& entry : entries_) {
            manualOrder_.push_back(entry.id);
        }
    }
    const int firstAdded = entries_.size();
    for (VideoEntry entry : entryClipboard_) {
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.title += QStringLiteral(" (kopya)");
        manualOrder_.push_back(entry.id);
        entries_.push_back(std::move(entry));
    }
    markDirty();
    refreshList();
    videoList_->clearSelection();
    for (int row = firstAdded; row < entries_.size(); ++row) {
        videoList_->item(row)->setSelected(true);
    }
    videoList_->setCurrentRow(firstAdded);
}

void PackManagerWindow::duplicateSelectedEntries()
{
    copySelectedEntries();
    if (!entryClipboard_.isEmpty()) {
        pasteEntries();
    }
}

void PackManagerWindow::markDirty()
{
    dirty_ = true;
    const QString baseTitle = projectPath_.isEmpty()
        ? QStringLiteral("PackManager")
        : QStringLiteral("PackManager — %1").arg(QFileInfo(projectPath_).fileName());
    setWindowTitle(baseTitle + QLatin1Char('*'));
    scheduleRecoverySave();
    updateSummary();
}

void PackManagerWindow::recordUndoState()
{
    undoHistory_.push_back({entries_, packageMetadata_, manualOrder_});
    if (undoHistory_.size() > 50) {
        undoHistory_.removeFirst();
    }
    redoHistory_.clear();
    undoAction_->setEnabled(!undoHistory_.isEmpty());
    redoAction_->setEnabled(false);
}

void PackManagerWindow::markFormDirty()
{
    if (loadingEntry_) {
        return;
    }
    formDirty_ = true;
    const QString baseTitle = projectPath_.isEmpty()
        ? QStringLiteral("PackManager")
        : QStringLiteral("PackManager — %1").arg(QFileInfo(projectPath_).fileName());
    setWindowTitle(baseTitle + QLatin1Char('*'));
    scheduleRecoverySave();
    updateEditorState();
    updateSummary();
}

void PackManagerWindow::scheduleRecoverySave()
{
    if (recoveryTimer_ != nullptr
        && QSettings().value(QStringLiteral("recovery/enabled"), true).toBool()) {
        recoveryTimer_->start();
    }
}

void PackManagerWindow::saveRecoveryDraft()
{
    QVector<VideoEntry> recoveryEntries = entries_;
    if (formDirty_ && !videoPath_->text().trimmed().isEmpty()
        && !title_->text().trimmed().isEmpty()
        && !category_->currentText().trimmed().isEmpty()) {
        VideoEntry pending;
        if (editingIndex_ >= 0 && editingIndex_ < entries_.size()) {
            pending = entries_.at(editingIndex_);
        } else {
            pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        const QString videoPath = videoPath_->text();
        if (pending.videoPath != videoPath) {
            pending.videoPath = videoPath;
            pending.videoIsEmbedded = false;
            pending.videoOffset = 0;
            pending.videoSize = 0;
            pending.durationMs = -1;
        }
        const QString coverPath = coverPath_->text();
        if (pending.coverPath != coverPath) {
            pending.coverPath = coverPath;
            pending.coverIsEmbedded = false;
            pending.coverOffset = 0;
            pending.coverSize = 0;
        }
        pending.title = title_->text().trimmed();
        pending.description = description_->toPlainText().trimmed();
        pending.category = category_->currentText().trimmed();
        if (editingIndex_ >= 0 && editingIndex_ < recoveryEntries.size()) {
            recoveryEntries[editingIndex_] = pending;
        } else {
            recoveryEntries.push_back(pending);
        }
    }
    if (recoveryEntries.isEmpty()) {
        QFile::remove(appDataProjectPath());
        return;
    }
    const QString path = appDataProjectPath();
    const QFileInfo recoveryInfo(path);
    if (!QDir().mkpath(recoveryInfo.absolutePath())) {
        playbackStatus_->setText(QStringLiteral("Kurtarma taslağı klasörü oluşturulamadı."));
        return;
    }
    try {
        writeProjectFile(recoveryEntries, path, packageMetadata_);
        playbackStatus_->setText(QStringLiteral("Değişiklikler kurtarma taslağına kaydedildi."));
    } catch (const std::exception& error) {
        playbackStatus_->setText(
            QStringLiteral("Otomatik kurtarma kaydedilemedi: %1")
                .arg(QString::fromUtf8(error.what())));
    }
}

void PackManagerWindow::restoreRecoveryDraft()
{
    const QString path = appDataProjectPath();
    if (!QFileInfo(path).isFile()) {
        return;
    }
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, QStringLiteral("Kurtarma taslağı bulundu"),
        QStringLiteral("Önceki oturumdan kaydedilmemiş çalışmalar var. Kurtarma taslağı açılsın mı?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        QFile::remove(path);
        return;
    }
    try {
        entries_ = readProjectFile(path, &packageMetadata_);
        manualOrder_.clear();
        for (VideoEntry& entry : entries_) {
            manualOrder_.push_back(entryId(entry));
        }
        projectPath_.clear();
        packagePath_.clear();
        dirty_ = true;
        formDirty_ = false;
        refreshList();
        clearForm();
        setWindowTitle(QStringLiteral("PackManager — kurtarılan çalışma*"));
        playbackStatus_->setText(QStringLiteral("Kurtarma taslağı yüklendi; çalışmayı kaydedin."));
    } catch (const std::exception& error) {
        QMessageBox::warning(
            this, QStringLiteral("Kurtarma taslağı açılamadı"),
            QStringLiteral("%1\n\nTaslak dosyası korunuyor: %2")
                .arg(QString::fromUtf8(error.what()), path));
    }
}

bool PackManagerWindow::hasUnsavedChanges() const
{
    return dirty_ || formDirty_;
}

void PackManagerWindow::saveEntry()
{
    saveCurrentEntry();
}

bool PackManagerWindow::saveCurrentEntry()
{
    if (videoPath_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Eksik bilgi"),
                             QStringLiteral("Önce bir video dosyası seçin."));
        return false;
    }
    if (title_->text().trimmed().isEmpty() || category_->currentText().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Eksik bilgi"),
                             QStringLiteral("Başlık ve kategori zorunludur."));
        return false;
    }

    VideoEntry entry;
    int savedIndex = editingIndex_;
    recordUndoState();
    if (editingIndex_ < 0) {
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entries_.push_back(entry);
        savedIndex = entries_.size() - 1;
    } else {
        const VideoEntry& previous = entries_.at(editingIndex_);
        entry = previous;
    }
    const QString newVideoPath = videoPath_->text();
    if (entry.videoPath != newVideoPath) {
        entry.videoPath = newVideoPath;
        entry.videoIsEmbedded = false;
        entry.videoOffset = 0;
        entry.videoSize = 0;
        entry.durationMs = -1;
    }
    const QString newCoverPath = coverPath_->text();
    if (entry.coverPath != newCoverPath) {
        entry.coverPath = newCoverPath;
        entry.coverIsEmbedded = false;
        entry.coverOffset = 0;
        entry.coverSize = 0;
        entry.coverSha256.clear();
    }
    const QString newSubtitlePath = subtitlePath_->text();
    if (entry.subtitlePath != newSubtitlePath) {
        entry.subtitlePath = newSubtitlePath;
        entry.subtitleIsEmbedded = false;
        entry.subtitleOffset = 0;
        entry.subtitleSize = 0;
        entry.subtitleSha256.clear();
    }
    entry.title = title_->text().trimmed();
    entry.description = description_->toPlainText().trimmed();
    entry.category = category_->currentText().trimmed();
    entry.tags = tags_->text().split(
        QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts);
    for (QString& tag : entry.tags) {
        tag = tag.trimmed();
    }
    entry.tags.removeAll(QString());
    entry.tags.removeDuplicates();
    entry.collection = collection_->text().trimmed();
    if (savedIndex < entries_.size()) {
        entries_[savedIndex] = entry;
    }
    formDirty_ = false;
    markDirty();
    refreshList();
    videoList_->setCurrentRow(savedIndex);
    updateEditorState();
    return true;
}

void PackManagerWindow::loadSelectedEntry(int row)
{
    if (row < 0 || row >= entries_.size()) {
        editingIndex_ = -1;
        return;
    }
    if (formDirty_ && row != editingIndex_) {
        QMessageBox prompt(QMessageBox::Warning,
                           QStringLiteral("Kaydedilmemiş düzenleme"),
                           QStringLiteral("Başka bir videoya geçmeden önce değişiklikleri kaydetmek ister misiniz?"),
                           QMessageBox::NoButton, this);
        QPushButton* saveButton = prompt.addButton(
            QStringLiteral("Değişiklikleri kaydet"), QMessageBox::AcceptRole);
        QPushButton* discardButton = prompt.addButton(
            QStringLiteral("Atla"), QMessageBox::DestructiveRole);
        prompt.addButton(QStringLiteral("İptal"), QMessageBox::RejectRole);
        prompt.setDefaultButton(saveButton);
        prompt.exec();
        if (prompt.clickedButton() == saveButton) {
            if (!saveCurrentEntry()) {
                const QSignalBlocker blocker(videoList_);
                videoList_->setCurrentRow(editingIndex_);
                return;
            }
            videoList_->setCurrentRow(row);
            return;
        }
        if (prompt.clickedButton() != discardButton) {
            const QSignalBlocker blocker(videoList_);
            videoList_->setCurrentRow(editingIndex_);
            return;
        }
    }
    mediaPlayer_->stop();
    mediaPlayer_->setSource(QUrl());
    playbackPath_.clear();
    lastVideoFrame_ = QImage();
    frameCapturePending_ = false;
    loadingEntry_ = true;
    formDirty_ = false;
    editingIndex_ = row;
    const VideoEntry& entry = entries_.at(row);
    videoPath_->setText(entry.videoPath);
    coverPath_->setText(entry.coverPath);
    title_->setText(entry.title);
    category_->setCurrentText(entry.category);
    description_->setPlainText(entry.description);
    tags_->setText(entry.tags.join(QStringLiteral(", ")));
    collection_->setText(entry.collection);
    subtitlePath_->setText(entry.subtitlePath);
    updateCoverPreview(entry);
    updateEditorState();
    playbackStatus_->setText(
        QStringLiteral("%1 · %2").arg(entry.title, entry.category));
    loadingEntry_ = false;
}

void PackManagerWindow::removeSelectedEntry()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    const int row = videoList_->currentRow();
    if (row < 0 || row >= entries_.size()) {
        return;
    }
    recordUndoState();
    entries_.removeAt(row);
    markDirty();
    refreshList();
    clearForm();
}

void PackManagerWindow::clearForm()
{
    loadingEntry_ = true;
    formDirty_ = false;
    mediaPlayer_->stop();
    mediaPlayer_->setSource(QUrl());
    playbackPath_.clear();
    videoPath_->clear();
    coverPath_->clear();
    subtitlePath_->clear();
    title_->clear();
    tags_->clear();
    collection_->clear();
    category_->setCurrentIndex(0);
    category_->setEditText(category_->itemText(0));
    description_->clear();
    editingIndex_ = -1;
    videoList_->clearSelection();
    videoList_->setCurrentRow(-1);
    coverPreview_->setPixmap(QPixmap());
    coverPreview_->setText(QStringLiteral("Kapak yok"));
    playbackStatus_->setText(QStringLiteral("Bir video seçin."));
    loadingEntry_ = false;
    updateEditorState();
}

void PackManagerWindow::updateEditorState()
{
    if (editingIndex_ >= 0 && editingIndex_ < entries_.size()) {
        editStatus_->setText(
            QStringLiteral("Düzenleniyor: %1 — değişiklikleri kaydetmeyi unutmayın.")
                .arg(entries_.at(editingIndex_).title));
        saveEntryButton_->setText(QStringLiteral("Değişiklikleri kaydet"));
        return;
    }
    editStatus_->setText(
        QStringLiteral("Yeni video ekleyin veya düzenlemek için listeden bir video seçin."));
    saveEntryButton_->setText(QStringLiteral("Yeni videoyu listeye ekle"));
}

bool PackManagerWindow::exportPackage()
{
    if (entries_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Boş paket"),
                             QStringLiteral("Önce listeye en az bir video ekleyin."));
        return false;
    }

    const QString destination = QFileDialog::getSaveFileName(
        this, QStringLiteral("Paketi kaydet"),
        packagePath_.isEmpty() ? QStringLiteral("video_paketi.safetensors") : packagePath_,
        QStringLiteral("SafeTensors paketi (*.safetensors)"));
    if (destination.isEmpty()) {
        return false;
    }

    QString outputPath = destination;
    if (!outputPath.endsWith(QStringLiteral(".safetensors"), Qt::CaseInsensitive)) {
        outputPath += QStringLiteral(".safetensors");
    }

    const bool embedMedia = true;
    QString signingAuthor;
    startPackageExport(entries_, outputPath, true, embedMedia, signingAuthor);
    return true;
}

void PackManagerWindow::selectEntriesForExport()
{
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    QVector<VideoEntry> selected;
    for (QListWidgetItem* item : videoList_->selectedItems()) {
        const QString id = item->data(Qt::UserRole).toString();
        const auto found = std::find_if(entries_.cbegin(), entries_.cend(),
                                        [&id](const VideoEntry& entry) {
                                            return entry.id == id;
                                        });
        if (found != entries_.cend()) {
            selected.push_back(*found);
        }
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Seçim yok"),
                                 QStringLiteral("Önce dışa aktarılacak videoları seçin."));
        return;
    }
    exportPackageEntries(selected, QStringLiteral("secili_videolar.safetensors"));
}

void PackManagerWindow::exportPackageEntries(
    const QVector<VideoEntry>& entries, const QString& suggestedPath)
{
    if (packageWriteWatcher_ != nullptr && packageWriteWatcher_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("Dışa aktarma sürüyor"),
                                 QStringLiteral("Mevcut paket işlemi tamamlanana kadar bekleyin."));
        return;
    }
    const QString destination = QFileDialog::getSaveFileName(
        this, QStringLiteral("SafeTensors paketini kaydet"), suggestedPath,
        QStringLiteral("SafeTensors paketi (*.safetensors)"));
    if (destination.isEmpty()) {
        return;
    }
    QString outputPath = destination;
    if (!outputPath.endsWith(QStringLiteral(".safetensors"), Qt::CaseInsensitive)) {
        outputPath += QStringLiteral(".safetensors");
    }
    const bool embedMedia = true;
    QString signingAuthor;
    startPackageExport(entries, outputPath, &entries == &entries_, embedMedia,
                       signingAuthor);
}

void PackManagerWindow::startPackageExport(
    const QVector<VideoEntry>& entries, const QString& outputPath,
    bool updateCurrentPackage, bool embedMedia, const QString& signingAuthor)
{
    if (activeProgress_ != nullptr) {
        QMessageBox::information(this, QStringLiteral("İşlem sürüyor"),
                                 QStringLiteral("Mevcut arka plan işlemi bitince dışa aktarabilirsiniz."));
        return;
    }
    if (packageWriteWatcher_ != nullptr && packageWriteWatcher_->isRunning()) {
        return;
    }
    exportUpdatesCurrentPackage_ = updateCurrentPackage;
    PackageMetadata metadata = packageMetadata_;
    if (metadata.title.trimmed().isEmpty()) {
        metadata.title = QFileInfo(outputPath).completeBaseName();
    }
    if (metadata.createdAt.trimmed().isEmpty()) {
        metadata.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    }
    activeProgress_ = new QProgressDialog(
        QStringLiteral("SafeTensors paketi yazılıyor..."), QString(), 0, 1000, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowTitle(QStringLiteral("Dışa aktarma"));
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();

    auto* watcher = new QFutureWatcher<QString>(this);
    packageWriteWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher] {
        const bool updatesCurrentPackage = exportUpdatesCurrentPackage_;
        if (activeProgress_ != nullptr) {
            activeProgress_->setValue(1000);
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            const QString path = watcher->result();
            if (updatesCurrentPackage) {
                readSafeTensors(path, nullptr, &packageMetadata_);
                packagePath_ = QFileInfo(path).absoluteFilePath();
                dirty_ = false;
                recoveryTimer_->stop();
                QFile::remove(appDataProjectPath());
                setWindowTitle(QStringLiteral("PackManager — %1")
                                   .arg(QFileInfo(packagePath_).fileName()));
            }
            updateSummary();
            QMessageBox::information(
                this, QStringLiteral("Tamamlandı"),
                QStringLiteral("Paket oluşturuldu:\n%1").arg(path));
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Paket oluşturulamadı"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (packageWriteWatcher_ == watcher) {
            packageWriteWatcher_ = nullptr;
        }
    });
    const QVector<VideoEntry> exportEntries = entries;
    PackageWriteOptions writeOptions;
    writeOptions.embedMedia = embedMedia;
    writeOptions.signingAuthor = signingAuthor;
    packageWriteWatcher_->setFuture(QtConcurrent::run(
        [this, exportEntries, metadata, writeOptions, outputPath] {
            createPackageBackup(outputPath);
            writeSafeTensors(
                exportEntries, outputPath, metadata,
                [this](quint64 completed, quint64 total) {
                    const int value = total == 0 ? 1000 : static_cast<int>(
                        std::min(1000.0, static_cast<double>(completed) * 1000.0
                                                  / static_cast<double>(total)));
                    QMetaObject::invokeMethod(this, [this, value, completed, total] {
                        if (activeProgress_ != nullptr) {
                            activeProgress_->setValue(value);
                            activeProgress_->setLabelText(
                                QStringLiteral("Paket yazılıyor: %1 / %2")
                                    .arg(humanSize(completed), humanSize(total)));
                        }
                    }, Qt::QueuedConnection);
                }, writeOptions);
            return outputPath;
        }));
}

void PackManagerWindow::extractSelectedEntries()
{
    if (activeProgress_ != nullptr) {
        QMessageBox::information(this, QStringLiteral("İşlem sürüyor"),
                                 QStringLiteral("Mevcut arka plan işlemi bitince içerik çıkarabilirsiniz."));
        return;
    }
    if (formDirty_ && !saveCurrentEntry()) {
        return;
    }
    QVector<VideoEntry> selected;
    const QList<QListWidgetItem*> selectedItems = videoList_->selectedItems();
    if (selectedItems.isEmpty()) {
        selected = entries_;
    } else {
        QSet<QString> ids;
        for (QListWidgetItem* item : selectedItems) {
            ids.insert(item->data(Qt::UserRole).toString());
        }
        for (const VideoEntry& entry : entries_) {
            if (ids.contains(entry.id)) {
                selected.push_back(entry);
            }
        }
    }
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Çıkarılacak içerik yok"),
                                 QStringLiteral("Pakette çıkarılacak video bulunamadı."));
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Videoların çıkarılacağı klasör"));
    if (directory.isEmpty()) {
        return;
    }
    struct ExtractionTask {
        VideoEntry entry;
        QString destination;
        bool cover = false;
        bool subtitle = false;
    };
    QVector<ExtractionTask> tasks;
    QStringList videoDestinations;
    QStringList coverDestinations;
    QStringList subtitleDestinations;
    QSet<QString> usedNames;
    const auto uniqueDestination = [&directory, &usedNames](QString filename) {
        filename.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")),
                         QStringLiteral("_"));
        if (filename.isEmpty() || filename == QStringLiteral(".")
            || filename == QStringLiteral("..")) {
            filename = QStringLiteral("media.bin");
        }
        const QString base = QFileInfo(filename).completeBaseName();
        const QString suffix = QFileInfo(filename).suffix();
        QString unique = filename;
        int duplicate = 2;
        while (usedNames.contains(unique.toCaseFolded())) {
            unique = QStringLiteral("%1_%2%3%4")
                .arg(base).arg(duplicate++)
                .arg(suffix.isEmpty() ? QString() : QStringLiteral("."))
                .arg(suffix);
        }
        usedNames.insert(unique.toCaseFolded());
        return QDir(directory).filePath(unique);
    };
    for (int index = 0; index < selected.size(); ++index) {
        QString filename = QFileInfo(selected[index].videoPath).fileName();
        if (filename.isEmpty()) {
            filename = QStringLiteral("%1.mp4").arg(selected[index].title);
        }
        const QString videoDestination = uniqueDestination(filename);
        videoDestinations.push_back(QFileInfo(videoDestination).fileName());
        tasks.push_back({selected[index], videoDestination, false});
        if (selected[index].coverIsEmbedded || !selected[index].coverPath.isEmpty()) {
            QString coverName = QFileInfo(selected[index].coverPath).fileName();
            if (coverName.isEmpty()) {
                coverName = QFileInfo(filename).completeBaseName()
                    + QStringLiteral("_cover.jpg");
            }
            const QString coverDestination = uniqueDestination(coverName);
            coverDestinations.push_back(QFileInfo(coverDestination).fileName());
            tasks.push_back({selected[index], coverDestination, true});
        } else {
            coverDestinations.push_back(QString());
        }
        if (selected[index].subtitleIsEmbedded
            || !selected[index].subtitlePath.isEmpty()) {
            QString subtitleName = QFileInfo(selected[index].subtitlePath).fileName();
            if (subtitleName.isEmpty()) {
                subtitleName = QFileInfo(filename).completeBaseName()
                    + QStringLiteral(".srt");
            }
            const QString subtitleDestination = uniqueDestination(subtitleName);
            subtitleDestinations.push_back(QFileInfo(subtitleDestination).fileName());
            tasks.push_back({selected[index], subtitleDestination, false, true});
        } else {
            subtitleDestinations.push_back(QString());
        }
    }
    const QString manifestPath = QDir(directory).filePath(QStringLiteral("manifest.json"));
    QStringList existing;
    for (const ExtractionTask& task : tasks) {
        const QString& path = task.destination;
        if (QFileInfo::exists(path)) {
            existing.push_back(QFileInfo(path).fileName());
        }
    }
    if (QFileInfo::exists(manifestPath)) {
        existing.push_back(QFileInfo(manifestPath).fileName());
    }
    if (!existing.isEmpty()
        && QMessageBox::question(
               this, QStringLiteral("Dosyaların üzerine yazılsın mı?"),
               QStringLiteral("Bu dosyalar zaten var ve değiştirilecek:\n%1")
                   .arg(existing.join(QLatin1Char('\n'))))
            != QMessageBox::Yes) {
        return;
    }
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Medya dosyaları çıkarılıyor..."), QString(), 0,
        tasks.size() + 1, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    auto* watcher = new QFutureWatcher<QStringList>(this);
    extractionWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher] {
        if (activeProgress_ != nullptr) {
            activeProgress_->setValue(activeProgress_->maximum());
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            const QStringList paths = watcher->result();
            QMessageBox::information(
                this, QStringLiteral("Çıkarma tamamlandı"),
                QStringLiteral("%1 video klasöre çıkarıldı.")
                    .arg(paths.size()));
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Medya çıkarılamadı"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (extractionWatcher_ == watcher) {
            extractionWatcher_ = nullptr;
        }
    });
    const QVector<VideoEntry> entries = selected;
    const PackageMetadata metadata = packageMetadata_;
    watcher->setFuture(QtConcurrent::run(
        [this, entries, tasks, videoDestinations, coverDestinations,
         subtitleDestinations,
         manifestPath, metadata] {
            QStringList outputPaths;
            for (int index = 0; index < tasks.size(); ++index) {
                const ExtractionTask& task = tasks.at(index);
                if (task.cover) {
                    extractCover(task.entry, task.destination);
                } else if (task.subtitle) {
                    extractSubtitle(task.entry, task.destination);
                } else {
                    extractVideo(task.entry, task.destination);
                }
                outputPaths.push_back(task.destination);
                QMetaObject::invokeMethod(this, [this, index] {
                    if (activeProgress_ != nullptr) {
                        activeProgress_->setValue(index + 1);
                    }
                }, Qt::QueuedConnection);
            }
            QJsonArray videos;
            for (int index = 0; index < entries.size(); ++index) {
                const VideoEntry& entry = entries.at(index);
                videos.append(QJsonObject{
                    {QStringLiteral("id"), entry.id},
                    {QStringLiteral("title"), entry.title},
                    {QStringLiteral("description"), entry.description},
                    {QStringLiteral("category"), entry.category},
                    {QStringLiteral("video_file"), videoDestinations.at(index)},
                    {QStringLiteral("cover_file"), coverDestinations.at(index)},
                    {QStringLiteral("subtitle_file"), subtitleDestinations.at(index)},
                    {QStringLiteral("tags"), QJsonArray::fromStringList(entry.tags)},
                    {QStringLiteral("collection"), entry.collection}
                });
            }
            const QJsonObject manifest{
                {QStringLiteral("format"), QStringLiteral("packmanager.extracted")},
                {QStringLiteral("package"), QJsonObject{
                     {QStringLiteral("title"), metadata.title},
                     {QStringLiteral("description"), metadata.description},
                     {QStringLiteral("version"), metadata.version},
                     {QStringLiteral("created_at"), metadata.createdAt}
                 }},
                {QStringLiteral("videos"), videos}
            };
            QSaveFile output(manifestPath);
            if (!output.open(QIODevice::WriteOnly)) {
                throw std::runtime_error(output.errorString().toStdString());
            }
            const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
            if (output.write(bytes) != bytes.size() || !output.commit()) {
                throw std::runtime_error(output.errorString().toStdString());
            }
            outputPaths.push_back(manifestPath);
            QMetaObject::invokeMethod(this, [this, taskCount = tasks.size()] {
                if (activeProgress_ != nullptr) {
                    activeProgress_->setValue(taskCount + 1);
                }
            }, Qt::QueuedConnection);
            return outputPaths;
        }));
}

void PackManagerWindow::updateListRow(int index)
{
    if (index < 0 || index >= entries_.size() || index >= videoList_->count()) {
        return;
    }
    VideoEntry& entry = entries_[index];
    QListWidgetItem* item = videoList_->item(index);
    entryId(entry);
    item->setData(Qt::UserRole, entry.id);

    const bool videoMissing = entry.videoIsEmbedded
        ? !QFileInfo::exists(entry.assetContainerPath)
        : !QFileInfo(entry.videoPath).isFile();
    const bool coverMissing = !entry.coverPath.isEmpty()
        && (entry.coverIsEmbedded
            ? !QFileInfo::exists(entry.assetContainerPath)
            : !QFileInfo(entry.coverPath).isFile());
    QString displayTitle = entry.title;
    if (videoMissing) {
        displayTitle = QStringLiteral("⚠ %1 (video kaynağı eksik)").arg(entry.title);
    }
    item->setText(QStringLiteral("%1  [%2] · %3 · %4")
                      .arg(displayTitle, entry.category,
                           humanSize(entryMediaSize(entry)),
                           durationText(entry.durationMs)));
    QStringList tooltipLines{
        QStringLiteral("%1 · %2").arg(entry.title, entry.category),
        QStringLiteral("Video: %1").arg(entry.videoPath),
        QStringLiteral("Boyut: %1").arg(humanSize(entryMediaSize(entry))),
        QStringLiteral("Süre: %1").arg(durationText(entry.durationMs))
    };
    if (!entry.coverPath.isEmpty()) {
        tooltipLines.push_back(QStringLiteral("Kapak: %1").arg(entry.coverPath));
        if (coverMissing) {
            tooltipLines.push_back(QStringLiteral("Kapak kaynağı bulunamadı."));
        }
    }
    tooltipLines.push_back(entry.description);
    item->setToolTip(tooltipLines.join(QLatin1Char('\n')));

    item->setIcon(QIcon());
    const quint64 coverByteSize = entry.coverIsEmbedded
        ? entry.coverSize
        : static_cast<quint64>(std::max<qint64>(0, QFileInfo(entry.coverPath).size()));
    if (!entry.coverPath.isEmpty() && !coverMissing
        && coverByteSize <= 16ULL * 1024ULL * 1024ULL) {
        try {
            QBuffer buffer;
            buffer.setData(readVideoCover(entry));
            if (buffer.open(QIODevice::ReadOnly)) {
                QImageReader reader(&buffer);
                reader.setAutoTransform(true);
                const QSize imageSize = reader.size();
                if (!imageSize.isEmpty() && imageSize.width() <= 8192
                    && imageSize.height() <= 8192) {
                    const QSize thumbnailSize = listViewMode_->currentIndex() == 0
                        ? QSize(64, 48) : QSize(160, 100);
                    reader.setScaledSize(imageSize.scaled(
                        thumbnailSize, Qt::KeepAspectRatio));
                    const QImage image = reader.read();
                    if (!image.isNull()) {
                        item->setIcon(QIcon(QPixmap::fromImage(image)));
                    }
                }
            }
        } catch (const std::exception&) {
            item->setToolTip(item->toolTip() + QStringLiteral("\nKapak küçük resmi oluşturulamadı."));
        }
    }
}

void PackManagerWindow::updateSummary()
{
    quint64 totalSize = 0;
    QMap<QString, int> categories;
    int missingFiles = 0;
    for (const VideoEntry& entry : entries_) {
        const quint64 size = entryMediaSize(entry);
        if (size <= std::numeric_limits<quint64>::max() - totalSize) {
            totalSize += size;
        }
        ++categories[entry.category];
        const bool videoMissing = entry.videoIsEmbedded
            ? !QFileInfo::exists(entry.assetContainerPath)
            : !QFileInfo(entry.videoPath).isFile();
        if (videoMissing) {
            ++missingFiles;
        }
        if (!entry.coverPath.isEmpty()) {
            const bool coverMissing = entry.coverIsEmbedded
                ? !QFileInfo::exists(entry.assetContainerPath)
                : !QFileInfo(entry.coverPath).isFile();
            if (coverMissing) {
                ++missingFiles;
            }
        }
    }
    QStringList categorySummary;
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        categorySummary.push_back(QStringLiteral("%1: %2").arg(it.key()).arg(it.value()));
    }
    QString text = QStringLiteral("%1 video · %2 · Kategoriler: %3")
        .arg(entries_.size())
        .arg(humanSize(totalSize), categorySummary.isEmpty()
                 ? QStringLiteral("—")
                 : categorySummary.join(QStringLiteral(", ")));
    if (!packageMetadata_.title.trimmed().isEmpty()) {
        text.prepend(QStringLiteral("%1 · v%2 · ")
                         .arg(packageMetadata_.title, packageMetadata_.version));
    }
    if (missingFiles > 0) {
        text += QStringLiteral(" · %1 medya kaynağı eksik").arg(missingFiles);
    }
    if (hasUnsavedChanges()) {
        text += QStringLiteral(" · Kaydedilmemiş değişiklikler");
    }
    summaryLabel_->setText(text);
}

void PackManagerWindow::playSelectedVideo()
{
    if (activeProgress_ != nullptr) {
        return;
    }
    const int row = videoList_->currentRow();
    if (row < 0 || row >= entries_.size()) {
        QMessageBox::information(this, QStringLiteral("Video seçilmedi"),
                                 QStringLiteral("Önce oynatılacak videoyu seçin."));
        return;
    }
    const VideoEntry& entry = entries_.at(row);
    lastVideoFrame_ = QImage();
    if (!entry.videoIsEmbedded) {
        mediaPlayer_->setSource(QUrl::fromLocalFile(entry.videoPath));
        mediaPlayer_->play();
        return;
    }
    if (!playbackTempDir_.isValid()) {
        QMessageBox::critical(this, QStringLiteral("Oynatma başlatılamadı"),
                              QStringLiteral("Geçici video klasörü oluşturulamadı."));
        return;
    }

    QString suffix = QFileInfo(entry.videoPath).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > 8
        || !std::all_of(suffix.cbegin(), suffix.cend(),
                        [](QChar character) { return character.isLetterOrNumber(); })) {
        suffix = QStringLiteral("mp4");
    }
    const QString destination = QDir(playbackTempDir_.path()).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QLatin1Char('.') + suffix);
    mediaPlayer_->stop();
    mediaPlayer_->setSource(QUrl());
    activeProgress_ = new QProgressDialog(
        QStringLiteral("Gömülü video oynatma için hazırlanıyor..."), QString(),
        0, 1000, this);
    activeProgress_->setCancelButton(nullptr);
    activeProgress_->setWindowModality(Qt::WindowModal);
    activeProgress_->setMinimumDuration(0);
    activeProgress_->show();
    const VideoEntry selectedEntry = entry;
    auto* watcher = new QFutureWatcher<QString>(this);
    playbackExtractionWatcher_ = watcher;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        if (activeProgress_ != nullptr) {
            activeProgress_->setValue(1000);
            activeProgress_->deleteLater();
            activeProgress_ = nullptr;
        }
        try {
            playbackPath_ = watcher->result();
            mediaPlayer_->setSource(QUrl::fromLocalFile(playbackPath_));
            mediaPlayer_->play();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, QStringLiteral("Video açılamadı"),
                                  QString::fromUtf8(error.what()));
        }
        watcher->deleteLater();
        if (playbackExtractionWatcher_ == watcher) {
            playbackExtractionWatcher_ = nullptr;
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [this, selectedEntry, destination] {
            extractVideo(selectedEntry, destination,
                         [this](quint64 completed, quint64 total) {
                const int value = total == 0 ? 1000 : static_cast<int>(
                    std::min(1000.0, static_cast<double>(completed) * 1000.0
                                              / static_cast<double>(total)));
                QMetaObject::invokeMethod(this, [this, value, completed, total] {
                    if (activeProgress_ != nullptr) {
                        activeProgress_->setValue(value);
                        activeProgress_->setLabelText(
                            QStringLiteral("Geçici video hazırlanıyor: %1 / %2")
                                .arg(humanSize(completed), humanSize(total)));
                    }
                }, Qt::QueuedConnection);
            });
            return destination;
        }));
}

void PackManagerWindow::togglePlayback()
{
    if (mediaPlayer_->playbackState() == QMediaPlayer::PlayingState) {
        mediaPlayer_->pause();
        playbackButton_->setText(QStringLiteral("Devam et"));
        return;
    }
    if (mediaPlayer_->source().isEmpty()) {
        playSelectedVideo();
        return;
    }
    mediaPlayer_->play();
    playbackButton_->setText(QStringLiteral("Duraklat"));
}

void PackManagerWindow::updatePlaybackPosition(qint64 position)
{
    const QSignalBlocker blocker(playbackSlider_);
    playbackSlider_->setValue(static_cast<int>(
        std::min<qint64>(position, std::numeric_limits<int>::max())));
}

void PackManagerWindow::updatePlaybackDuration(qint64 duration)
{
    playbackSlider_->setRange(
        0, static_cast<int>(std::min<qint64>(duration, std::numeric_limits<int>::max())));
    const int row = videoList_->currentRow();
    if (duration > 0 && row >= 0 && row < entries_.size()
        && entries_[row].durationMs != duration) {
        recordUndoState();
        entries_[row].durationMs = duration;
        markDirty();
        updateListRow(row);
        playbackStatus_->setText(
            QStringLiteral("%1 · %2 · %3")
                .arg(entries_[row].title, entries_[row].category,
                     durationText(duration)));
    }
}

void PackManagerWindow::seekVideo(int position)
{
    mediaPlayer_->setPosition(position);
}

void PackManagerWindow::showPlayerError()
{
    if (mediaPlayer_->error() == QMediaPlayer::NoError) {
        return;
    }
    playbackStatus_->setText(QStringLiteral("Video oynatılamadı"));
    QMessageBox::warning(this, QStringLiteral("Video oynatılamadı"),
                         mediaPlayer_->errorString());
}

void PackManagerWindow::updateCoverPreview(const VideoEntry& entry)
{
    coverPreview_->setPixmap(QPixmap());
    coverPreview_->setText(QStringLiteral("Kapak yok"));
    if (entry.coverPath.isEmpty()) {
        return;
    }
    try {
        const QByteArray imageBytes = readVideoCover(entry);
        QBuffer buffer;
        buffer.setData(imageBytes);
        if (!buffer.open(QIODevice::ReadOnly)) {
            playbackStatus_->setText(QStringLiteral("Kapak resmi açılamadı"));
            return;
        }
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize imageSize = reader.size();
        if (imageSize.isEmpty() || imageSize.width() > 8192
            || imageSize.height() > 8192) {
            coverPreview_->setText(QStringLiteral("Kapak boyutu desteklenmiyor"));
            return;
        }
        reader.setScaledSize(imageSize.scaled(QSize(520, 360), Qt::KeepAspectRatio));
        const QImage image = reader.read();
        if (image.isNull()) {
            coverPreview_->setText(QStringLiteral("Kapak resmi okunamadı"));
            return;
        }
        coverPreview_->setPixmap(QPixmap::fromImage(image).scaled(
            coverPreview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        coverPreview_->setText(QString());
    } catch (const std::exception& error) {
        coverPreview_->setText(QStringLiteral("Kapak resmi okunamadı"));
        playbackStatus_->setText(QString::fromUtf8(error.what()));
    }
}

void PackManagerWindow::refreshList()
{
    const QString selectedCategory = categoryFilter_->currentIndex() > 0
        ? categoryFilter_->currentText() : QString();
    {
        const QSignalBlocker blocker(categoryFilter_);
        categoryFilter_->clear();
        categoryFilter_->addItem(language_ == QStringLiteral("en")
                                     ? QStringLiteral("All categories")
                                     : QStringLiteral("Tüm kategoriler"));
        QSet<QString> categories;
        for (const VideoEntry& entry : entries_) {
            if (!entry.category.trimmed().isEmpty()) {
                categories.insert(entry.category);
            }
        }
        QStringList sortedCategories = categories.values();
        std::sort(sortedCategories.begin(), sortedCategories.end(),
                  [](const QString& left, const QString& right) {
                      return left.compare(right, Qt::CaseInsensitive) < 0;
                  });
        categoryFilter_->addItems(sortedCategories);
        const int categoryIndex = categoryFilter_->findText(selectedCategory);
        categoryFilter_->setCurrentIndex(categoryIndex > 0 ? categoryIndex : 0);
    }
    const QSignalBlocker blocker(videoList_);
    videoList_->clear();
    for (int index = 0; index < entries_.size(); ++index) {
        videoList_->addItem(QString());
        updateListRow(index);
    }
    videoList_->setCurrentRow(-1);
    applyFilters();
    updateSummary();
}
