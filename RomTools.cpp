#include "RomTools.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdlib>

namespace RomTools {

namespace {

quint32 readBe32(const QByteArray& data, int offset) {
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

QString readCStringAtAddress(const QByteArray& rom, quint32 addr, quint32 baseAddr) {
    if (addr < baseAddr) return {};
    const int start = int(addr - baseAddr);
    if (start < 0 || start >= rom.size()) return {};

    QByteArray out;
    for (int i = start; i < rom.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(rom[i]);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return {};
        out.append(char(c));
        if (out.size() > 96) break;
    }
    return QString::fromLatin1(out);
}

QString safeFileName(QString s) {
    if (s.isEmpty()) return "unnamed";
    s.replace(' ', '_');
    s.replace('/', '_');
    s.replace('\\', '_');
    s.replace(':', '_');
    s.replace('*', '_');
    s.replace('?', '_');
    s.replace('"', '_');
    s.replace('<', '_');
    s.replace('>', '_');
    s.replace('|', '_');
    return s;
}

quint32 detectBaseAddress(int sizeBytes) {
    // Typical Kickstart ROM mapping: top of 24-bit address space.
    // This heuristic allows 256/512/1024/2048 KiB dumps.
    const quint32 top = 0x01000000;
    return top - quint32(sizeBytes);
}

} // namespace

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

    if (meta.canonicalData.size() < SLOT_SIZE) {
        meta.warnings << QString("Input smaller than 512 KiB (%1 bytes), padded with 0xFF.")
                             .arg(meta.canonicalData.size());
    }
    if (meta.canonicalData.size() < TOTAL_BYTES) {
        meta.warnings << QString("Input smaller than 2 MiB (%1 bytes), padded with 0xFF to 2 MiB for bank handling.")
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

QVector<ComponentInfo> extractComponents(const QByteArray& canonicalRom, QStringList* warnings) {
    QVector<ComponentInfo> out;
    if (canonicalRom.isEmpty()) return out;

    const quint32 baseAddr = detectBaseAddress(canonicalRom.size());

    struct Candidate {
        int offset;
        QString name;
    };
    QVector<Candidate> tags;

    for (int i = 0; i + 26 <= canonicalRom.size(); ++i) {
        const unsigned char b0 = static_cast<unsigned char>(canonicalRom[i]);
        const unsigned char b1 = static_cast<unsigned char>(canonicalRom[i + 1]);
        if (b0 != 0x4a || b1 != 0xfc) continue;

        const quint32 matchTag = readBe32(canonicalRom, i + 2);
        const quint32 namePtr = readBe32(canonicalRom, i + 14);

        if (matchTag < baseAddr || matchTag >= baseAddr + quint32(canonicalRom.size())) continue;
        const int expect = int(matchTag - baseAddr);
        if (std::abs(expect - i) > 8) continue;

        const QString name = readCStringAtAddress(canonicalRom, namePtr, baseAddr);
        if (name.isEmpty()) continue;

        bool duplicate = false;
        for (const auto& t : tags) {
            if (t.offset == i) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) tags.push_back({i, name});
    }

    std::sort(tags.begin(), tags.end(), [](const Candidate& a, const Candidate& b) {
        return a.offset < b.offset;
    });

    for (int idx = 0; idx < tags.size(); ++idx) {
        const int start = tags[idx].offset;
        const int end = (idx + 1 < tags.size()) ? tags[idx + 1].offset : canonicalRom.size();
        if (end <= start) continue;

        ComponentInfo c;
        c.name = tags[idx].name;
        c.offset = start;
        c.size = end - start;
        c.data = canonicalRom.mid(start, c.size);
        c.checksumSha256 = QCryptographicHash::hash(c.data, QCryptographicHash::Sha256);
        out.push_back(std::move(c));
    }

    if (warnings && out.isEmpty()) {
        warnings->push_back("No ROM components detected via RomTag scan (Romsplit-compatible tagging not found).");
    }

    return out;
}

bool writeCatalog(const QString& outDir,
                  const RomMeta& meta,
                  const QVector<SliceInfo>& slices,
                  const QVector<ComponentInfo>& components,
                  QString* error) {
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

    const QString componentsDirPath = QDir(outDir).filePath("components");
    if (!dir.mkpath(componentsDirPath)) {
        if (error) *error = "Could not create components directory.";
        return false;
    }

    QJsonArray componentsJson;
    for (int i = 0; i < components.size(); ++i) {
        const auto& c = components[i];
        const QString fileName = QString("%1_%2.bin").arg(i, 3, 10, QChar('0')).arg(safeFileName(c.name));
        QFile compFile(QDir(componentsDirPath).filePath(fileName));
        if (!compFile.open(QIODevice::WriteOnly)) {
            if (error) *error = QString("Could not write component file %1.").arg(fileName);
            return false;
        }
        compFile.write(c.data);
        compFile.close();

        QJsonObject cj;
        cj["name"] = c.name;
        cj["offset"] = c.offset;
        cj["size"] = c.size;
        cj["file"] = QString("components/%1").arg(fileName);
        cj["sha256"] = QString::fromLatin1(toHex(c.checksumSha256));
        componentsJson.append(cj);
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
    root["components"] = componentsJson;

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
