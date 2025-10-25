#pragma once
#include <QWidget>
#include <QListWidget>
#include <QByteArray>
#include <QVector>
#include <QPushButton>

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
    QByteArray buildTiled512k() const;   // Kachelt bis exakt 512 KiB; leer -> 0xFF
    void clear();

signals:
    void requestWriteSlot(int bank, const QByteArray& img512k);
    void log(const QString& line);

public slots:
    void addFiles();
    void removeSelected();
    void doWriteSlot();

private:
    static QByteArray swap16(const QByteArray& in);
    void refreshUi();

    int m_bank;
    QListWidget* m_list;
    MeterBar*    m_meter;
    QPushButton* m_btnAdd;
    QPushButton* m_btnRemove;
    QPushButton* m_btnClear;
    QPushButton* m_btnWrite;

    QVector<RomPart> m_parts;
};
