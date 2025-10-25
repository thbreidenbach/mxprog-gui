#include "MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#include <QDateTime>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QtSerialPort/QSerialPortInfo>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QFontDatabase>
#include <QTextOption>
#include <QStatusBar>

static const int SLOT_SIZE   = 512 * 1024;
static const int TOTAL_BYTES = 2048 * 1024;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_central = new QWidget(this);
    setCentralWidget(m_central);

    auto* v = new QVBoxLayout(m_central);

    // --- mxprog path (Autodiscover + Browse + persist) ---
    auto* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel("mxprog path:"));
    m_mxprogEdit = new QLineEdit(this);
    m_mxprogEdit->setPlaceholderText("autodetected mxprog path…");
    pathLayout->addWidget(m_mxprogEdit, 1);
    auto* btnBrowse = new QPushButton("Browse…", this);
    pathLayout->addWidget(btnBrowse);
    v->addLayout(pathLayout);

    connect(btnBrowse, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, "Select mxprog", "/usr/local/bin", "Executables (*)");
        if (!file.isEmpty()) { m_mxprogEdit->setText(file); saveSettings(); }
    });

    // Top: Device & options
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel("Device:"));
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setMinimumWidth(320);
    top->addWidget(m_deviceCombo, 1);
    auto* btnRefresh = new QPushButton("Refresh", this);
    top->addWidget(btnRefresh);
    m_chkErase  = new QCheckBox("Erase first (-e)", this);
    m_chkVerify = new QCheckBox("Verify after", this);
    m_chkErase->setChecked(true);
    m_chkVerify->setChecked(true);
    top->addWidget(m_chkErase);
    top->addWidget(m_chkVerify);
    v->addLayout(top);

    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    refreshDevices();

    loadSettings();
    if (m_mxprogEdit->text().isEmpty()) {
        m_mxprogEdit->setText(discoverMxprog());
    }

    // ROM Bar: 1 Zeile, horizontal scrollbar
    auto* barFrame = new QFrame(this);
    auto* barLayout = new QHBoxLayout(barFrame);
    barLayout->setContentsMargins(0,0,0,0);
    barLayout->setSpacing(8);

    for (int i=0;i<4;++i) {
        auto* b = new BankWidget(i, barFrame);
        barLayout->addWidget(b);
        m_banks.push_back(b);
        connect(b, &BankWidget::requestWriteSlot, this, &MainWindow::writeSlot);
        connect(b, &BankWidget::log, [this](const QString& s){ m_log->appendPlainText(s); });
    }
    auto* barInner = new QWidget();
    barInner->setLayout(barLayout);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(barInner);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(300);
    v->addWidget(scroll);

    // Global actions BELOW bar
    auto* actions = new QHBoxLayout();
    auto* btnWriteAll = new QPushButton("Write All (monolithic)", this);
    auto* btnSaveAll  = new QPushButton("Save 2 MiB Buffer…", this);
    actions->addWidget(btnSaveAll);
    actions->addStretch();
    actions->addWidget(btnWriteAll);
    v->addLayout(actions);

    connect(btnWriteAll, &QPushButton::clicked, this, &MainWindow::writeAllMonolithic);
    connect(btnSaveAll,  &QPushButton::clicked, this, &MainWindow::saveMonolithic);

    // Device actions
    auto* devBox = new QHBoxLayout();
    auto* btnIdentify = new QPushButton("Identify (-i)", this);
    auto* btnErase    = new QPushButton("Erase (-e)", this);
    auto* btnRead     = new QPushButton("Read (-r)", this);
    auto* btnTerm     = new QPushButton("Terminal (-t)", this);
    devBox->addWidget(btnIdentify);
    devBox->addWidget(btnErase);
    devBox->addWidget(btnRead);
    devBox->addWidget(btnTerm);
    devBox->addStretch();
    v->addLayout(devBox);

    connect(btnIdentify, &QPushButton::clicked, this, &MainWindow::identify);
    connect(btnErase,    &QPushButton::clicked, this, &MainWindow::erase);
    connect(btnRead,     &QPushButton::clicked, this, &MainWindow::readDump);
    connect(btnTerm,     &QPushButton::clicked, this, &MainWindow::terminal);

    // Log
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(200);
    m_log->setMaximumBlockCount(5000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setWordWrapMode(QTextOption::NoWrap);
    v->addWidget(m_log, 1);

    // Statusbar + Progress
    m_progBar = new QProgressBar(this);
    m_progBar->setRange(0,100);
    m_progBar->setValue(0);
    statusBar()->addPermanentWidget(m_progBar, 0);

    // Watchdog: EINMAL verbinden (keine unique-connection-Warnung)
    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, &MainWindow::onWatchdogTimeout);
}

void MainWindow::refreshDevices() {
    m_deviceCombo->clear();
    m_deviceCombo->addItem("Auto (no -d)");
    for (const auto& info : QSerialPortInfo::availablePorts()) {
        m_deviceCombo->addItem(info.systemLocation());
    }
#ifdef Q_OS_MAC
    QDir dev("/dev");
    QStringList patterns = { "cu.usbmodem*", "cu.usbserial*" };
    for (const auto& pat : patterns) {
        auto list = dev.entryList(QStringList(pat), QDir::System | QDir::Readable | QDir::Files);
        for (const auto& n : list) {
            QString full = "/dev/" + n;
            if (m_deviceCombo->findText(full) < 0) m_deviceCombo->addItem(full);
        }
    }
#endif
#ifdef Q_OS_LINUX
    QDir dev("/dev");
    for (const auto& pat : QStringList{ "ttyACM*", "ttyUSB*" }) {
        auto list = dev.entryList(QStringList(pat), QDir::System | QDir::Readable | QDir::Files);
        for (const auto& n : list) {
            QString full = "/dev/" + n;
            if (m_deviceCombo->findText(full) < 0) m_deviceCombo->addItem(full);
        }
    }
#endif
}

QString MainWindow::selectedDeviceArg() const {
    QString dev = m_deviceCombo->currentText();
    if (dev.startsWith("Auto")) return QString();
    return QStringList{"-d", dev}.join(' ');
}

QString MainWindow::discoverMxprog() const {
    QStringList paths = QString::fromLocal8Bit(qgetenv("PATH")).split(':', Qt::SkipEmptyParts);
#ifdef Q_OS_MAC
    paths << "/usr/local/bin" << "/opt/homebrew/bin";
#endif
#ifdef Q_OS_LINUX
    paths << "/usr/bin" << "/usr/local/bin";
#endif
    for (const QString& p : paths) {
        QString cand = QDir(p).filePath("mxprog");
        QFileInfo fi(cand);
        if (fi.exists() && fi.isExecutable()) return fi.absoluteFilePath();
    }
    return QString();
}

void MainWindow::loadSettings() {
    QSettings s("mxprog_gui", "mxprog_qt");
    m_mxprogEdit->setText(s.value("mxprog_path").toString());
}
void MainWindow::saveSettings() const {
    QSettings s("mxprog_gui", "mxprog_qt");
    s.setValue("mxprog_path", m_mxprogEdit->text().trimmed());
}

QString MainWindow::mxprogPath() const {
    QString p = m_mxprogEdit ? m_mxprogEdit->text().trimmed() : QString();
    if (p.isEmpty()) p = const_cast<MainWindow*>(this)->discoverMxprog();
    return p.isEmpty() ? "mxprog" : p;
}

void MainWindow::resetProgressTracking() {
    m_progressBlock = -1;
    m_prevWasBlank  = false;
    if (m_progBar) m_progBar->setValue(0);
}

void MainWindow::appendSmart(const QString& chunk) {
    QString data = chunk;
    data.replace("\r\n", "\n");
    const QStringList lines = data.split('\n', Qt::KeepEmptyParts);

    for (const QString& rawLine : lines) {
        QString line = rawLine;
        int cr = line.lastIndexOf('\r');
        if (cr >= 0) line = line.mid(cr + 1);

        // Prozent erkennen, Statusbar updaten, Log nicht fluten
        auto m = m_rePercent.match(line);
        if (m.hasMatch()) {
            bool ok=false; int val = line.left(line.size()-1).toInt(&ok);
            if (ok && m_progBar) m_progBar->setValue(qBound(0, val, 100));
            m_prevWasBlank = false;
            continue;
        }

        const bool isBlank = line.trimmed().isEmpty();
        if (isBlank) {
            if (m_prevWasBlank) continue;
            m_prevWasBlank = true;
        } else {
            m_prevWasBlank = false;
        }
        m_log->appendPlainText(line);
    }
    m_log->ensureCursorVisible();
}

void MainWindow::onProcReadyRead() {
    if (!m_proc) return;
    const QString out = QString::fromUtf8(m_proc->readAllStandardOutput());
    const QString err = QString::fromUtf8(m_proc->readAllStandardError());
    if (!out.isEmpty()) appendSmart(out);
    if (!err.isEmpty()) appendSmart(err);
}

void MainWindow::onProcFinished(int code, QProcess::ExitStatus st) {
    m_watchdog->stop();

    if (st == QProcess::NormalExit && code == 0) {
        m_log->appendPlainText("Command completed successfully.");
        m_running = false;
        runNext();
    } else {
        m_log->appendPlainText(QString("Command failed (exit=%1, status=%2). Aborting queue.")
                               .arg(code).arg(st == QProcess::NormalExit ? "Normal" : "Crashed"));
        m_queue.clear();
        m_running = false;
        resetProgressTracking();
    }
}

void MainWindow::onProcError(QProcess::ProcessError e) {
    QString why;
    switch (e) {
    case QProcess::FailedToStart: why = "FailedToStart (program not found or not executable)"; break;
    case QProcess::Crashed:       why = "Crashed"; break;
    case QProcess::Timedout:      why = "Timedout"; break;
    case QProcess::WriteError:    why = "WriteError"; break;
    case QProcess::ReadError:     why = "ReadError"; break;
    default:                      why = "UnknownError"; break;
    }
    m_log->appendPlainText("QProcess error: " + why + (m_proc ? " — " + m_proc->errorString() : ""));
    // Weiter zur nächsten Queue-Aufgabe (oder hier abbrechen)
    m_running = false;
    runNext();
}

void MainWindow::onWatchdogTimeout() {
    m_log->appendPlainText("Watchdog: Timed out. Killing process and clearing queue.");
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
    }
    m_queue.clear();
    m_running = false;
    resetProgressTracking();
}

void MainWindow::enqueue(const QStringList& args, const QString& label, bool log, int timeoutMs) {
    QStringList realArgs;
    QString devArg = selectedDeviceArg();
    if (!devArg.isEmpty()) {
        auto parts = devArg.split(' ');
        realArgs << parts; // "-d" "<device>"
    }
    realArgs << args;

    Cmd c; c.args = realArgs; c.log = log; c.label = label; c.timeoutMs = timeoutMs;
    m_queue.enqueue(c);

    if (!m_running) runNext();
}

void MainWindow::runNext() {
    if (m_running || m_queue.isEmpty()) return;

    m_running = true;
    Cmd c = m_queue.dequeue();

    if (m_proc) { m_proc->deleteLater(); m_proc = nullptr; }
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &MainWindow::onProcReadyRead);
    connect(m_proc, &QProcess::readyReadStandardError,  this, &MainWindow::onProcReadyRead);
    connect(m_proc, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::onProcFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &MainWindow::onProcError);
    connect(m_proc, &QProcess::started, this, [this](){
        m_log->appendPlainText("Process started.");
        resetProgressTracking();
    });

    const QString prog = mxprogPath();
    if (c.log) m_log->appendPlainText("$ " + prog + " " + c.args.join(' '));

    // PATH ergänzen 
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_MAC
    {
        QString path = env.value("PATH");
        if (!path.contains("/usr/local/bin"))    path += ":/usr/local/bin";
        if (!path.contains("/opt/homebrew/bin")) path += ":/opt/homebrew/bin";
        env.insert("PATH", path);
    }
#endif
#ifdef Q_OS_LINUX
    {
        QString path = env.value("PATH");
        if (!path.contains("/usr/local/bin")) path += ":/usr/local/bin";
        env.insert("PATH", path);
    }
#endif
    m_proc->setProcessEnvironment(env);

    m_proc->setProgram(prog);
    m_proc->setArguments(c.args);
    m_proc->start();

    // Timeout-Überwachung (0 = aus) – nur Intervall + Start, kein mehrfaches connect
    m_watchdog->stop();
    if (c.timeoutMs > 0) {
        m_watchdog->setInterval(c.timeoutMs);
        m_watchdog->start();
    }
}

QByteArray MainWindow::buildMonolithic2MiB() const {
    QByteArray out; out.reserve(TOTAL_BYTES);
    for (auto* b : m_banks) out.append(b->buildTiled512k());
    if (out.size() < TOTAL_BYTES) out.append(QByteArray(TOTAL_BYTES - out.size(), char(0xff)));
    return out.left(TOTAL_BYTES);
}

QString MainWindow::timestampedDumpName() const {
    return QString("romdump_%1.bin").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
}

// ---------- Actions ----------

void MainWindow::writeSlot(int bank, const QByteArray& img512k) {
    // Tempfile
    QString tmpPath = QDir::temp().filePath(QString("slot%1_512k.bin").arg(bank));
    QFile f(tmpPath);
    if (!f.open(QIODevice::WriteOnly)) {
        m_log->appendPlainText("Temp file creation failed: " + tmpPath);
        return;
    }
    f.write(img512k);
    f.close();

    // Beispiel-Timeouts / Passt bei mir
    const int tEraseMs  =  90'000;
    const int tWriteMs  = 240'000;
    const int tVerifyMs = 120'000;

    if (m_chkErase->isChecked()) {
        enqueue(QStringList() << "-y" << "-e", "erase", true, tEraseMs);
    }
    enqueue(QStringList() << "-b" << QString::number(bank) << "-w" << tmpPath, "write", true, tWriteMs);
    if (m_chkVerify->isChecked()) {
        enqueue(QStringList() << "-b" << QString::number(bank) << "-v" << tmpPath, "verify", true, tVerifyMs);
    }
}

void MainWindow::writeAllMonolithic() {
    QByteArray blob = buildMonolithic2MiB();

    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString path = QDir(docs.isEmpty() ? QDir::homePath() : docs).filePath(timestampedDumpName());

    auto trySave = [&](const QString& p)->bool{
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(blob);
        f.close();
        m_log->appendPlainText("Saved 2 MiB buffer to: " + p);
        return true;
    };

    bool saved = trySave(path);
    if (!saved) {
        const QString tmp = QDir::temp().filePath(timestampedDumpName());
        if (trySave(tmp)) {
            path = tmp; saved = true;
            m_log->appendPlainText("Primary save failed, used temp path instead.");
        } else {
            m_log->appendPlainText("Save failed in Documents and Temp. Will program from a temp path without persisting.");
            QString hardTmp = QDir::temp().filePath("romdump_fallback.bin");
            QFile f(hardTmp);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(blob); f.close();
                path = hardTmp; saved = true;
            } else {
                QMessageBox::critical(this, "Save failed", "Could not create any buffer file for programming.");
                return;
            }
        }
    }

    // Beispiel-Timeouts s.o
    const int tEraseMs  = 120'000;
    const int tWriteMs  = 300'000;
    const int tVerifyMs = 180'000;

    if (m_chkErase->isChecked()) {
        enqueue(QStringList() << "-y" << "-e", "erase", true, tEraseMs);
    }
    // ohne -b (Alle Bänke ab Adresse 0)
    enqueue(QStringList() << "-w" << path, "write-all", true, tWriteMs);
    if (m_chkVerify->isChecked()) {
        enqueue(QStringList() << "-v" << path, "verify-all", true, tVerifyMs);
    }
}

void MainWindow::saveMonolithic() {
    QByteArray blob = buildMonolithic2MiB();
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString defDir = docs.isEmpty() ? QDir::homePath() : docs;
    const QString defName = QDir(defDir).filePath(timestampedDumpName());

    QString path = QFileDialog::getSaveFileName(this, "Save 2 MiB Buffer", defName,
                                                "Binary (*.bin);;All (*.*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        m_log->appendPlainText("Save failed: " + path);
        QMessageBox::warning(this, "Save failed", path);
        return;
    }
    f.write(blob);
    f.close();
    m_log->appendPlainText("Saved 2 MiB buffer to: " + path);
}

void MainWindow::identify() { enqueue(QStringList() << "-i", "identify", true, 15'000); }
void MainWindow::erase()     { enqueue(QStringList() << "-y" << "-e", "erase", true, 120'000); }
void MainWindow::readDump() {
    QString path = QFileDialog::getSaveFileName(this, "Read EEPROM to file", "eeprom_dump.bin",
                                                "Binary (*.bin);;All (*.*)");
    if (path.isEmpty()) return;
    enqueue(QStringList() << "-r" << path << "-l" << QString::number(TOTAL_BYTES), "read", true, 240'000);
}
void MainWindow::terminal()  { enqueue(QStringList() << "-t", "term", true, 0); /* kein Timeout im Terminal */ }
