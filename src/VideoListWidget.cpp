#include "VideoListWidget.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

VideoListWidget::VideoListWidget(QWidget* parent)
    : QListWidget(parent)
{
}

void VideoListWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QListWidget::dragEnterEvent(event);
}

void VideoListWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    QListWidget::dragMoveEvent(event);
}

void VideoListWidget::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        QListWidget::dropEvent(event);
        return;
    }

    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    emit filesDropped(paths);
    event->acceptProposedAction();
}
