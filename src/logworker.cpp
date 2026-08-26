#include "logworker.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>

#ifdef Q_OS_WIN
  #include <windows.h>
#endif

static QString nowTsFileSafe() {
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
}

static QString crashLastNameFromBase(const QString& baseFileName) {
    QString stem = baseFileName, ext;
    const int dot = baseFileName.lastIndexOf('.');
    if (dot > 0) { stem = baseFileName.left(dot); ext = baseFileName.mid(dot); }
    return QString("%1_crash_last%2").arg(stem, ext);
}

LogWorker::LogWorker(Logger* owner, Logger::Config cfg)
    : m_owner(owner), m_cfg(std::move(cfg)) {}

void LogWorker::quit() { m_quit = true; }

void LogWorker::openFileIfNeeded() {
    if (m_file.isOpen()) return;

    QDir().mkpath(m_cfg.logDir);

    // Если в прошлый раз было падение, crash handler оставил *_crash_last.log.
    // Сохраним его под уникальным именем (с timestamp) при следующем запуске.
    {
        QDir dir(m_cfg.logDir);
        const QString last = dir.filePath(crashLastNameFromBase(m_cfg.baseFileName));
        if (QFileInfo::exists(last)) {
            const QString archived = dir.filePath(crashName());
            QFile::rename(last, archived);
        }
    }

    const QString path = QDir(m_cfg.logDir).filePath(m_cfg.baseFileName);

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return; // не падаем
    }
    m_out.setDevice(&m_file);
    m_out.setCodec("UTF-8");

    m_openDay = QDate::currentDate();
    m_flushTimer.restart();
}

QString LogWorker::rotatedName() const {
    // app_YYYYMMDD_HHMMSS.log
    const QString base = m_cfg.baseFileName;
    QString stem = base, ext;
    const int dot = base.lastIndexOf('.');
    if (dot > 0) { stem = base.left(dot); ext = base.mid(dot); }
    return QString("%1_%2%3").arg(stem, nowTsFileSafe(), ext);
}

QString LogWorker::crashName() const {
    // app_crash_YYYYMMDD_HHMMSS.log
    const QString base = m_cfg.baseFileName;
    QString stem = base, ext;
    const int dot = base.lastIndexOf('.');
    if (dot > 0) { stem = base.left(dot); ext = base.mid(dot); }
    return QString("%1_crash_%2%3").arg(stem, nowTsFileSafe(), ext);
}

void LogWorker::writeLine(const QString& s) {
    if (m_file.isOpen()) {
        m_out << s << '\n';
        m_hadWritesSinceFlush = true;
    }
    if (m_cfg.alsoStdErr) {
        fprintf(stderr, "%s\n", s.toLocal8Bit().constData());
    }
#ifdef Q_OS_WIN
    if (m_cfg.windowsDebugOutput) {
        winDebugOutput(s);
    }
#endif
}

#ifdef Q_OS_WIN
void LogWorker::winDebugOutput(const QString& s) {
#ifndef NDEBUG
    const std::wstring ws = (s + "\n").toStdWString();
    OutputDebugStringW(ws.c_str());
#else
    Q_UNUSED(s);
#endif
}
#endif

void LogWorker::flushIfNeeded(bool force) {
    if (!m_file.isOpen()) return;

    if (force) {
        m_out.flush();
        m_file.flush();
        m_hadWritesSinceFlush = false;
        m_flushTimer.restart();
        return;
    }

    if (!m_hadWritesSinceFlush) return;
    if (m_flushTimer.elapsed() >= m_cfg.flushIntervalMs) {
        m_out.flush();
        m_file.flush();
        m_hadWritesSinceFlush = false;
        m_flushTimer.restart();
    }
}

void LogWorker::doRotate(const QString& reason) {
    if (!m_file.isOpen()) return;

    // пометка в лог
    writeLine(QString("---- ROTATE (%1) ----").arg(reason));
    flushIfNeeded(true);

    m_file.close();

    QDir dir(m_cfg.logDir);
    const QString currentPath = dir.filePath(m_cfg.baseFileName);
    const QString rotatedPath = dir.filePath(rotatedName());
    QFile::rename(currentPath, rotatedPath);

    enforceRetention();
    openFileIfNeeded();
}

void LogWorker::rotateIfNeededBySize() {
    if (!m_file.isOpen()) return;
    if (m_cfg.rotateBytes > 0 && m_file.size() >= m_cfg.rotateBytes) {
        doRotate("size");
    }
}

void LogWorker::rotateIfNeededByDay() {
    if (!m_file.isOpen()) return;
    const QDate today = QDate::currentDate();
    if (today != m_openDay) {
        doRotate("day");
    }
}

void LogWorker::enforceRetention() {
    QDir dir(m_cfg.logDir);

    // базовые элементы имени
    const QString base = m_cfg.baseFileName;
    QString stem = base, ext;
    const int dot = base.lastIndexOf('.');
    if (dot > 0) { stem = base.left(dot); ext = base.mid(dot); }

    // rotated: stem_*.ext
    const QString rotatedPattern = QString("%1_*%2").arg(stem, ext);
    // crash: stem_crash_*.ext
    const QString crashPattern   = QString("%1_crash_*%2").arg(stem, ext);

    auto files = dir.entryInfoList({rotatedPattern, crashPattern}, QDir::Files, QDir::Time); // новые сверху

    // Удаляем по возрасту (keepDays)
    if (m_cfg.keepDays > 0) {
        const QDateTime cutoff = QDateTime::currentDateTime().addDays(-m_cfg.keepDays);
        for (const auto& fi : files) {
            // берём lastModified (кроссплатформенно)
            if (fi.lastModified() < cutoff) {
                QFile::remove(fi.absoluteFilePath());
            }
        }
        // перечитаем
        files = dir.entryInfoList({rotatedPattern, crashPattern}, QDir::Files, QDir::Time);
    }

    // Удаляем по количеству (keepFiles)
    if (m_cfg.keepFiles > 0 && files.size() > m_cfg.keepFiles) {
        for (int i = m_cfg.keepFiles; i < files.size(); ++i) {
            QFile::remove(files[i].absoluteFilePath());
        }
    }
}

void LogWorker::run() {
    openFileIfNeeded();
    writeLine("==== LOG START ====");
    flushIfNeeded(true);

    while (!m_quit && m_owner->m_running.loadAcquire() != 0) {
        // пакетно вытаскиваем данные
        QVector<Logger::Item> batch;
        batch.reserve(2048);

        {
            QMutexLocker lock(&m_owner->m_mutex);
            if (m_owner->m_queue.isEmpty()) {
                m_owner->m_hasData.wait(&m_owner->m_mutex, 200);
            }
            while (!m_owner->m_queue.isEmpty() && batch.size() < 2048) {
                batch.push_back(std::move(m_owner->m_queue.dequeue()));
            }
        }

        if (!batch.isEmpty()) {
            rotateIfNeededByDay();

            for (const auto& it : batch) {
                writeLine(it.line);

                if (it.line.contains(QStringLiteral("[logCore]"))) {
                    flushIfNeeded(true);
                }

                // при Error/Fatal — форс-флаш и crash-dump (ring buffer)
                if (it.severity >= Logger::Error) {
                    // пишем отдельный crash файл с контекстом
                    QDir().mkpath(m_cfg.logDir);
                    QFile crash(QDir(m_cfg.logDir).filePath(crashName()));
                    if (crash.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream cs(&crash);
                        cs.setCodec("UTF-8");
                        cs << "==== CRASH CONTEXT ====\n";
                        cs << "Triggered by severity=" << it.severity << "\n\n";

                        // снимаем ring buffer атомарно под mutex
                        QVector<QString> ringSnapshot;
                        {
                            QMutexLocker lock(&m_owner->m_mutex);
                            const int n = m_owner->m_ring.size();
                            ringSnapshot.reserve(n);

                            if (n > 0) {
                                if (m_owner->m_ringFilled) {
                                    for (int i = 0; i < n; ++i) {
                                        int idx = (m_owner->m_ringPos + i) % n;
                                        ringSnapshot.push_back(m_owner->m_ring[idx]);
                                    }
                                } else {
                                    for (int i = 0; i < m_owner->m_ringPos; ++i)
                                        ringSnapshot.push_back(m_owner->m_ring[i]);
                                }
                            }
                        }

                        for (const auto& s : ringSnapshot) cs << s << "\n";
                        cs << "\n==== LAST MESSAGE ====\n" << it.line << "\n";
                        cs.flush();
                        crash.flush();
                        crash.close();
                    }

                    flushIfNeeded(true);
                }
            }

            flushIfNeeded(false);
            rotateIfNeededBySize();
        } else {
            // нет данных — всё равно иногда флашнем
            flushIfNeeded(false);
        }
    }

    writeLine("==== LOG STOP ====");
    flushIfNeeded(true);

    if (m_file.isOpen()) {
        m_file.close();
    }
}
