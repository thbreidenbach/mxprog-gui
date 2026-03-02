#include "BankWidget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>

MeterBar::MeterBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(20);
}

void MeterBar::setSegments(const QVector<int>& sizes) {
    m_sizes = sizes; update();
}
void MeterBar::setTotal(int total) {
    m_total = qMax(1, total); update();
}

void MeterBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), Qt::white);

    const int W = width();
    const int H = height();
    const int total = qMax(1, m_total);
    const int halfBytes = total / 2;
    const int xMid = W / 2;

    // Base fill: actually loaded bytes (linear from offset 0).
    int x = 0;
    int consumed = 0;
    for (int i = 0; i < m_sizes.size(); ++i) {
        const int segBytes = qBound(0, m_sizes[i], total - consumed);
        if (segBytes <= 0) continue;

        const int xNext = qBound(0, int((qint64(consumed + segBytes) * W) / total), W);
        const int w = qMax(1, xNext - x);
        const QColor color = (i % 2 == 0) ? QColor("#3b82f6") : QColor("#10b981");
        p.fillRect(QRect(x, 0, w, H), color);
        p.setPen(Qt::black);
        p.drawRect(QRect(x, 0, w, H));

        consumed += segBytes;
        x = xNext;
        if (consumed >= total) break;
    }

    // Middle marker at 256 KiB.
    p.setPen(QPen(QColor("#111827"), 2));
    p.drawLine(xMid, 0, xMid, H - 1);

    // Visual mode hints:
    // - <=256 KiB: mirrored area in upper half of bank is highlighted.
    // - >256 KiB: overflow area (256..used) is hatched as warning.
    if (consumed > 0 && consumed <= halfBytes) {
        const int mirrorEnd = qBound(xMid, int((qint64(halfBytes + consumed) * W) / total), W);
        const int mirrorW = qMax(0, mirrorEnd - xMid);
        if (mirrorW > 0) {
            QBrush mirrorBrush(QColor(59, 130, 246, 70), Qt::Dense6Pattern);
            p.fillRect(QRect(xMid, 0, mirrorW, H), mirrorBrush);
        }
    } else if (consumed > halfBytes) {
        const int overflowEnd = qBound(xMid, int((qint64(consumed) * W) / total), W);
        const int overflowW = qMax(0, overflowEnd - xMid);
        if (overflowW > 0) {
            QBrush warnBrush(QColor(220, 38, 38, 90), Qt::BDiagPattern);
            p.fillRect(QRect(xMid, 0, overflowW, H), warnBrush);
        }
    }

    p.setPen(Qt::black);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

// ---------------- BankWidget ----------------

BankWidget::BankWidget(int bankIndex, QWidget* parent)
    : QWidget(parent), m_bank(bankIndex)
{
    auto* v = new QVBoxLayout(this);
    m_meter = new MeterBar(this);
    m_meter->setTotal(SLOT_SIZE);
    v->addWidget(m_meter);

    m_list = new QListWidget(this);
    m_list->setMinimumWidth(320);
    v->addWidget(m_list, 1);

    auto* h = new QHBoxLayout();
    m_btnAdd    = new QPushButton("Add…", this);
    m_btnRemove = new QPushButton("Remove", this);   // kürzer
    m_btnClear  = new QPushButton("Clear", this);
    m_btnWrite  = new QPushButton("Write Slot", this);
    h->addWidget(m_btnAdd);
    h->addWidget(m_btnRemove);
    h->addWidget(m_btnClear);
    h->addStretch();
    h->addWidget(m_btnWrite);
    v->addLayout(h);

    connect(m_btnAdd,    &QPushButton::clicked, this, &BankWidget::addFiles);
    connect(m_btnRemove, &QPushButton::clicked, this, &BankWidget::removeSelected);
    connect(m_btnClear,  &QPushButton::clicked, this, &BankWidget::clear);
    connect(m_btnWrite,  &QPushButton::clicked, this, &BankWidget::doWriteSlot);

    refreshUi();
}

int BankWidget::usedBytes() const {
    int sum = 0; for (auto& p : m_parts) sum += p.data.size(); return sum;
}

QByteArray BankWidget::swap16(const QByteArray& in) {
    QByteArray out = in;
    if (out.size() % 2) out.append(char(0xff));
    for (int i = 0; i + 1 < out.size(); i += 2)
        std::swap(out[i], out[i+1]);
    return out;
}



int BankWidget::canonicalScore(const QByteArray& data) {
    int score = 0;
    if (data.size() >= 0x10) {
        const quint16 w0 = readBe16(data, 0);
        const quint16 w1 = readBe16(data, 2);
        if (w0 == 0x4EF9 || w1 == 0x4EF9 || w1 == 0xFC00) score += 3;

        const quint16 ver = readBe16(data, 0x0C);
        if (ver >= 30 && ver <= 60) score += 2;
    }

    // RomTag signal density in canonical byte order.
    int tags = 0;
    for (int i = 0; i + 1 < data.size(); i += 2) {
        if (readBe16(data, i) == 0x4AFC) {
            ++tags;
            if (tags >= 8) break;
        }
    }
    score += qMin(tags, 6);

    if (data.contains("exec.library") || data.contains("dos.library")) score += 2;

    return score;
}

bool BankWidget::shouldAutoSwap(const QFileInfo& fi, const QByteArray& raw) {
    const QString ext = fi.suffix().toLower();

    // .rom is historically word-swapped on disk for this workflow.
    if (ext == "rom") return true;

    // Compare canonical plausibility of raw vs swapped view to avoid wrong default swapping.
    const QByteArray swapped = swap16(raw);
    const int rawScore = canonicalScore(raw);
    const int swappedScore = canonicalScore(swapped);

    // Keep .bin stable unless swapped view is significantly more plausible.
    const int threshold = (ext == "bin") ? 3 : 1;
    return (swappedScore - rawScore) >= threshold;
}

quint32 BankWidget::readBe32(const QByteArray& in, int off) {
    if (off < 0 || off + 4 > in.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(in.constData() + off);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

quint16 BankWidget::readBe16(const QByteArray& in, int off) {
    if (off < 0 || off + 2 > in.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(in.constData() + off);
    return (quint16(p[0]) << 8) | quint16(p[1]);
}

void BankWidget::writeBe32(QByteArray& out, int off, quint32 v) {
    if (off < 0 || off + 4 > out.size()) return;
    out[off + 0] = char((v >> 24) & 0xff);
    out[off + 1] = char((v >> 16) & 0xff);
    out[off + 2] = char((v >> 8) & 0xff);
    out[off + 3] = char(v & 0xff);
}

bool BankWidget::looksLikeKickstartHeader(const QByteArray& image, int effectiveSize) {
    if (effectiveSize < 0x20 || image.size() < effectiveSize) return false;

    // Kickstart usually starts with 1111 4EF9... (KS2+) or FC00 03.. (KS1.x).
    const quint16 w0 = readBe16(image, 0);
    const quint16 w1 = readBe16(image, 2);
    const bool hasJump = (w0 == 0x4EF9) || (w1 == 0x4EF9) || (w1 == 0xFC00);

    const quint16 version = readBe16(image, 0x0C);
    const quint16 revision = readBe16(image, 0x0E);
    const bool plausibleVersion = (version >= 30 && version <= 60 && revision < 1000);

    return hasJump || plausibleVersion;
}

bool BankWidget::relocateRomTagPointers(QByteArray& image, int effectiveSize, QStringList* notes) {
    if (effectiveSize <= 0 || image.size() < effectiveSize) return false;

    quint32 oldBase = 0;
    bool haveOldBase = false;
    const quint32 newBase = 0x01000000u - quint32(effectiveSize);

    for (int off = 0; off + 26 <= effectiveSize; ++off) {
        if (readBe16(image, off) != 0x4AFC) continue;

        const quint32 matchTag24 = readBe32(image, off + 2) & 0x00FFFFFFu;
        if (!haveOldBase && matchTag24 >= quint32(off)) {
            const quint32 candidate = (matchTag24 - quint32(off)) & 0x00FFFFFFu;
            const quint32 end = candidate + quint32(effectiveSize);
            if (end <= 0x01000000u) {
                oldBase = candidate;
                haveOldBase = true;
            }
        }
    }

    if (!haveOldBase) return false;

    auto translate = [&](quint32 ptr24) -> quint32 {
        if (ptr24 >= oldBase && ptr24 < oldBase + quint32(effectiveSize)) {
            return newBase + (ptr24 - oldBase);
        }
        return ptr24;
    };

    int changed = 0;
    for (int off = 0; off + 26 <= effectiveSize; ++off) {
        if (readBe16(image, off) != 0x4AFC) continue;

        const int ptrFields[] = {2, 6, 14, 18, 22}; // MatchTag, EndSkip, Name, IdString, Init
        for (int field : ptrFields) {
            const quint32 oldFull = readBe32(image, off + field);
            const quint32 old24 = oldFull & 0x00FFFFFFu;
            const quint32 translated = translate(old24) & 0x00FFFFFFu;
            if (translated != old24) {
                const quint32 patched = (oldFull & 0xFF000000u) | translated;
                writeBe32(image, off + field, patched);
                ++changed;
            }
        }
    }

    if (notes && changed > 0) {
        notes->push_back(QString("Relocated %1 RomTag pointer fields to bank base 0x%2.")
                         .arg(changed)
                         .arg(newBase & 0x00FFFFFFu, 6, 16, QLatin1Char('0')));
    }

    return changed > 0;
}

QStringList BankWidget::validateRomTags(const QByteArray& image, int effectiveSize) {
    QStringList issues;
    if (effectiveSize <= 0 || image.size() < effectiveSize) return issues;

    const quint32 baseAddr = 0x01000000u - quint32(effectiveSize);
    const quint32 endAddr = baseAddr + quint32(effectiveSize);

    int checked = 0;
    for (int off = 0; off + 26 <= effectiveSize; ++off) {
        if (readBe16(image, off) != 0x4AFC) continue;

        const quint32 matchTag = readBe32(image, off + 2) & 0x00FFFFFFu;
        const quint32 endSkip = readBe32(image, off + 6) & 0x00FFFFFFu;
        const quint32 namePtr = readBe32(image, off + 14) & 0x00FFFFFFu;

        const quint32 selfAddr = baseAddr + quint32(off);
        if (matchTag != selfAddr) {
            issues << QString("RomTag @0x%1 has rt_MatchTag=0x%2 (expected self 0x%3)")
                          .arg(off, 6, 16, QLatin1Char('0'))
                          .arg(matchTag, 6, 16, QLatin1Char('0'))
                          .arg(selfAddr & 0x00FFFFFFu, 6, 16, QLatin1Char('0'));
        }
        if (endSkip <= selfAddr || endSkip > endAddr) {
            issues << QString("RomTag @0x%1 has invalid rt_EndSkip=0x%2")
                          .arg(off, 6, 16, QLatin1Char('0'))
                          .arg(endSkip, 6, 16, QLatin1Char('0'));
        }
        if (namePtr < baseAddr || namePtr >= endAddr) {
            issues << QString("RomTag @0x%1 has out-of-range rt_Name=0x%2")
                          .arg(off, 6, 16, QLatin1Char('0'))
                          .arg(namePtr, 6, 16, QLatin1Char('0'));
        }

        ++checked;
        if (issues.size() >= 16) {
            issues << "Further RomTag issues suppressed.";
            break;
        }
    }

    if (checked == 0) {
        issues << "No RomTag signatures (0x4AFC) found in composed image.";
    }

    return issues;
}

void BankWidget::finalizeKickstartChecksum(QByteArray& image, int effectiveSize) {
    if (effectiveSize <= 0 || effectiveSize > image.size() || (effectiveSize % 4) != 0) return;

    // Henne-Ei safe: checksum slot is treated as 0 during summation,
    // then filled with the additive inverse so global sum becomes 0xFFFFFFFF.
    const int checksumOff = effectiveSize - 4;
    writeBe32(image, checksumOff, 0);

    quint64 sum = 0;
    for (int off = 0; off < effectiveSize; off += 4) {
        sum += readBe32(image, off);
    }
    const quint32 partial = quint32(sum & 0xFFFFFFFFu);
    const quint32 checksum = quint32((0x100000000ULL - partial) & 0xFFFFFFFFu);
    writeBe32(image, checksumOff, checksum);
}

QByteArray BankWidget::buildTiled512k() const {
    QByteArray base;
    base.reserve(SLOT_SIZE);
    for (const auto& p : m_parts) base.append(p.data);

    if (base.isEmpty()) {
        return QByteArray(SLOT_SIZE, char(0xff));
    }

    static const int HALF_BANK = SLOT_SIZE / 2; // 256 KiB

    // For <= 256 KiB payloads, build a 256 KiB image and mirror it to 512 KiB.
    // This keeps classic 256 KiB ROM layout compatible in a 512 KiB bank.
    if (base.size() <= HALF_BANK) {
        QByteArray half = base.left(HALF_BANK);
        if (half.size() < HALF_BANK) {
            half.append(QByteArray(HALF_BANK - half.size(), char(0xff)));
        }

        // Rebase resident pointers to current bank mapping before checksum finalization.
        relocateRomTagPointers(half, HALF_BANK, nullptr);

        if (looksLikeKickstartHeader(half, HALF_BANK) || hasRomHeaderPart()) {
            finalizeKickstartChecksum(half, HALF_BANK);
        }

        QByteArray out;
        out.reserve(SLOT_SIZE);
        out.append(half);
        out.append(half);
        return out;
    }

    // > 256 KiB: keep linear layout and pad up to full 512 KiB bank.
    if (base.size() < SLOT_SIZE) {
        base.append(QByteArray(SLOT_SIZE - base.size(), char(0xff)));
    }

    QByteArray out = base.left(SLOT_SIZE);

    // Rebase resident pointers to current bank mapping before checksum finalization.
    relocateRomTagPointers(out, SLOT_SIZE, nullptr);

    if (looksLikeKickstartHeader(out, SLOT_SIZE) || hasRomHeaderPart()) {
        finalizeKickstartChecksum(out, SLOT_SIZE);
    }
    return out;
}


void BankWidget::loadSinglePart(const QString& name, const QByteArray& data, bool swapped) {
    m_parts.clear();

    RomPart part;
    part.name = name + (swapped ? " [swap16]" : "");
    part.data = data.left(SLOT_SIZE);
    part.swapped = swapped;
    m_parts.push_back(std::move(part));

    refreshUi();
    emit log(QString("Loaded into Slot %1: %2 (%3 KiB)")
             .arg(m_bank).arg(part.name).arg(part.data.size()/1024));
}

void BankWidget::clear() {
    m_parts.clear();
    refreshUi();
    emit log(QString("Cleared Slot %1").arg(m_bank));
}

void BankWidget::addFiles() {
    QStringList files = QFileDialog::getOpenFileNames(this,
        QString("Add ROM(s) to Slot %1").arg(m_bank),
        QString(), "ROM/Parts (*.bin *.rom *.library *.device);;All (*.*)");
    if (files.isEmpty()) return;

    for (const QString& path : files) {
        QFileInfo fi(path); QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Open failed", fi.fileName()); continue;
        }
        QByteArray raw = f.readAll();
        const bool autoSwap = shouldAutoSwap(fi, raw);
        QByteArray data = autoSwap ? swap16(raw) : raw;
        if (data.size() > SLOT_SIZE) {
            QMessageBox::warning(this, "Too large",
                                 QString("%1 exceeds 512 KiB").arg(fi.fileName()));
            continue;
        }
        if (usedBytes() + data.size() > SLOT_SIZE) {
            QMessageBox::warning(this, "Slot full",
                QString("Adding %1 would exceed 512 KiB in Slot %2").arg(fi.fileName()).arg(m_bank));
            continue;
        }
        const int beforeBytes = usedBytes();

        RomPart part;
        part.name    = fi.fileName() + (autoSwap ? " [swap16]" : "");
        part.data    = data;
        part.swapped = autoSwap;
        m_parts.push_back(std::move(part));

        emit log(QString("Added to Slot %1: %2 (%3 KiB)")
                 .arg(m_bank).arg(fi.fileName() + (autoSwap ? " [swap16]" : "")).arg(data.size()/1024));
        if (fi.suffix().compare("bin", Qt::CaseInsensitive) == 0 && autoSwap) {
            emit log(QString("Slot %1 notice: .bin was auto-swapped due to content heuristic.").arg(m_bank));
        }

        const int halfBank = SLOT_SIZE / 2;
        if (beforeBytes <= halfBank && usedBytes() > halfBank) {
            emit log(QString("Slot %1 warning: payload exceeds 256 KiB; 256..512 KiB region is now linear (hatched in meter).")
                     .arg(m_bank));
        }
    }
    refreshUi();
}

void BankWidget::removeSelected() {
    int sel = m_list->currentRow();
    if (sel < 0 || sel >= m_parts.size()) return;
    auto name = m_parts[sel].name;
    m_parts.remove(sel);
    emit log(QString("Removed from Slot %1: %2").arg(m_bank).arg(name));
    refreshUi();
}

void BankWidget::doWriteSlot() {
    if (ensureRomHeaderFirst()) {
        emit log(QString("Slot %1: moved __rom_header to first position before write.").arg(m_bank));
        refreshUi();
    }

    const int effectiveSize = (usedBytes() <= SLOT_SIZE / 2) ? SLOT_SIZE / 2 : SLOT_SIZE;
    QByteArray preview = buildTiled512k();
    QStringList relocateNotes;
    relocateRomTagPointers(preview, effectiveSize, &relocateNotes);
    for (const auto& n : relocateNotes) {
        emit log(QString("Slot %1 preflight: %2").arg(m_bank).arg(n));
    }

    const auto issues = validatePartsForCurrentLayout();
    for (const auto& issue : issues) {
        emit log(QString("Slot %1 preflight: %2").arg(m_bank).arg(issue));
    }

    if (!hasRomHeaderPart()) {
        emit log(QString("Slot %1 notice: no __rom_header part detected. Header/vectors may be incomplete for modified Kickstart ROMs.")
                 .arg(m_bank));
    }

    QByteArray img = buildTiled512k();
    emit requestWriteSlot(m_bank, img);
}

QStringList BankWidget::validatePartRomTags(int effectiveSize) const {
    QStringList issues;
    if (effectiveSize <= 0) return issues;

    const quint32 baseAddr = 0x01000000u - quint32(effectiveSize);
    int absOffset = 0;
    for (const auto& part : m_parts) {
        const int partStart = absOffset;
        const QByteArray& data = part.data;
        for (int off = 0; off + 26 <= data.size(); ++off) {
            if (readBe16(data, off) != 0x4AFC) continue;
            const quint32 matchTag = readBe32(data, off + 2) & 0x00FFFFFFu;
            const quint32 expected = (baseAddr + quint32(partStart + off)) & 0x00FFFFFFu;
            if (matchTag != expected) {
                issues << QString("Part '%1': RomTag @+0x%2 has rt_MatchTag=0x%3 (expected 0x%4 after current placement)")
                              .arg(part.name)
                              .arg(off, 6, 16, QLatin1Char('0'))
                              .arg(matchTag, 6, 16, QLatin1Char('0'))
                              .arg(expected, 6, 16, QLatin1Char('0'));
                if (issues.size() >= 16) {
                    issues << "Further per-part RomTag issues suppressed.";
                    return issues;
                }
            }
        }
        absOffset += part.data.size();
    }
    return issues;
}

QStringList BankWidget::validatePartsForCurrentLayout() const {
    QStringList issues;
    if (m_parts.isEmpty()) return issues;

    const int effectiveSize = (usedBytes() <= SLOT_SIZE / 2) ? SLOT_SIZE / 2 : SLOT_SIZE;
    if (effectiveSize <= 0) return issues;

    issues << validatePartRomTags(effectiveSize);

    QByteArray img = buildTiled512k();
    if (looksLikeKickstartHeader(img, effectiveSize)) {
        issues << validateRomTags(img, effectiveSize);
        if (!hasRomHeaderPart()) {
            issues << "No __rom_header part present; header/vectors may be incomplete.";
        }
    }

    return issues;
}

bool BankWidget::ensureRomHeaderFirst() {
    for (int i = 0; i < m_parts.size(); ++i) {
        if (!m_parts[i].name.contains("__rom_header", Qt::CaseInsensitive)) continue;
        if (i == 0) return false;
        RomPart header = m_parts.takeAt(i);
        m_parts.prepend(std::move(header));
        return true;
    }
    return false;
}

bool BankWidget::hasRomHeaderPart() const {
    for (const auto& p : m_parts) {
        if (p.name.contains("__rom_header", Qt::CaseInsensitive)) return true;
    }
    return false;
}

void BankWidget::updateWriteButtonState() {
    const auto issues = validatePartsForCurrentLayout();
    if (issues.isEmpty()) {
        m_btnWrite->setToolTip(QString("Slot %1 preflight: no obvious issues detected.").arg(m_bank));
        return;
    }

    m_btnWrite->setToolTip(QString("Slot %1 preflight warnings (%2):\n- %3")
                           .arg(m_bank)
                           .arg(issues.size())
                           .arg(issues.join("\n- ")));
}

void BankWidget::refreshUi() {
    m_list->clear();
    QVector<int> seg;
    for (int i=0;i<m_parts.size();++i) {
        const auto& p = m_parts[i];
        auto* item = new QListWidgetItem(
            QString("%1 (%2 KiB)").arg(p.name).arg(p.data.size()/1024));
        QColor color = (i % 2 == 0) ? QColor("#3b82f6") : QColor("#10b981");
        item->setBackground(color);
        item->setForeground(Qt::black);
        m_list->addItem(item);
        seg.push_back(p.data.size());
    }
    m_meter->setSegments(seg);

    const int used = usedBytes();
    const int halfBank = SLOT_SIZE / 2;
    if (used <= halfBank) {
        m_meter->setToolTip(QString("Slot %1: %2/%3 KiB (mirrored from 256 to 512 KiB)")
                            .arg(m_bank).arg(used/1024).arg(SLOT_SIZE/1024));
    } else {
        m_meter->setToolTip(QString("Slot %1: %2/%3 KiB (above 256 KiB, linear layout in upper half)")
                            .arg(m_bank).arg(used/1024).arg(SLOT_SIZE/1024));
    }

    updateWriteButtonState();
}
