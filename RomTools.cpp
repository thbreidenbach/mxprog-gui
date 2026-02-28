#include "RomTools.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace RomTools {

QByteArray swap16(const QByteArray& in) {
    QByteArray out = in;
    if (out.size() % 2) out.append(char(0xff));
    for (int i = 0; i + 1 < out.size(); i += 2) {
        std::swap(out[i], out[i + 1]);
    }
    return out;
}

QByteArray toHex(const QByteArray& bytes) {
    return bytes.toHex();
}

RomMeta inspectRom(const QString& path) {
    RomMeta meta;
    meta.sourcePath = path;

    QFileInfo fi(path);
    meta.fileName = fi.fileName();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        meta.warnings << "Failed to open file.";
        return meta;
    }

    QByteArray raw = f.readAll();
    f.close();

    meta.originalSize = raw.size();
    meta.validSize = (raw.size() > 0 && raw.size() <= TOTAL_BYTES);
    if (!meta.validSize) {
        meta.warnings << QString("Unsupported ROM size: %1 bytes").arg(raw.size());
        return meta;
    }

    meta.alreadyByteswapped = fi.suffix().compare("rom", Qt::CaseInsensitive) == 0;
    meta.canonicalData = meta.alreadyByteswapped ? swap16(raw) : raw;

    if (meta.canonicalData.size() < TOTAL_BYTES) {
        meta.warnings << QString("Input smaller than 2 MiB (%1 bytes), padded with 0xFF to 2 MiB.")
                             .arg(meta.canonicalData.size());
    }

    meta.padded2MiB = meta.canonicalData.left(TOTAL_BYTES);
    if (meta.padded2MiB.size() < TOTAL_BYTES) {
        meta.padded2MiB.append(QByteArray(TOTAL_BYTES - meta.padded2MiB.size(), char(0xff)));
    }

    meta.checksumSha256 = QCryptographicHash::hash(meta.padded2MiB, QCryptographicHash::Sha256);
    return meta;
}

QVector<SliceInfo> splitIntoBanks(const QByteArray& twoMiB) {
    QVector<SliceInfo> out;
    if (twoMiB.size() < TOTAL_BYTES) return out;

    for (int bank = 0; bank < 4; ++bank) {
        const int offset = bank * SLOT_SIZE;
        SliceInfo s;
        s.bank = bank;
        s.fileName = QString("bank_%1.bin").arg(bank);
        s.data = twoMiB.mid(offset, SLOT_SIZE);
        s.checksumSha256 = QCryptographicHash::hash(s.data, QCryptographicHash::Sha256);
        out.push_back(std::move(s));
    }
    return out;
}

bool writeCatalog(const QString& outDir, const RomMeta& meta, const QVector<SliceInfo>& slices, QString* error) {
    QDir dir;
    if (!dir.mkpath(outDir)) {
        if (error) *error = "Could not create output directory.";
        return false;
    }

    QFile full(QDir(outDir).filePath("rom_2mib.bin"));
    if (!full.open(QIODevice::WriteOnly)) {
        if (error) *error = "Could not write rom_2mib.bin.";
        return false;
    }
    full.write(meta.padded2MiB);
    full.close();

    for (const auto& s : slices) {
        QFile sliceFile(QDir(outDir).filePath(s.fileName));
        if (!sliceFile.open(QIODevice::WriteOnly)) {
            if (error) *error = QString("Could not write %1.").arg(s.fileName);
            return false;
        }
        sliceFile.write(s.data);
        sliceFile.close();
    }

    QJsonObject root;
    root["sourcePath"] = meta.sourcePath;
    root["sourceFileName"] = meta.fileName;
    root["originalSize"] = static_cast<qint64>(meta.originalSize);
    root["canonicalSize"] = meta.canonicalData.size();
    root["isRomByteSwappedInput"] = meta.alreadyByteswapped;
    root["sha256_2mib"] = QString::fromLatin1(toHex(meta.checksumSha256));

    QJsonArray warns;
    for (const auto& w : meta.warnings) warns.append(w);
    root["warnings"] = warns;

    QJsonArray banks;
    for (const auto& s : slices) {
        QJsonObject b;
        b["bank"] = s.bank;
        b["file"] = s.fileName;
        b["size"] = s.data.size();
        b["sha256"] = QString::fromLatin1(toHex(s.checksumSha256));
        banks.append(b);
    }
    root["banks"] = banks;

    QFile catalog(QDir(outDir).filePath("catalog.json"));
    if (!catalog.open(QIODevice::WriteOnly)) {
        if (error) *error = "Could not write catalog.json.";
        return false;
    }
    catalog.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    catalog.close();

    return true;
}

} // namespace RomTools
