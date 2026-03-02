#pragma once
#include <QWidget>
#include <QListWidget>
#include <QByteArray>
#include <QVector>
#include <QPushButton>
#include <QFileInfo>
#include <QStringList>
#include <QtGlobal>

struct RomPart {
    QString     name;
    QByteArray  data;   // ggf. bereits swap16-konvertiert
    bool        swapped = false;
};

class MeterBar : public QWidget {
    Q_OBJECT
public:
    explicit MeterBar(QWidget* parent=nullptr);
    void setSegments(const QVector<int>& sizes); // in Bytes
    void setTotal(int total); // z.B. 512 KiB
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QVector<int> m_sizes;
    int m_total = 1;
};

class BankWidget : public QWidget {
    Q_OBJECT
public:
    explicit BankWidget(int bankIndex, QWidget* parent=nullptr);

    static constexpr int SLOT_SIZE = 512 * 1024;

    int bank() const { return m_bank; }
    int usedBytes() const;
    QByteArray buildTiled512k() const;   // <=256KiB: auf 256KiB auffüllen+spiegeln; >256KiB: auf 512KiB mit 0xFF
    void clear();
    void loadSinglePart(const QString& name, const QByteArray& data, bool swapped = false);

signals:
    void requestWriteSlot(int bank, const QByteArray& img512k);
    void log(const QString& line);

public slots:
    void addFiles();
    void removeSelected();
    void doWriteSlot();

private:
    static QByteArray swap16(const QByteArray& in);
    static bool shouldAutoSwap(const QFileInfo& fi);
    static quint32 readBe32(const QByteArray& in, int off);
    static quint16 readBe16(const QByteArray& in, int off);
    static void writeBe32(QByteArray& out, int off, quint32 v);
    static void finalizeKickstartChecksum(QByteArray& image, int effectiveSize);
    static bool looksLikeKickstartHeader(const QByteArray& image, int effectiveSize);
    static QStringList validateRomTags(const QByteArray& image, int effectiveSize);
    static bool relocateRomTagPointers(QByteArray& image, int effectiveSize, QStringList* notes = nullptr);
    QStringList validatePartRomTags(int effectiveSize) const;
    QStringList validatePartsForCurrentLayout() const;
    bool ensureRomHeaderFirst();
    void normalizeComponentOrder();
    bool hasRomHeaderPart() const;
    void refreshUi();
    void updateWriteButtonState();

    int m_bank;
    QListWidget* m_list;
    MeterBar*    m_meter;
    QPushButton* m_btnAdd;
    QPushButton* m_btnRemove;
    QPushButton* m_btnClear;
    QPushButton* m_btnWrite;

    QVector<RomPart> m_parts;
};
