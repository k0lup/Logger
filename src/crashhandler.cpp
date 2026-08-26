#include "crashhandler.h"

#include <QDir>
#include <QFileInfo>
#include <QByteArray>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef Q_OS_WIN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <signal.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/stat.h>
#endif

CrashHandler& CrashHandler::instance() {
    static CrashHandler inst;
    return inst;
}

QString CrashHandler::crashLastPath() const {
    return m_crashLastPath;
}

static QString crashLastNameFromBase(const QString& baseFileName) {
    QString stem = baseFileName, ext;
    const int dot = baseFileName.lastIndexOf('.');
    if (dot > 0) { stem = baseFileName.left(dot); ext = baseFileName.mid(dot); }
    return QString("%1_crash_last%2").arg(stem, ext);
}

void CrashHandler::buildCrashLastPath() {
    const QString name = crashLastNameFromBase(m_s.baseFileName);
    m_crashLastPath = QDir(m_s.logDir).filePath(name);
}

void CrashHandler::allocRing() {
    freeRing();

    m_ringLines = std::max(0, m_s.ringLines);
    m_maxLineBytes = std::max(128, m_s.maxLineBytes);

    if (m_ringLines <= 0) return;

    m_slots = new Slot[m_ringLines];
    m_storage = new char[(size_t)m_ringLines * (size_t)(m_maxLineBytes + 1)];

    for (int i = 0; i < m_ringLines; ++i) {
        m_slots[i].data = m_storage + (size_t)i * (size_t)(m_maxLineBytes + 1);
        m_slots[i].data[0] = '\0';
        m_slots[i].len.store(0, std::memory_order_relaxed);
    }
    m_seq.store(0, std::memory_order_relaxed);
}

void CrashHandler::freeRing() {
    delete[] m_slots;
    m_slots = nullptr;
    delete[] m_storage;
    m_storage = nullptr;
    m_ringLines = 0;
    m_maxLineBytes = 0;
}

void CrashHandler::install(const Settings& s) {
    m_s = s;
    if (m_s.baseFileName.isEmpty())
        m_s.baseFileName = "app.log";
    if (m_s.logDir.isEmpty())
        m_s.logDir = ".";

    QDir().mkpath(m_s.logDir); // заранее (не в crash handler)
    buildCrashLastPath();
    allocRing();

    if (!m_installed) {
        installPlatformHandlers();
        m_installed = true;
    }
}

void CrashHandler::uninstall() {
    if (!m_installed) return;
    uninstallPlatformHandlers();
    m_installed = false;
    freeRing();
}

void CrashHandler::pushLine(const QString& line) {
    if (!m_installed || !m_slots || m_ringLines <= 0) return;

    QByteArray utf8 = line.toUtf8();
    if (utf8.size() > m_maxLineBytes)
        utf8.truncate(m_maxLineBytes);

    const uint32_t seq = m_seq.fetch_add(1, std::memory_order_relaxed);
    const int idx = (int)(seq % (uint32_t)m_ringLines);
    Slot& slot = m_slots[idx];

    const int n = utf8.size();
    if (n > 0) {
        std::memcpy(slot.data, utf8.constData(), (size_t)n);
    }
    slot.data[n] = '\0';
    slot.len.store((uint32_t)n, std::memory_order_release);
}

#ifndef Q_OS_WIN

// ---------- Linux/Posix ----------

static CrashHandler* g_self = nullptr;
static struct sigaction g_oldActions[NSIG];
static stack_t g_altstack{};
static bool g_hasAltStack = false;

static void crashSigaction(int signo, siginfo_t* info, void* ucontext) {
    CrashHandler::onCrash_Linux(signo, ucontext, info);

    // вызов предыдущего handler (если был), без рекурсии
    if (signo > 0 && signo < NSIG) {
        const auto& old = g_oldActions[signo];
        if (old.sa_flags & SA_SIGINFO) {
            if (old.sa_sigaction && old.sa_sigaction != crashSigaction)
                old.sa_sigaction(signo, info, ucontext);
        } else {
            if (old.sa_handler && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)
                old.sa_handler(signo);
        }
    }

    _exit(128 + signo);
}

void CrashHandler::installPlatformHandlers() {
    g_self = this;

    // alt stack — чтобы handler выжил даже при проблемах со стеком
    const size_t sz = 64 * 1024;
    void* mem = std::malloc(sz);
    if (mem) {
        g_altstack.ss_sp = mem;
        g_altstack.ss_size = sz;
        g_altstack.ss_flags = 0;
        if (sigaltstack(&g_altstack, nullptr) == 0) {
            g_hasAltStack = true;
        }
    }

    const int handledSignals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
    for (int sig : handledSignals) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crashSigaction;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        if (g_hasAltStack) sa.sa_flags |= SA_ONSTACK;

        sigaction(sig, nullptr, &g_oldActions[sig]);
        sigaction(sig, &sa, nullptr);
    }
}

void CrashHandler::uninstallPlatformHandlers() {
    const int handledSignals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
    for (int sig : handledSignals) {
        sigaction(sig, &g_oldActions[sig], nullptr);
    }

    if (g_hasAltStack) {
        stack_t disabled{};
        disabled.ss_flags = SS_DISABLE;
        sigaltstack(&disabled, nullptr);
        std::free(g_altstack.ss_sp);
        g_altstack = {};
        g_hasAltStack = false;
    }
    g_self = nullptr;
}

void CrashHandler::writeAllFd_Public(int fd, const char* buf, size_t n) noexcept {
    while (n > 0) {
        const ssize_t r = ::write(fd, buf, n);
        if (r > 0) {
            buf += (size_t)r;
            n -= (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        break;
    }
}

static void writeCStr(int fd, const char* s) noexcept {
    CrashHandler::writeAllFd_Public(fd, s, std::strlen(s));
}

static void writeUIntHex(int fd, uintptr_t v) noexcept {
    char tmp[32];
    const char* hex = "0123456789abcdef";
    tmp[0] = '0'; tmp[1] = 'x';
    int pos = 2;
    bool started = false;
    for (int i = (int)(sizeof(uintptr_t) * 8 - 4); i >= 0; i -= 4) {
        const int d = (int)((v >> (unsigned)i) & 0xF);
        if (d != 0 || started || i == 0) {
            started = true;
            tmp[pos++] = hex[d];
        }
    }
    tmp[pos++] = '\n';
    CrashHandler::writeAllFd_Public(fd, tmp, (size_t)pos);
}

void CrashHandler::onCrash_Linux(int signo, void*, void* siginfo) noexcept {
    CrashHandler* self = g_self;
    if (!self) return;

    const QByteArray path = self->m_crashLastPath.toUtf8();
    int fd = ::open(path.constData(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) fd = STDERR_FILENO;

    writeCStr(fd, "==== CRASH ====\n");
    writeCStr(fd, "signal=");
    {
        char b[32];
        int n = std::snprintf(b, sizeof(b), "%d\n", signo);
        if (n > 0) writeAllFd_Public(fd, b, (size_t)n);
    }

    if (siginfo) {
        siginfo_t* si = (siginfo_t*)siginfo;
        writeCStr(fd, "addr=");
        writeUIntHex(fd, (uintptr_t)si->si_addr);
    }

    writeCStr(fd, "\n==== LAST LOG LINES ====\n");

    const uint32_t seq = self->m_seq.load(std::memory_order_relaxed);
    const int lines = self->m_ringLines;
    const int take = (lines > 0) ? std::min(lines, 1024) : 0;

    for (int i = take; i > 0; --i) {
        const uint32_t s = seq - (uint32_t)i;
        const int idx = (int)(s % (uint32_t)lines);
        const Slot& slot = self->m_slots[idx];
        const uint32_t len = slot.len.load(std::memory_order_acquire);
        if (len == 0) continue;
        writeAllFd_Public(fd, slot.data, (size_t)len);
        writeCStr(fd, "\n");
    }

    writeCStr(fd, "==== END CRASH LOG ====\n");

    if (fd != STDERR_FILENO) {
        ::fsync(fd);
        ::close(fd);
    }
}

#else

// ---------- Windows ----------

static CrashHandler* g_self = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

void CrashHandler::installPlatformHandlers() {
    g_self = this;
    g_prevFilter = SetUnhandledExceptionFilter(&CrashHandler::sehFilter);
}

void CrashHandler::uninstallPlatformHandlers() {
    SetUnhandledExceptionFilter(g_prevFilter);
    g_prevFilter = nullptr;
    g_self = nullptr;
}

static void writeAllHandle(HANDLE h, const char* buf, size_t n) noexcept {
    while (n > 0) {
        DWORD written = 0;
        const DWORD chunk = (DWORD)std::min<size_t>(n, (1u << 20));
        if (!WriteFile(h, buf, chunk, &written, nullptr) || written == 0)
            break;
        buf += written;
        n -= written;
    }
}

static void writeCStrH(HANDLE h, const char* s) noexcept {
    writeAllHandle(h, s, std::strlen(s));
}

static void writeHexH(HANDLE h, uintptr_t v) noexcept {
    char tmp[32];
    const char* hex = "0123456789abcdef";
    tmp[0]='0'; tmp[1]='x';
    int pos=2;
    bool started=false;
    for (int i=(int)(sizeof(uintptr_t)*8-4); i>=0; i-=4) {
        int d=(int)((v>> (unsigned)i)&0xF);
        if (d!=0 || started || i==0) { started=true; tmp[pos++]=hex[d]; }
    }
    tmp[pos++]='\r'; tmp[pos++]='\n';
    writeAllHandle(h, tmp, (size_t)pos);
}

long __stdcall CrashHandler::sehFilter(_EXCEPTION_POINTERS* ep) {
    CrashHandler* self = g_self;
    if (!self) return EXCEPTION_EXECUTE_HANDLER;

    const std::wstring wpath = self->m_crashLastPath.toStdWString();
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        h = GetStdHandle(STD_ERROR_HANDLE);
    }

    writeCStrH(h, "==== CRASH ====\r\n");
    if (ep && ep->ExceptionRecord) {
        writeCStrH(h, "code=");
        {
            char b[32];
            int n = std::snprintf(b, sizeof(b), "0x%08lx\r\n",
                                  (unsigned long)ep->ExceptionRecord->ExceptionCode);
            if (n > 0) writeAllHandle(h, b, (size_t)n);
        }
        writeCStrH(h, "addr=");
        writeHexH(h, (uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    }

    writeCStrH(h, "\r\n==== LAST LOG LINES ====\r\n");
    const uint32_t seq = self->m_seq.load(std::memory_order_relaxed);
    const int lines = self->m_ringLines;
    const int take = (lines > 0) ? std::min(lines, 1024) : 0;

    for (int i = take; i > 0; --i) {
        const uint32_t s = seq - (uint32_t)i;
        const int idx = (int)(s % (uint32_t)lines);
        const Slot& slot = self->m_slots[idx];
        const uint32_t len = slot.len.load(std::memory_order_acquire);
        if (len == 0) continue;
        writeAllHandle(h, slot.data, (size_t)len);
        writeCStrH(h, "\r\n");
    }
    writeCStrH(h, "==== END CRASH LOG ====\r\n");

    if (h != GetStdHandle(STD_ERROR_HANDLE)) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    if (g_prevFilter) return g_prevFilter(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif
