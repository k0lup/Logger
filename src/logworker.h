#pragma once
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QElapsedTimer>
#include <QDate>

#include "Logger/logger.h"

class LogWorker final : public QObject {
    Q_OBJECT
public:
    LogWorker(Logger* owner, Logger::Config cfg);

public slots:
    void run();
    void quit();

private:
    void openFileIfNeeded();
    void rotateIfNeededBySize();
    void rotateIfNeededByDay();
    void doRotate(const QString& reason);
    void enforceRetention();
    QString rotatedName() const;
    QString crashName() const;

    void writeLine(const QString& s);
    void flushIfNeeded(bool force);

#ifdef Q_OS_WIN
    void winDebugOutput(const QString& s);
#endif

    Logger* m_owner = nullptr;
    Logger::Config m_cfg;

    QFile m_file;
    QTextStream m_out;

    bool m_quit = false;
    QElapsedTimer m_flushTimer;
    QDate m_openDay;

    bool m_hadWritesSinceFlush = false;
};
