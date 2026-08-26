#pragma once

#include <Logger/logger_global.h>

#include <QObject>
#include <QThread>
#include <QAtomicInt>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QString>
#include <QVector>

class LogWorker;

class LOGGER_EXPORT Logger final : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString logDir;
        QString baseFileName = "app.log";

        qint64 rotateBytes = 10 * 1024 * 1024; // 10 MB
        int keepFiles = 20;
        int keepDays  = 7;

        bool alsoStdErr = true;
        bool windowsDebugOutput = true; // OutputDebugString в debug (Windows)
        int  flushIntervalMs = 300;     // flush раз в 300мс при активности

        int  maxQueue = 50000;          // защита от лог-шторма
        int  ringBufferLines = 2000;    // последние N строк для crash-dump

        // crash handler: гарантированно пишет *_crash_last.log при SIGSEGV/Access Violation
        bool enableCrashHandler = true;
        int  crashRingLines = 512;        // сколько последних строк держать для crash handler
        int  crashMaxLineBytes = 1024;    // max bytes per line (UTF-8) в crash handler
    };

    static Logger& instance();

    void start(const Config& cfg);
    void stop();

    // быстрый путь: кладём строку в очередь
    void enqueue(QString line, int severity /*0..4*/);

    static void installQtHandler();
    static void uninstallQtHandler();

    // severity: 0=Debug,1=Info,2=Warn,3=Error,4=Fatal
    enum Severity { Debug=0, Info=1, Warn=2, Error=3, Fatal=4 };

private:
    Logger();
    ~Logger();
    Q_DISABLE_COPY(Logger)

    // producer/consumer queue
    QMutex m_mutex;
    QWaitCondition m_hasData;
    struct Item { QString line; int severity; };
    QQueue<Item> m_queue;

    // ring buffer (только строки; хранится в producer, защищено тем же mutex)
    QVector<QString> m_ring;
    int m_ringPos = 0;
    bool m_ringFilled = false;

    // overflow stats
    quint64 m_dropped = 0;
    qint64  m_lastDropReportMs = 0;

    QAtomicInt m_running {0};

    QThread m_thread;
    LogWorker* m_worker = nullptr;

    // локальная копия конфига (для enqueue, без доступа к LogWorker)
    Config m_cfg;

    friend class LogWorker;
};
