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

quint16 readBe16(const QByteArray& data, int offset) {
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return (quint16(p[0]) << 8) | quint16(p[1]);
}

bool isPrintableAscii(unsigned char c) {
    return c >= 0x20 && c <= 0x7e;
}

QString readCStringAtAddress(const QByteArray& rom, quint32 addr, quint32 baseAddr) {
    if (addr < baseAddr) return {};
    const int start = int(addr - baseAddr);
    if (start < 0 || start >= rom.size()) return {};

    QByteArray out;
    for (int i = start; i < rom.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(rom[i]);
        if (c == 0) break;
        if (!isPrintableAscii(c)) return {};
        out.append(char(c));
        if (out.size() > 128) break;
    }
    if (out.isEmpty()) return {};
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

quint32 detectBaseAddressBySize(int sizeBytes) {
    const quint32 top = 0x01000000;
    return top - quint32(sizeBytes);
}

quint32 normalizeAddress(quint32 addr, quint32 baseAddr, int romSize) {
    const quint32 end = baseAddr + quint32(romSize);
    if (addr >= baseAddr && addr < end) return addr;

    // Some dumps carry upper bits outside 24-bit address space.
    const quint32 masked24 = addr & 0x00FFFFFFu;
    if (masked24 >= baseAddr && masked24 < end) return masked24;

    return 0;
}

QVector<quint32> baseCandidates(const QByteArray& rom) {
    QVector<quint32> out;
    out.push_back(detectBaseAddressBySize(rom.size()));

    // Heuristic from initial PC vector.
    if (rom.size() >= 8) {
        quint32 pc = readBe32(rom, 4) & 0x00FFFFFFu;
        int size = rom.size();
        if (size > 0 && (size & (size - 1)) == 0) {
            quint32 mask = quint32(size - 1);
            quint32 byPc = pc & ~mask;
            if (byPc < 0x01000000u) out.push_back(byPc);
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

QVector<ComponentInfo> scanComponentsWithBase(const QByteArray& rom, quint32 baseAddr) {
    QVector<ComponentInfo> out;

    for (int i = 0; i + 26 <= rom.size(); ++i) {
        if (readBe16(rom, i) != 0x4AFC) continue;

        const quint32 matchTagRaw = readBe32(rom, i + 2);
        const quint32 namePtrRaw = readBe32(rom, i + 14);

        const quint32 matchTag = normalizeAddress(matchTagRaw, baseAddr, rom.size());
        const quint32 namePtr = normalizeAddress(namePtrRaw, baseAddr, rom.size());
        if (!matchTag || !namePtr) continue;

        const int expected = int(matchTag - baseAddr);
        if (std::abs(expected - i) > 32) continue;

        const QString name = readCStringAtAddress(rom, namePtr, baseAddr);
        if (name.isEmpty()) continue;

        ComponentInfo c;
        c.name = name;
        c.offset = i;
        c.size = 0; // assigned after sort
        out.push_back(std::move(c));
    }

    std::sort(out.begin(), out.end(), [](const ComponentInfo& a, const ComponentInfo& b) {
        return a.offset < b.offset;
    });

    // Deduplicate by offset
    QVector<ComponentInfo> dedup;
    for (const auto& c : out) {
        if (!dedup.isEmpty() && dedup.back().offset == c.offset) continue;
        dedup.push_back(c);
    }

    for (int idx = 0; idx < dedup.size(); ++idx) {
        const int start = dedup[idx].offset;
        const int end = (idx + 1 < dedup.size()) ? dedup[idx + 1].offset : rom.size();
        if (end <= start) {
            dedup[idx].size = 0;
            dedup[idx].data.clear();
            dedup[idx].checksumSha256.clear();
            continue;
        }
        dedup[idx].size = end - start;
        dedup[idx].data = rom.mid(start, dedup[idx].size);
        dedup[idx].checksumSha256 = QCryptographicHash::hash(dedup[idx].data, QCryptographicHash::Sha256);
    }

    QVector<ComponentInfo> finalOut;

    // Preserve bytes before the first detected RomTag as dedicated header block.
    // Without this prelude, reassembled ROMs may miss vectors/startup header.
    if (!dedup.isEmpty() && dedup[0].offset > 0) {
        ComponentInfo header;
        header.name = "__rom_header";
        header.offset = 0;
        header.size = dedup[0].offset;
        header.data = rom.left(header.size);
        header.checksumSha256 = QCryptographicHash::hash(header.data, QCryptographicHash::Sha256);
        finalOut.push_back(std::move(header));
    }

    for (auto& c : dedup) {
        if (c.size > 0) finalOut.push_back(std::move(c));
    }
    return finalOut;
}

QVector<ComponentInfo> bestScan(const QByteArray& rom) {
    QVector<ComponentInfo> best;
    const auto bases = baseCandidates(rom);
    for (quint32 base : bases) {
        auto curr = scanComponentsWithBase(rom, base);
        if (curr.size() > best.size()) best = std::move(curr);
    }
    return best;
}



bool hasValidKickChecksum(const QByteArray& rom) {
    if (rom.size() < 4 || (rom.size() % 4) != 0) return false;

    quint64 sum = 0;
    for (int off = 0; off < rom.size(); off += 4) {
        sum += readBe32(rom, off);
    }
    return (quint32(sum & 0xFFFFFFFFu) == 0xFFFFFFFFu);
}

void separateTrailingChecksum(QVector<ComponentInfo>& comps, const QByteArray& rom, QStringList* warnings) {
    if (comps.isEmpty() || rom.size() < 4 || !hasValidKickChecksum(rom)) return;

    const int checksumOff = rom.size() - 4;

    int lastIdx = -1;
    for (int i = comps.size() - 1; i >= 0; --i) {
        if (comps[i].name == "__rom_header") continue;
        lastIdx = i;
        break;
    }
    if (lastIdx < 0) return;

    auto& last = comps[lastIdx];
    const int lastEnd = last.offset + last.size;
    if (lastEnd != rom.size() || last.size < 4) return;

    // Detach trailing checksum longword from payload component.
    last.size -= 4;
    last.data = rom.mid(last.offset, last.size);
    last.checksumSha256 = QCryptographicHash::hash(last.data, QCryptographicHash::Sha256);

    ComponentInfo checksum;
    checksum.name = "__rom_checksum";
    checksum.offset = checksumOff;
    checksum.size = 4;
    checksum.data = rom.mid(checksumOff, 4);
    checksum.checksumSha256 = QCryptographicHash::hash(checksum.data, QCryptographicHash::Sha256);
    comps.push_back(std::move(checksum));

    if (warnings) warnings->push_back("Separated trailing ROM checksum longword into __rom_checksum metadata component.");
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

    out = bestScan(canonicalRom);

    // Fallback: if input endian assumption is wrong, scanning swapped view may recover components.
    if (out.isEmpty()) {
        const QByteArray swapped = swap16(canonicalRom);
        auto fallback = bestScan(swapped);
        if (!fallback.isEmpty()) {
            if (warnings) warnings->push_back("RomTag scan only matched after word-swap fallback.");

            // Important: always export canonical byte order component payloads.
            for (auto& c : fallback) {
                c.data = canonicalRom.mid(c.offset, c.size);
                c.checksumSha256 = QCryptographicHash::hash(c.data, QCryptographicHash::Sha256);
            }
            out = std::move(fallback);
        }
    }

    if (warnings && out.isEmpty()) {
        warnings->push_back("No ROM components detected via RomTag scan (Romsplit-compatible tagging not found). It may be a plain monolithic image without resident tags.");
    }

    if (!out.isEmpty()) {
        separateTrailingChecksum(out, canonicalRom, warnings);
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
