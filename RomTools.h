#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace RomTools {

static constexpr int SLOT_SIZE = 512 * 1024;
static constexpr int TOTAL_BYTES = 4 * SLOT_SIZE;

struct RomMeta {
    QString sourcePath;
    QString fileName;
    qint64 originalSize = 0;
    QByteArray canonicalData;
    QByteArray padded2MiB;
    QByteArray checksumSha256;
    bool validSize = false;
    bool alreadyByteswapped = false;
    QStringList warnings;
};

struct SliceInfo {
    int bank = 0;
    QString fileName;
    QByteArray data;
    QByteArray checksumSha256;
};

QByteArray swap16(const QByteArray& in);
QByteArray toHex(const QByteArray& bytes);
RomMeta inspectRom(const QString& path);
QVector<SliceInfo> splitIntoBanks(const QByteArray& twoMiB);
bool writeCatalog(const QString& outDir, const RomMeta& meta, const QVector<SliceInfo>& slices, QString* error);

} // namespace RomTools
