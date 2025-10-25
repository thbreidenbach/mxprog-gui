#pragma once
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QProcess>
#include <QScrollArea>
#include <QLineEdit>
#include <QSettings>
#include <QQueue>
#include <QRegularExpression>
#include <QProgressBar>
#include <QTimer>

#include "BankWidget.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent=nullptr);

private slots:
    void writeSlot(int bank, const QByteArray& img512k);
    void writeAllMonolithic();
    void saveMonolithic();
    void identify();
    void erase();
    void readDump();
    void terminal();

    void onProcReadyRead();
    void onProcFinished(int, QProcess::ExitStatus);
    void onProcError(QProcess::ProcessError);
    void refreshDevices();

    // NEU: echter Slot für den Watchdog
    void onWatchdogTimeout();

private:
    struct Cmd {
        QStringList args;
        bool log = true;
        QString label;
        int timeoutMs = 0; // 0 = kein Timeout
    };

    void enqueue(const QStringList& args, const QString& label = QString(), bool log=true, int timeoutMs=0);
    void runNext();

    QByteArray buildMonolithic2MiB() const;
    QString timestampedDumpName() const;
    QString mxprogPath() const;
    QString selectedDeviceArg() const;
    QString discoverMxprog() const;
    void loadSettings();
    void saveSettings() const;

    void resetProgressTracking();
    void appendSmart(const QString& chunk);

    QWidget*        m_central = nullptr;
    QLineEdit*      m_mxprogEdit = nullptr;

    QComboBox*      m_deviceCombo = nullptr;
    QCheckBox*      m_chkErase = nullptr;
    QCheckBox*      m_chkVerify = nullptr;
    QPlainTextEdit* m_log = nullptr;

    QVector<BankWidget*> m_banks;

    QProcess*   m_proc = nullptr;
    QQueue<Cmd> m_queue;
    bool        m_running = false;

    QProgressBar*      m_progBar = nullptr;
    int                m_progressBlock = -1;
    bool               m_prevWasBlank  = false;
    QRegularExpression m_rePercent{R"(^\d+%$)"};

    QTimer* m_watchdog = nullptr;
};
