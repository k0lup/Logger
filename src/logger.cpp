#include <Logger/logger.h>
#include "logworker.h"
#include "crashhandler.h"

#include <QDateTime>
#include <QThread>
#include <QCoreApplication>

#ifdef Q_OS_WIN
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static QtMessageHandler g_prevHandler = nullptr;

static int toSeverity(QtMsgType t) {
    switch (t) {
        case QtDebugMsg:    return Logger::Debug;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 5, 0))
        case QtInfoMsg:     return Logger::Info;
#endif
        case QtWarningMsg:  return Logger::Warn;
        case QtCriticalMsg: return Logger::Error;
        case QtFatalMsg:    return Logger::Fatal;
    }
    return Logger::Info;
}

static const char* levelToStr(int sev) {
    switch (sev) {
        case Logger::Debug: return "DEBUG";
        case Logger::Info:  return "INFO ";
        case Logger::Warn:  return "WARN ";
        case Logger::Error: return "ERROR";
        case Logger::Fatal: return "FATAL";
    }
    return "UNKWN";
}

static int currentPid() {
#ifdef Q_OS_WIN
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static inline QString normalizeNewlines(QString s) {
    // Приводим CRLF/CR к LF
    s.replace("\r\n", "\n");
    s.replace('\r', '\n');
    return s;
}

static void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const int sev = toSeverity(type);

    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const quintptr tid = (quintptr)QThread::currentThreadId();
    const int pid = currentPid();

    const QString category = ctx.category ? QString::fromLatin1(ctx.category) : QStringLiteral("-");
    const QString file = ctx.file ? QString::fromLatin1(ctx.file) : QStringLiteral("-");
    const QString func = ctx.function ? QString::fromLatin1(ctx.function) : QStringLiteral("-");
    const int line = ctx.line;

    const QString prefix = QString("%1 %2 pid=%3 tid=0x%4 [%5] %6:%7 %8  ")
        .arg(ts,
             QString::fromLatin1(levelToStr(sev)),
             QString::number(pid),
             QString::number((qulonglong)tid, 16),
             category,
             file,
             QString::number(line),
             func);

    QString text = normalizeNewlines(msg);

    // Важно: keepEmptyParts, чтобы пустые строки внутри блока тоже сохранились.
    const QStringList parts = text.split('\n', QString::KeepEmptyParts);

    // Первая строка как обычно, остальные — как продолжения
    for (int i = 0; i < parts.size(); ++i) {
        const QString& p = parts[i];
        const QString lineOut = (i == 0)
            ? (prefix + p)
            : (prefix + QStringLiteral("| ") + p);

        Logger::instance().enqueue(lineOut, sev);
    }

    if (type == QtFatalMsg) {
        if (g_prevHandler) g_prevHandler(type, ctx, msg);
        abort();
    }
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() = default;
Logger::~Logger() { stop(); }

void Logger::start(const Config& cfg) {
    if (m_running.fetchAndStoreOrdered(1) == 1) return;

    // сохраняем конфиг локально (для enqueue)
    m_cfg = cfg;

    // init ring
    {
        QMutexLocker lock(&m_mutex);
        m_ring.clear();
        m_ring.resize(qMax(0, cfg.ringBufferLines));
        m_ringPos = 0;
        m_ringFilled = false;

        m_queue.clear();
        m_dropped = 0;
        m_lastDropReportMs = 0;
    }

    // crash handler (SIGSEGV/SEH): пишет отдельный crash_last файл даже при падении
    if (m_cfg.enableCrashHandler) {
        CrashHandler::Settings cs;
        cs.logDir = m_cfg.logDir;
        cs.baseFileName = m_cfg.baseFileName;
        cs.ringLines = (m_cfg.crashRingLines > 0) ? m_cfg.crashRingLines : m_cfg.ringBufferLines;
        cs.maxLineBytes = m_cfg.crashMaxLineBytes;
        cs.alsoStdErr = m_cfg.alsoStdErr;
        CrashHandler::instance().install(cs);
    }

    m_worker = new LogWorker(this, cfg);
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::started, m_worker, &LogWorker::run);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_thread.start();
}

void Logger::stop() {
    if (m_running.fetchAndStoreOrdered(0) == 0) return;

    {
        QMutexLocker lock(&m_mutex);
        m_hasData.wakeAll();
    }

    if (QThread::currentThread() == &m_thread) {
        m_thread.quit();
        CrashHandler::instance().uninstall();
        return;
    }

    m_thread.quit();   // <— важно: остановить event loop QThread
    m_thread.wait();
    m_worker = nullptr;
}
void Logger::enqueue(QString line, int severity) {
    if (m_running.loadAcquire() == 0) return;

    QMutexLocker lock(&m_mutex);

    // 1) ring buffer
    if (!m_ring.isEmpty()) {
        m_ring[m_ringPos] = line;
        m_ringPos = (m_ringPos + 1) % m_ring.size();
        if (m_ringPos == 0) m_ringFilled = true;
    }

    // 2) overflow policy
    const bool isImportant = (severity >= Error);
    const int maxQueue = m_cfg.maxQueue;
    if (maxQueue > 0 && m_queue.size() >= maxQueue) {
        if (isImportant) {
            // важные сообщения кладём, вытесняя "хвост" debug/info если надо
            // (простая стратегия: если переполнено — удалим один элемент с начала)
            if (!m_queue.isEmpty()) m_queue.dequeue();
            m_queue.enqueue({std::move(line), severity});
        } else {
            // дропаем и считаем
            m_dropped++;

            // раз в ~1с добавим одну строку о дропах (если есть место)
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - m_lastDropReportMs >= 1000 && m_queue.size() < maxQueue) {
                m_lastDropReportMs = nowMs;
                const QString warn = QString("%1 WARN  pid=%2 tid=0x%3 [app.logger] -:- -  LOG OVERFLOW: dropped=%4")
                    .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"))
                    .arg(currentPid())
                    .arg(QString::number((qulonglong)(quintptr)QThread::currentThreadId(), 16))
                    .arg(m_dropped);
                m_queue.enqueue({warn, Warn});
            }
            return;
        }
    } else {
        m_queue.enqueue({std::move(line), severity});
    }

    m_hasData.wakeOne();
}

void Logger::installQtHandler() {
    if (!g_prevHandler) {
        g_prevHandler = qInstallMessageHandler(qtMessageHandler);
    }
}

void Logger::uninstallQtHandler() {
    if (g_prevHandler) {
        qInstallMessageHandler(g_prevHandler);
        g_prevHandler = nullptr;
    }
}
