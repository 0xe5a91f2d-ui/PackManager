#pragma once

#include <QListWidget>
#include <QStringList>

class QDragEnterEvent;
class QDropEvent;

class VideoListWidget final : public QListWidget {
    Q_OBJECT

public:
    explicit VideoListWidget(QWidget* parent = nullptr);

signals:
    void filesDropped(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};
