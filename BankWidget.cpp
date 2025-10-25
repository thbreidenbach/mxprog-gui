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
    p.fillRect(rect(), Qt::white);
    p.setPen(Qt::black);

    int W = width();
    int H = height();
    int x = 0;
    for (int i = 0; i < m_sizes.size(); ++i) {
        double frac = double(m_sizes[i]) / double(m_total);
        int w = qMax(1, int(W * frac));
        QColor color = (i % 2 == 0) ? QColor("#3b82f6") : QColor("#10b981");
        p.fillRect(QRect(x,0,w,H), color);
        p.drawRect(QRect(x,0,w,H));
        x += w;
    }
    p.drawRect(rect().adjusted(0,0,-1,-1));
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

QByteArray BankWidget::buildTiled512k() const {
    QByteArray base;
    for (auto& p : m_parts) base.append(p.data);

    if (base.isEmpty()) {
        return QByteArray(SLOT_SIZE, char(0xff)); // leer -> 0xFF
    }
    if (base.size() > SLOT_SIZE) {
        return base.left(SLOT_SIZE);
    }
    if (base.size() == SLOT_SIZE) return base;

    int repeats  = SLOT_SIZE / base.size();
    int rem      = SLOT_SIZE % base.size();
    QByteArray out; out.reserve(SLOT_SIZE);
    for (int i=0; i<repeats; ++i) out.append(base);
    if (rem) out.append(base.constData(), rem);
    return out;
}

void BankWidget::clear() {
    m_parts.clear();
    refreshUi();
    emit log(QString("Cleared Slot %1").arg(m_bank));
}

void BankWidget::addFiles() {
    QStringList files = QFileDialog::getOpenFileNames(this,
        QString("Add ROM(s) to Slot %1").arg(m_bank),
        QString(), "ROM/Binary (*.bin *.rom);;Binary (*.bin);;Amiga ROM (*.rom);;All (*.*)");
    if (files.isEmpty()) return;

    for (const QString& path : files) {
        QFileInfo fi(path); QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Open failed", fi.fileName()); continue;
        }
        QByteArray raw = f.readAll();
        bool isRom = fi.suffix().compare("rom", Qt::CaseInsensitive) == 0;
        QByteArray data = isRom ? swap16(raw) : raw;
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
        RomPart part;
        part.name    = fi.fileName() + (isRom ? " [swap16]" : "");
        part.data    = data;
        part.swapped = isRom;
        m_parts.push_back(std::move(part));
        emit log(QString("Added to Slot %1: %2 (%3 KiB)")
                 .arg(m_bank).arg(fi.fileName() + (isRom ? " [swap16]" : "")).arg(data.size()/1024));
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
    QByteArray img = buildTiled512k();
    emit requestWriteSlot(m_bank, img);
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
}
