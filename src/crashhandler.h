#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

#include <QString>

// Кроссплатформенный аварийный логгер (Windows/Linux).
// Идея:
//  1) В обычном потоке держим lock-free ring buffer последних N строк (UTF-8, фикс. размер).
//  2) В момент падения (SIGSEGV/SEH) пишем crash-файл через низкоуровневые API.
//     Никаких Qt/heap/mutex в crash handler.
class CrashHandler final {
public:
    struct Settings {
        QString logDir;
        QString baseFileName;      // "app.log" -> "<stem>_crash_last.log"
        int ringLines = 512;       // сколько последних строк хранить
        int maxLineBytes = 1024;   // max bytes per line (UTF-8), с обрезкой
        bool alsoStdErr = true;
    };

    static CrashHandler& instance();

    void install(const Settings& s);
    void uninstall();

    // Добавляет строку в ring buffer (UTF-8). Вызывать из обычного кода, НЕ из crash handler.
    void pushLine(const QString& line);

    QString crashLastPath() const;

#ifndef Q_OS_WIN
    static void onCrash_Linux(int signo, void* ucontext, void* siginfo) noexcept;
    static void writeAllFd_Public(int fd, const char* buf, size_t n) noexcept;
#endif
private:
    CrashHandler() = default;
    ~CrashHandler() = default;
    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    struct Slot {
        std::atomic<uint32_t> len {0};
        char* data = nullptr;
    };

    void allocRing();
    void freeRing();

    void buildCrashLastPath();
    void installPlatformHandlers();
    void uninstallPlatformHandlers();

    //static void writeCrashReport_Linux(int signo, void* ucontext, void* siginfo);
#ifdef Q_OS_WIN
    static long __stdcall sehFilter(struct _EXCEPTION_POINTERS* ep);
#endif

    //static void writeAllFd(int fd, const char* buf, size_t n) noexcept;

    Settings m_s;
    bool m_installed = false;

    std::atomic<uint32_t> m_seq {0};
    Slot* m_slots = nullptr;
    int m_ringLines = 0;
    int m_maxLineBytes = 0;
    char* m_storage = nullptr;

    QString m_crashLastPath;
};
