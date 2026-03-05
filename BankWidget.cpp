#include "BankWidget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <cstring>

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



bool BankWidget::shouldAutoSwap(const QFileInfo& fi) {
    const QString ext = fi.suffix().toLower();

    // Only .rom files use the historical byte-swapped (word-swapped) format
    // that EPROM programmers expect.  Everything else is canonical big-endian.
    return (ext == "rom");
}

/* ---------------------------------------------------------------------------
   hasCanonicalSignatures – heuristic: does `data` look like canonical
   big-endian Amiga ROM content?  Checks for RomTag magic (0x4AFC) and
   Kickstart header patterns at plausible positions.
   ----------------------------------------------------------------------- */
bool BankWidget::hasCanonicalSignatures(const QByteArray& data) {
    if (data.size() < 2) return false;

    // Check first word for RomTag magic.
    if (readBe16(data, 0) == 0x4AFC) return true;

    // Kickstart header: 0x1111 0x4EF9  or  0x4EF9 at word 0.
    if (data.size() >= 4) {
        const quint16 w0 = readBe16(data, 0);
        const quint16 w1 = readBe16(data, 2);
        if (w0 == 0x1111 && w1 == 0x4EF9) return true;
        if (w0 == 0x4EF9) return true;
    }

    // Scan first 256 bytes for RomTag (some components have a small preamble).
    for (int off = 2; off + 2 <= qMin(data.size(), 256); off += 2) {
        if (readBe16(data, off) == 0x4AFC) return true;
    }

    // Deterministic policy: .rom is swapped, .bin is canonical and stays unchanged.
    if (ext == "rom") return true;
    return false;
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

    // Amiga Kickstart checksum: the carry-propagating (ones' complement)
    // sum of all 32-bit big-endian longwords must equal 0xFFFFFFFF.
    // This matches exec.library's SumKickData() which uses:
    //   add.l (a0)+,d0 / bcc.s .noc / addq.l #1,d0
    const int checksumOff = effectiveSize - 4;
    writeBe32(image, checksumOff, 0);

    quint32 sum = 0;
    for (int off = 0; off < effectiveSize; off += 4) {
        quint32 old = sum;
        sum += readBe32(image, off);
        if (sum < old) ++sum;           // fold carry (ones' complement add)
    }
    const quint32 checksum = ~sum;
    writeBe32(image, checksumOff, checksum);
}

/* ---------------------------------------------------------------------------
   detectOriginalAddr – determine the original ROM address of a component.

   For __rom_header parts the address equals the ROM base (detected later).
   For normal components the first bytes are the RomTag (0x4AFC magic),
   and rt_MatchTag (bytes 2-5) encodes the original absolute address.
   Returns 0 for __rom_header parts (always placed at ROM base),
   or the 24-bit rt_MatchTag value for regular components.
   ----------------------------------------------------------------------- */
quint32 BankWidget::detectOriginalAddr(const QByteArray& data, const QString& name) {
    // __rom_header is always placed at offset 0 (= baseAddr).
    if (name.contains("__rom_header", Qt::CaseInsensitive))
        return 0;

    // Accept any 24-bit address that could be an Amiga ROM mapping.
    // Standard Kickstart ranges: 256K→0xFC0000, 512K→0xF80000,
    // 1MB→0xF00000, 2MB→0xE00000.  Extended/card ROMs can start lower.
    auto plausibleRomAddr = [](quint32 addr) -> bool {
        return addr >= 0x00800000u && addr < 0x01000000u;
    };

    // Regular component: RomTag should be at the very start.
    if (data.size() >= 6 && readBe16(data, 0) == 0x4AFC) {
        quint32 matchTag = readBe32(data, 2) & 0x00FFFFFFu;
        if (plausibleRomAddr(matchTag))
            return matchTag;
    }

    // Fallback: scan first 64 bytes for a RomTag.
    for (int off = 2; off + 6 <= qMin(data.size(), 64); off += 2) {
        if (readBe16(data, off) != 0x4AFC) continue;
        quint32 matchTag = readBe32(data, off + 2) & 0x00FFFFFFu;
        if (plausibleRomAddr(matchTag))
            return matchTag;
    }

    return 0;   // unknown – will trigger concatenation fallback
}

/* ---------------------------------------------------------------------------
   verifyKickstartChecksum – re-compute the ones' complement sum over the
   given effective range and return true iff it equals 0xFFFFFFFF.
   ----------------------------------------------------------------------- */
bool BankWidget::verifyKickstartChecksum(const QByteArray& image, int effectiveSize) {
    if (effectiveSize <= 0 || effectiveSize > image.size() || (effectiveSize % 4) != 0)
        return false;
    quint32 sum = 0;
    for (int off = 0; off < effectiveSize; off += 4) {
        quint32 old = sum;
        sum += readBe32(image, off);
        if (sum < old) ++sum;
    }
    return (sum == 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------------------
   relocateRomTags – patch absolute addresses inside Resident (RomTag)
   structures so that a bank composed from individually extracted components
   boots correctly even when modules are omitted or reordered.

   Each RomTag carries several absolute ROM pointers that encode the
   component's original position.  When a preceding component is removed the
   remaining modules shift to lower offsets and every one of those pointers
   becomes invalid.  The Amiga ROM scanner (exec/InitResident) rejects any
   RomTag whose rt_MatchTag does not equal its own address, so shifted
   modules are silently skipped – which is fatal for essential drivers.

   This function scans the composed image for RomTag magic (0x4AFC), detects
   mismatched self-pointers, and adjusts:

     1. RomTag fields:  rt_MatchTag, rt_EndSkip, rt_Name, rt_IdString, rt_Init
     2. RTF_AUTOINIT init-struct fields:  funcTable, dataInit, initFunc
     3. Absolute function-table entries (if the table is not in relative mode)
     4. Reset vector (Initial PC at offset +4) when the effective base changes

   Returns the number of RomTags that were patched (0 when the image was
   already consistent).
   ----------------------------------------------------------------------- */
int BankWidget::relocateRomTags(QByteArray& image, int effectiveSize) {
    if (effectiveSize <= 0 || image.size() < effectiveSize) return 0;
    if ((effectiveSize % 2) != 0) return 0;

    const quint32 baseAddr = 0x01000000u - quint32(effectiveSize);
    const quint32 endAddr  = baseAddr + quint32(effectiveSize);

    int patched = 0;

    for (int off = 0; off + 26 <= effectiveSize; off += 2) {
        if (readBe16(image, off) != 0x4AFC) continue;

        const quint32 matchTag = readBe32(image, off + 2) & 0x00FFFFFFu;
        const quint32 selfAddr = baseAddr + quint32(off);

        if (matchTag == selfAddr) {          // already in place
            off += 24;                       // skip past this RomTag structure
            continue;
        }

        // matchTag must look like a Kickstart ROM address (0x00F80000..0x00FFFFFF)
        if (matchTag < 0x00F80000u) continue;

        const qint64 delta = qint64(selfAddr) - qint64(matchTag);

        // Plausibility gate: after relocation rt_Name must point to printable ASCII.
        const quint32 namePtr = readBe32(image, off + 14) & 0x00FFFFFFu;
        if (namePtr != 0) {
            quint32 newName = quint32(qint64(namePtr) + delta);
            if (newName < baseAddr || newName >= endAddr) continue;
            unsigned char c0 = static_cast<unsigned char>(image.at(int(newName - baseAddr)));
            if (c0 < 0x20 || c0 > 0x7e) continue;   // not printable → false 0x4AFC hit
        }

        // ---- patch RomTag fields ----

        // rt_MatchTag (+2) – self-pointer
        writeBe32(image, off + 2, selfAddr);

        // rt_EndSkip (+6) – scanner resume address; clamp into [self+26 .. endAddr]
        {
            quint32 es = readBe32(image, off + 6) & 0x00FFFFFFu;
            if (es != 0) {
                quint32 newEs = quint32(qint64(es) + delta);
                if (newEs > endAddr) newEs = endAddr;
                if (newEs <= selfAddr) newEs = selfAddr + 26;
                writeBe32(image, off + 6, newEs);
            }
        }

        // rt_Name (+14)
        if (namePtr != 0)
            writeBe32(image, off + 14, quint32(qint64(namePtr) + delta));

        // rt_IdString (+18)
        {
            quint32 ids = readBe32(image, off + 18) & 0x00FFFFFFu;
            if (ids != 0) {
                quint32 newIds = quint32(qint64(ids) + delta);
                if (newIds >= baseAddr && newIds < endAddr)
                    writeBe32(image, off + 18, newIds);
            }
        }

        // rt_Init (+22) – init function or AUTOINIT table pointer
        quint32 newInitAddr = 0;
        {
            quint32 initVal = readBe32(image, off + 22) & 0x00FFFFFFu;
            if (initVal != 0) {
                newInitAddr = quint32(qint64(initVal) + delta);
                if (newInitAddr >= baseAddr && newInitAddr < endAddr)
                    writeBe32(image, off + 22, newInitAddr);
                else
                    newInitAddr = 0;
            }
        }

        // ---- RTF_AUTOINIT: patch the init-struct pointed to by rt_Init ----
        const quint8 flags = static_cast<quint8>(image.at(off + 10));
        if ((flags & 0x80) && newInitAddr != 0) {
            const int iso = int(newInitAddr - baseAddr);   // image offset of init struct
            if (iso >= 0 && iso + 16 <= effectiveSize) {
                // struct layout:  +0 dataSize | +4 funcTable | +8 dataInit | +12 initFunc
                quint32 newFuncTab = 0;
                const int fields[] = { 4, 8, 12 };
                for (int f : fields) {
                    quint32 ptr = readBe32(image, iso + f) & 0x00FFFFFFu;
                    if (ptr == 0) continue;
                    quint32 np = quint32(qint64(ptr) + delta);
                    if (np >= baseAddr && np < endAddr) {
                        writeBe32(image, iso + f, np);
                        if (f == 4) newFuncTab = np;
                    }
                }

                // If funcTable uses absolute pointers (first word != 0xFFFF),
                // patch every 32-bit entry until the 0xFFFFFFFF terminator.
                if (newFuncTab != 0) {
                    int ftOff = int(newFuncTab - baseAddr);
                    if (ftOff >= 0 && ftOff + 2 <= effectiveSize &&
                        readBe16(image, ftOff) != 0xFFFF)
                    {
                        const int maxEntries = 256;  // reasonable upper bound
                        for (int n = 0, e = ftOff; n < maxEntries && e + 4 <= effectiveSize; ++n, e += 4) {
                            quint32 entry = readBe32(image, e);
                            if (entry == 0xFFFFFFFFu) break;
                            quint32 e24 = entry & 0x00FFFFFFu;
                            if (e24 < 0x00F80000u) continue;   // not a ROM pointer
                            quint32 ne = quint32(qint64(e24) + delta);
                            if (ne >= baseAddr && ne < endAddr)
                                writeBe32(image, e, ne);
                        }
                    }
                }
            }
        }

        ++patched;
        off += 24;   // skip rest of RomTag; loop adds +2 → next check at off+26
    }

    // ---- Patch reset vector (Initial PC at offset +4) when base changed ----
    if (patched > 0 && image.size() >= 8) {
        const quint32 pc = readBe32(image, 4) & 0x00FFFFFFu;
        if (pc >= 0x00F80000u && (pc < baseAddr || pc >= endAddr)) {
            const quint32 trySizes[] = { 256u * 1024u, 512u * 1024u };
            for (quint32 sz : trySizes) {
                quint32 tryBase = 0x01000000u - sz;
                if (pc >= tryBase && pc < tryBase + sz) {
                    quint32 newPc = baseAddr + (pc - tryBase);
                    if (newPc >= baseAddr && newPc < endAddr)
                        writeBe32(image, 4, newPc);
                    break;
                }
            }
        }
    }

    return patched;
}

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
    if (m_parts.isEmpty()) {
        return QByteArray(SLOT_SIZE, char(0xff));
    }

    static const int HALF_BANK = SLOT_SIZE / 2; // 256 KiB

    /* ------------------------------------------------------------------
       Gap-filling strategy: if ALL loaded parts have a known original
       ROM address (from their RomTag's rt_MatchTag), place each one at
       its ORIGINAL offset inside the image.  Gaps left by omitted
       components are filled with 0xFF.  This preserves every absolute
       address in the 68000 machine code and is the only way to safely
       drop individual modules from a Kickstart ROM.
       ------------------------------------------------------------------ */
    bool canGapFill = (m_parts.size() > 1);   // single-part = monolithic, no gap-fill needed
    bool hasHeader = false;
    quint32 addrMin = 0x01000000u;
    quint32 addrMax = 0;

    if (canGapFill) {
        for (const auto& p : m_parts) {
            if (p.name.contains("__rom_header", Qt::CaseInsensitive)) {
                hasHeader = true;
                continue;   // header is always at offset 0
            }
            if (p.originalAddr == 0) {
                canGapFill = false;     // unknown address → fall back
                break;
            }
            if (p.originalAddr < addrMin) addrMin = p.originalAddr;
            quint32 end = p.originalAddr + quint32(p.data.size());
            if (end > addrMax) addrMax = end;
        }
    }

    if (canGapFill && addrMin >= 0x00800000u && addrMax <= 0x01000000u) {
        // Determine original ROM size from the address range spanned.
        // baseAddr = 0x01000000 - effectiveSize; pick the smallest
        // power-of-two effectiveSize (256K or 512K) whose base ≤ addrMin.
        int effectiveSize = HALF_BANK;  // try 256 KiB first
        quint32 baseAddr = 0x01000000u - quint32(effectiveSize);
        if (addrMin < baseAddr) {
            effectiveSize = SLOT_SIZE;  // need 512 KiB
            baseAddr = 0x01000000u - quint32(effectiveSize);
        }

        if (addrMin >= baseAddr) {      // addresses fit
            QByteArray image(effectiveSize, char(0xff));

            // Place each part at its original ROM offset.
            for (const auto& p : m_parts) {
                int destOff;
                if (p.name.contains("__rom_header", Qt::CaseInsensitive)) {
                    destOff = 0;
                } else {
                    destOff = int(p.originalAddr - baseAddr);
                }
                if (destOff < 0 || destOff + p.data.size() > effectiveSize) continue;
                memcpy(image.data() + destOff, p.data.constData(), p.data.size());
            }

            if (looksLikeKickstartHeader(image, effectiveSize)) {
                finalizeKickstartChecksum(image, effectiveSize);
            }

            // If 256 KiB effective: mirror to fill 512 KiB bank.
            if (effectiveSize == HALF_BANK) {
                QByteArray out;
                out.reserve(SLOT_SIZE);
                out.append(image);
                out.append(image);
                return out;
            }
            return image;
        }
        // else: addresses don't fit in 512 KiB → fall through to concatenation
    }

    /* ------------------------------------------------------------------
       Fallback: simple concatenation (for monolithic parts, non-component
       binaries, or parts without detectable original addresses).
       Applies RomTag relocation as a best-effort fixup.
       ------------------------------------------------------------------ */
    QByteArray base;
    base.reserve(SLOT_SIZE);
    for (const auto& p : m_parts) base.append(p.data);

    if (base.size() <= HALF_BANK) {
        QByteArray half = base.left(HALF_BANK);
        if (half.size() < HALF_BANK) {
            half.append(QByteArray(HALF_BANK - half.size(), char(0xff)));
        }
        relocateRomTags(half, HALF_BANK);
        if (looksLikeKickstartHeader(half, HALF_BANK)) {
            finalizeKickstartChecksum(half, HALF_BANK);
        }


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
    relocateRomTags(out, SLOT_SIZE);
    if (looksLikeKickstartHeader(out, SLOT_SIZE)) {

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

        // --- Intelligent byte-order detection ---
        // 1. Extension hint (shouldAutoSwap): only .rom is swapped.
        // 2. Content verification: check if the result contains recognisable
        //    big-endian structures (0x4AFC RomTag, Kickstart header).
        //    If the extension-based choice looks wrong, try the other order.
        bool autoSwap = shouldAutoSwap(fi);
        QByteArray data;

        if (fi.suffix().toLower() == "bin") {
            // .bin is ALWAYS canonical – no second-guessing.
            data = raw;
            autoSwap = false;
        } else if (hasCanonicalSignatures(raw)) {
            // Raw data already looks canonical big-endian → use as-is.
            data = raw;
            if (autoSwap) {
                emit log(QString("Note: %1 has canonical signatures despite .rom extension; using raw data.")
                         .arg(fi.fileName()));
                autoSwap = false;
            }
        } else {
            QByteArray swapped = swap16(raw);
            if (hasCanonicalSignatures(swapped)) {
                // Swapped data looks canonical → input was byte-swapped.
                data = swapped;
                if (!autoSwap) {
                    emit log(QString("Note: auto-detected byte-swapped format for %1 (converted to canonical).")
                             .arg(fi.fileName()));
                }
                autoSwap = true;
            } else {
                // Neither order shows recognisable structures → use extension hint.
                data = autoSwap ? swap16(raw) : raw;
                emit log(QString("Warning: %1 has no recognisable ROM/RomTag signatures in either byte order; "
                                 "using extension-based heuristic (%2).")
                         .arg(fi.fileName())
                         .arg(autoSwap ? "swapped" : "as-is"));
            }
        }

        if (fi.fileName().contains("__rom_checksum", Qt::CaseInsensitive)) {
            emit log(QString("Slot %1: skipping %2 (derived checksum metadata; recomputed on write).")
                     .arg(m_bank).arg(fi.fileName()));
            continue;
        }
        const bool autoSwap = shouldAutoSwap(fi);
        QByteArray data = autoSwap ? swap16(raw) : raw;
        const QString ext = fi.suffix().toLower();
        if (ext != "bin" && ext != "rom") {
            emit log(QString("Slot %1 notice: treating %2 as canonical (no auto-swap).")
                     .arg(m_bank).arg(fi.fileName()));
        }
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
        part.originalAddr = detectOriginalAddr(data, fi.fileName());

        const QString partLabel = part.name;
        const quint32 partAddr  = part.originalAddr;
        m_parts.push_back(std::move(part));

        // Log first 6 bytes + detection result for diagnostics
        QString hexPrefix;
        for (int b = 0; b < qMin(data.size(), 6); ++b)
            hexPrefix += QString("%1 ").arg(quint8(data[b]), 2, 16, QLatin1Char('0'));
        emit log(QString("Added to Slot %1: %2 (%3 KiB, origAddr=0x%4, first=[%5])")
                 .arg(m_bank)
                 .arg(partLabel)
                 .arg(data.size()/1024)
                 .arg(partAddr, 6, 16, QLatin1Char('0'))
                 .arg(hexPrefix.trimmed()));
        m_parts.push_back(std::move(part));

        emit log(QString("Added to Slot %1: %2 (%3 KiB)")
                 .arg(m_bank).arg(fi.fileName() + (autoSwap ? " [swap16]" : "")).arg(data.size()/1024));

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

    if (!hasRomHeaderPart()) {
        emit log(QString("Slot %1 notice: no __rom_header part detected. Header/vectors may be incomplete for modified Kickstart ROMs.")
                 .arg(m_bank));
    }

    // --- diagnostic: determine which build strategy will be used ---
    static const int HALF_BANK = SLOT_SIZE / 2;
    bool diagGapFill = (m_parts.size() > 1);
    if (diagGapFill) {
        for (const auto& p : m_parts) {
            if (p.name.contains("__rom_header", Qt::CaseInsensitive)) continue;
            if (p.originalAddr == 0) {
                emit log(QString("Slot %1 diag: part '%2' has no detectable originalAddr → concatenation fallback")
                         .arg(m_bank).arg(p.name));
                diagGapFill = false;
                break;
            }
        }
    }
    if (diagGapFill) {
        emit log(QString("Slot %1 diag: using GAP-FILL placement (%2 parts)")
                 .arg(m_bank).arg(m_parts.size()));
        for (const auto& p : m_parts) {
            emit log(QString("  %1: origAddr=0x%2, size=%3")
                     .arg(p.name)
                     .arg(p.originalAddr, 6, 16, QLatin1Char('0'))
                     .arg(p.data.size()));
        }
    } else if (m_parts.size() > 1) {
        emit log(QString("Slot %1 diag: using CONCATENATION fallback (gap-fill not possible)")
    const bool hadHeaderFirst = (!m_parts.isEmpty() && m_parts[0].name.contains("__rom_header", Qt::CaseInsensitive));
    int execBefore = -1; for (int i = 0; i < m_parts.size(); ++i) { if (m_parts[i].name.contains("exec", Qt::CaseInsensitive)) { execBefore = i; break; } }
    normalizeComponentOrder();
    int execAfter = -1; for (int i = 0; i < m_parts.size(); ++i) { if (m_parts[i].name.contains("exec", Qt::CaseInsensitive)) { execAfter = i; break; } }
    const bool hasHeaderFirst = (!m_parts.isEmpty() && m_parts[0].name.contains("__rom_header", Qt::CaseInsensitive));
    if (hadHeaderFirst != hasHeaderFirst || execBefore != execAfter) {
        emit log(QString("Slot %1: normalized component order (__rom_header first, exec early) before write.").arg(m_bank));
        refreshUi();
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

    // --- diagnostic: verify checksum ---
    // Compute effectiveSize the same way buildTiled512k does.
    int effectiveSize;
    if (diagGapFill) {
        quint32 am = 0x01000000u;
        for (const auto& p : m_parts) {
            if (p.name.contains("__rom_header", Qt::CaseInsensitive)) continue;
            if (p.originalAddr != 0 && p.originalAddr < am) am = p.originalAddr;
        }
        effectiveSize = (am < 0x00FC0000u) ? SLOT_SIZE : HALF_BANK;
    } else {
        effectiveSize = (usedBytes() <= HALF_BANK) ? HALF_BANK : SLOT_SIZE;
    }
    const bool csOk = verifyKickstartChecksum(img, effectiveSize);
    const quint32 csVal = readBe32(img, effectiveSize - 4);
    emit log(QString("Slot %1 diag: effectiveSize=%2, checksum=0x%3, verify=%4")
             .arg(m_bank)
             .arg(effectiveSize)
             .arg(csVal, 8, 16, QLatin1Char('0'))
             .arg(csOk ? "PASS" : "FAIL"));

    // Log first 8 bytes (reset vectors) for sanity
    if (img.size() >= 8) {
        emit log(QString("Slot %1 diag: header bytes: %2 %3 %4 %5 %6 %7 %8 %9")
                 .arg(m_bank)
                 .arg(quint8(img[0]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[1]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[2]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[3]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[4]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[5]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[6]), 2, 16, QLatin1Char('0'))
                 .arg(quint8(img[7]), 2, 16, QLatin1Char('0')));
    }

    // Save diagnostic image to temp for inspection / comparison
    {
        const QString diagPath = QDir::temp().filePath(QString("slot%1_diag.bin").arg(m_bank));
        QFile diagFile(diagPath);
        if (diagFile.open(QIODevice::WriteOnly)) {
            diagFile.write(img);
            diagFile.close();
            const QByteArray sha = QCryptographicHash::hash(img, QCryptographicHash::Sha256).toHex().left(16);
            emit log(QString("Slot %1 diag: image saved to %2 (%3 bytes, sha256=%4…)")
                     .arg(m_bank).arg(diagPath).arg(img.size()).arg(QString::fromLatin1(sha)));
        }
    }

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

void BankWidget::normalizeComponentOrder() {
    if (m_parts.size() < 2) return;

    ensureRomHeaderFirst();

    int headerIdx = -1;
    for (int i = 0; i < m_parts.size(); ++i) {
        if (m_parts[i].name.contains("__rom_header", Qt::CaseInsensitive)) {
            headerIdx = i;
            break;
        }
    }

    for (int i = 0; i < m_parts.size(); ++i) {
        if (!m_parts[i].name.contains("exec", Qt::CaseInsensitive)) continue;
        const int target = (headerIdx == 0) ? 1 : 0;
        if (i == target) return;
        RomPart exec = m_parts.takeAt(i);
        m_parts.insert(target, std::move(exec));
        return;
    }
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
