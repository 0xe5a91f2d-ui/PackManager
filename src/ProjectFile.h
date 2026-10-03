#pragma once

#include "PackageFormat.h"

#include <QString>
#include <QVector>

QVector<VideoEntry> readProjectFile(const QString& path,
                                    PackageMetadata* metadata = nullptr);
void writeProjectFile(const QVector<VideoEntry>& entries, const QString& path,
                      const PackageMetadata& metadata = {});
