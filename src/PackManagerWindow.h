#pragma once

#include "PackageFormat.h"

#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QStack>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>
#include <QFutureWatcher>
#include <QImage>

class QComboBox;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QAction;
class QActionGroup;
class QCheckBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QMediaPlayer;
class QProgressDialog;
class QPushButton;
class QSlider;
class QTextEdit;
class QTimer;
class QVideoSink;
class VideoListWidget;
class QVideoWidget;

struct PackageValidationResult {
    QString path;
    QVector<VideoEntry> entries;
    SafeTensorsValidationReport report;
    PackageMetadata metadata;
};

class PackManagerWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit PackManagerWindow(QWidget* parent = nullptr);
    ~PackManagerWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void chooseVideo();
    void chooseVideos();
    void chooseCover();
    void chooseSubtitle();
    void showSelectedSubtitle();
    void showVideoTechnicalInfo();
    void convertSelectedVideo();
    void removeCover();
    void openPackage();
    void openProjectDraft();
    void saveProjectDraft(bool saveAs = false);
    bool savePackage();
    bool confirmDiscardChanges(const QString& action);
    void createNewProject();
    void markDirty();
    void validatePackage();
    void importDroppedFiles(const QStringList& paths);
    void reorderEntries();
    void applyFilters();
    void changeSortOrder(int index);
    void bulkEditCategory();
    void bulkApplyCover();
    void deleteSelectedEntries();
    void undo();
    void redo();
    void copySelectedEntries();
    void pasteEntries();
    void duplicateSelectedEntries();
    void recordUndoState();
    void editCategories();
    void editSettings();
    void showPackageHistory();
    void checkForUpdates(bool silent = false);
    void captureVideoFrame();
    void extractSelectedEntries();
    void selectEntriesForExport();
    void exportPackageEntries(const QVector<VideoEntry>& entries,
                              const QString& suggestedPath);
    void startPackageExport(const QVector<VideoEntry>& entries,
                            const QString& outputPath, bool updateCurrentPackage,
                            bool embedMedia, const QString& signingAuthor = {});
    void editPackageMetadata();
    void changeLanguage(const QString& language);
    void changeTheme(const QString& theme);
    void applyCurrentTheme();
    void updateLocalizedUi();
    void findMissingMedia();
    bool hasUnsavedChanges() const;
    void markFormDirty();
    bool saveCurrentEntry();
    void scheduleRecoverySave();
    void saveRecoveryDraft();
    void restoreRecoveryDraft();
    bool saveCurrentWorkspace();
    void saveEntry();
    void loadSelectedEntry(int row);
    void removeSelectedEntry();
    void clearForm();
    bool exportPackage();
    void playSelectedVideo();
    void togglePlayback();
    void updatePlaybackPosition(qint64 position);
    void updatePlaybackDuration(qint64 duration);
    void seekVideo(int position);
    void showPlayerError();
    void updateCoverPreview(const VideoEntry& entry);
    void updateEditorState();
    void updateListRow(int index);
    void updateSummary();
    void refreshList();
    struct EditSnapshot {
        QVector<VideoEntry> entries;
        PackageMetadata metadata;
        QVector<QString> manualOrder;
    };

    QVector<VideoEntry> entries_;
    QVector<QString> manualOrder_;
    PackageMetadata packageMetadata_;
    QString packageName_;
    QVector<EditSnapshot> undoHistory_;
    QVector<EditSnapshot> redoHistory_;
    QVector<VideoEntry> entryClipboard_;
    QTemporaryDir playbackTempDir_;
    QString playbackPath_;
    QString packagePath_;
    QString projectPath_;
    int editingIndex_ = -1;
    QLineEdit* videoPath_ = nullptr;
    QLineEdit* coverPath_ = nullptr;
    QLineEdit* subtitlePath_ = nullptr;
    QLineEdit* title_ = nullptr;
    QLineEdit* tags_ = nullptr;
    QLineEdit* collection_ = nullptr;
    QComboBox* category_ = nullptr;
    QTextEdit* description_ = nullptr;
    VideoListWidget* videoList_ = nullptr;
    QLabel* coverPreview_ = nullptr;
    QLabel* playbackStatus_ = nullptr;
    QLabel* editStatus_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QMediaPlayer* mediaPlayer_ = nullptr;
    QTimer* recoveryTimer_ = nullptr;
    QVideoWidget* videoWidget_ = nullptr;
    QPushButton* playbackButton_ = nullptr;
    QPushButton* saveEntryButton_ = nullptr;
    QPushButton* savePackageButton_ = nullptr;
    QSlider* playbackSlider_ = nullptr;
    bool dirty_ = false;
    bool formDirty_ = false;
    bool loadingEntry_ = false;
    QString language_ = QStringLiteral("tr");
    QString theme_ = QStringLiteral("light");
    QLineEdit* searchField_ = nullptr;
    QComboBox* categoryFilter_ = nullptr;
    QComboBox* sortOrder_ = nullptr;
    QComboBox* secondarySortOrder_ = nullptr;
    QComboBox* listViewMode_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* darkThemeAction_ = nullptr;
    QAction* englishAction_ = nullptr;
    QPointer<QProgressDialog> activeProgress_;
    QFutureWatcher<QString>* packageWriteWatcher_ = nullptr;
    QFutureWatcher<QStringList>* extractionWatcher_ = nullptr;
    QFutureWatcher<QString>* playbackExtractionWatcher_ = nullptr;
    QFutureWatcher<PackageValidationResult>* validationWatcher_ = nullptr;
    QFutureWatcher<PackageValidationResult>* packageReadWatcher_ = nullptr;
    QFutureWatcher<QString>* videoToolWatcher_ = nullptr;
    bool exportUpdatesCurrentPackage_ = false;
    QVideoSink* videoSink_ = nullptr;
    QImage lastVideoFrame_;
    bool frameCapturePending_ = false;
    bool updateCheckRunning_ = false;
    QVector<VideoEntry> packageExportEntries_;
};
