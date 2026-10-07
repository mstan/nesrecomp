/* Host-only diagnostics; never reads ROM, RAM, saves or controller input.
 * Windows dumps use a waiting worker thread, so a crashing thread (including
 * stack overflow) need not load libraries or call DbgHelp on its damaged stack.
 * Dumps are best effort: power loss and forced process termination cannot be
 * caught. Keep the matching release PDB for symbolizing the exception address.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "cyc_diagnostics.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <wchar.h>
#include <sys/stat.h>
#include <fcntl.h>
#if NESRECOMP_ENABLE_MODS
#include "../include/mod_runtime.h"
#endif
#ifdef CYC_WITH_RECOMP_UI
#include "recomp_launcher.h"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <process.h>
#define close_fd _close
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#define close_fd close
#endif
#ifndef CYC_BUILD_VERSION
#define CYC_BUILD_VERSION "development"
#endif
#ifndef CYC_BUILD_REVISION
#define CYC_BUILD_REVISION "unknown"
#endif

static FILE *s_log;
static int s_saved_out = -1, s_saved_err = -1;
static bool s_exit_registered, s_exiting, s_err_redirected;
static uint64_t s_frame, s_cycles;
static unsigned s_pc;
static unsigned s_session;
static void (*s_old_abort)(int);

void cyc_diagnostics_note(const char *format, ...)
{
    if (!s_log) return;
    va_list args; va_start(args, format);
    fputs("[diagnostics] ", s_log); vfprintf(s_log, format, args); fputc('\n', s_log);
    va_end(args); fflush(s_log);
}

void cyc_diagnostics_error(const char *format, ...)
{
    va_list args; va_start(args, format);
    if (s_log && !s_err_redirected) {
        va_list copy; va_copy(copy, args);
        vfprintf(s_log, format, copy); va_end(copy); fflush(s_log);
    }
    vfprintf(stderr, format, args); va_end(args); fflush(stderr);
}

#ifdef _WIN32
static HANDLE s_request, s_done, s_worker;
static HMODULE s_dbghelp;
static HANDLE s_log_handle;
static wchar_t s_dump_path[4096];
static volatile LONG s_shutdown, s_crashing;
static EXCEPTION_POINTERS *s_exception;
static DWORD s_exception_thread;
static LPTOP_LEVEL_EXCEPTION_FILTER s_old_filter;
static UINT s_old_error_mode;
typedef BOOL (WINAPI *DumpWriter)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static DumpWriter s_dump_writer;

static DWORD WINAPI dump_worker(void *unused)
{
    (void)unused;
    WaitForSingleObject(s_request, INFINITE);
    if (!s_shutdown) {
        HANDLE dump = CreateFileW(s_dump_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        BOOL ok = FALSE; DWORD error = GetLastError();
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info = { s_exception_thread, s_exception, FALSE };
            if (s_dump_writer) ok = s_dump_writer(GetCurrentProcess(), GetCurrentProcessId(), dump,
                                                 MiniDumpNormal, &info, NULL, NULL);
            error = s_dump_writer ? GetLastError() : ERROR_PROC_NOT_FOUND;
            FlushFileBuffers(dump); CloseHandle(dump);
            if (!ok) DeleteFileW(s_dump_path); /* only the file this worker just created */
        }
        char result[128]; DWORD written;
        int n = snprintf(result, sizeof(result), "[diagnostics] minidump=%s win32_error=%lu\n",
                         ok ? "written" : "failed", (unsigned long)(ok ? 0 : error));
        WriteFile(s_log_handle, result, (DWORD)n, &written, NULL); FlushFileBuffers(s_log_handle);
    }
    SetEvent(s_done); return 0;
}

static LONG WINAPI unhandled_exception(EXCEPTION_POINTERS *exception)
{
    if (InterlockedCompareExchange(&s_crashing, 1, 0)) return EXCEPTION_EXECUTE_HANDLER;
    s_exception = exception; s_exception_thread = GetCurrentThreadId();
    /* Avoid stdio locks: a fault can occur inside a CRT write. */
    char line[256]; DWORD written;
    int n = snprintf(line, sizeof(line),
        "[diagnostics] CRASH exception=0x%08lX address=%p thread=%lu frame=%llu cycles=%llu pc=%04X\n",
        (unsigned long)exception->ExceptionRecord->ExceptionCode,
        exception->ExceptionRecord->ExceptionAddress, (unsigned long)s_exception_thread,
        (unsigned long long)s_frame, (unsigned long long)s_cycles, s_pc);
    WriteFile(s_log_handle, line, (DWORD)n, &written, NULL); FlushFileBuffers(s_log_handle);
    SetEvent(s_request); WaitForSingleObject(s_done, 10000);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void abort_signal(int sig)
{
    (void)sig;
    RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    _exit(3);
}

static bool redirected(FILE *stream)
{
    int fd = _fileno(stream);
    if (fd < 0) return false;
    HANDLE handle = (HANDLE)_get_osfhandle(fd);
    DWORD kind = GetFileType(handle);
    return kind == FILE_TYPE_DISK || kind == FILE_TYPE_PIPE;
}

static void redirect_stream(FILE *stream, int fd, int *saved)
{
    if (redirected(stream)) return; /* preserve an inherited pipe/file */
    if (_fileno(stream) < 0 && !freopen("NUL", "w", stream)) return;
    *saved = _dup(_fileno(stream));
    if (*saved >= 0) {
        if (_dup2(fd, _fileno(stream)) == 0) setvbuf(stream, NULL, _IONBF, 0);
        else { _close(*saved); *saved = -1; }
    }
}

static int open_log(void)
{
    wchar_t root[4096], folder[4096], base[4096], log_path[4096];
    DWORD n = GetModuleFileNameW(NULL, root, 4096);
    if (!n || n >= 4000) return -1;
    wchar_t *slash = wcsrchr(root, L'\\'); if (!slash) return -1; *slash = 0;
    SYSTEMTIME now; GetSystemTime(&now);
    const char *override = getenv("NESRECOMP_DIAGNOSTICS_DIR");
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (override && *override) {
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, override, -1, folder, 4096)) return -1;
        } else if (!attempt) {
            if (swprintf(folder, 4096, L"%ls\\diagnostics", root) < 0) return -1;
        } else {
            n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, 4096);
            if (!n || n >= 3950) return -1;
            if (swprintf(folder, 4096, L"%ls\\NESRecomp", root) < 0) return -1;
            CreateDirectoryW(folder, NULL);
            wcscat(folder, L"\\diagnostics");
        }
        CreateDirectoryW(folder, NULL);
        if (swprintf(base, 4096, L"%ls\\nesrecomp-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u",
            folder, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, (unsigned long)GetCurrentProcessId(), s_session) < 0) return -1;
        if (swprintf(log_path, 4096, L"%ls.log", base) < 0 ||
            swprintf(s_dump_path, 4096, L"%ls.dmp", base) < 0) return -1;
        int fd = _wopen(log_path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0 || (override && *override)) return fd;
    }
    return -1;
}
#else
static int s_signal_fd = -1;
static void abort_signal(int sig)
{
    static const char note[] = "[diagnostics] CRASH signal=SIGABRT\n";
    if (s_signal_fd >= 0) write(s_signal_fd, note, sizeof(note) - 1);
    _exit(128 + sig);
}
static bool redirected(FILE *stream)
{
    struct stat st;
    return fstat(fileno(stream), &st) == 0 && (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
}
static void redirect_stream(FILE *stream, int fd, int *saved)
{
    if (redirected(stream)) return;
    *saved = dup(fileno(stream));
    if (*saved >= 0) {
        if (dup2(fd, fileno(stream)) >= 0) setvbuf(stream, NULL, _IONBF, 0);
        else { close(*saved); *saved = -1; }
    }
}
static int open_log(void)
{
    char root[4096], folder[4096], path[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(root); if (_NSGetExecutablePath(root, &size)) return -1;
#else
    ssize_t n = readlink("/proc/self/exe", root, sizeof(root) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(root) - 1) return -1; root[n] = 0;
#endif
    char *slash = strrchr(root, '/'); if (!slash) return -1; *slash = 0;
    const char *override = getenv("NESRECOMP_DIAGNOSTICS_DIR");
    int nfolder = override && *override ? snprintf(folder, sizeof(folder), "%s", override)
        : snprintf(folder, sizeof(folder), "%s/diagnostics", root);
    if (nfolder < 0 || nfolder >= (int)sizeof(folder)) return -1;
    if (mkdir(folder, 0755) && errno != EEXIST) return -1;
    time_t now = time(NULL); struct tm utc; gmtime_r(&now, &utc);
    int npath = snprintf(path, sizeof(path), "%s/nesrecomp-%04d%02d%02d-%02d%02d%02d-%ld-%u.log", folder,
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, (long)getpid(), s_session);
    if (npath < 0 || npath >= (int)sizeof(path)) return -1;
    return open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
}
#endif

bool cyc_diagnostics_active(void) { return s_log != NULL; }

static void diagnostics_at_exit(void)
{
    if (!s_exiting) cyc_diagnostics_note("process exit without recorded return code");
    cyc_diagnostics_disable();
}

bool cyc_diagnostics_enable(void)
{
    if (s_log) return true;
    ++s_session;
    int fd = open_log();
    if (fd < 0) { fprintf(stderr, "[diagnostics] Cannot create a session log (errno=%d)\n", errno); return false; }
#ifdef _WIN32
    s_log = _fdopen(fd, "w");
#else
    s_log = fdopen(fd, "w");
#endif
    if (!s_log) { close_fd(fd); return false; }
    setvbuf(s_log, NULL, _IONBF, 0);
    s_frame = s_cycles = s_pc = 0;
    s_exiting = false;
    if (!s_exit_registered) { atexit(diagnostics_at_exit); s_exit_registered = true; }
    redirect_stream(stdout, fd, &s_saved_out);
    redirect_stream(stderr, fd, &s_saved_err); s_err_redirected = s_saved_err >= 0;
    cyc_diagnostics_note("session version=%s revision=%s pointer_bits=%u time=UTC backend=cycle",
        CYC_BUILD_VERSION, CYC_BUILD_REVISION, (unsigned)(8 * sizeof(void *)));
#ifdef _WIN32
    s_log_handle = (HANDLE)_get_osfhandle(fd);
    s_shutdown = s_crashing = 0;
    s_dbghelp = LoadLibraryW(L"dbghelp.dll");
    s_dump_writer = s_dbghelp ? (DumpWriter)GetProcAddress(s_dbghelp, "MiniDumpWriteDump") : NULL;
    s_request = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_worker = s_request && s_done ? CreateThread(NULL, 0, dump_worker, NULL, 0, NULL) : NULL;
    if (s_worker) {
        ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee);
        s_old_error_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        s_old_filter = SetUnhandledExceptionFilter(unhandled_exception);
        s_old_abort = signal(SIGABRT, abort_signal);
    }
    cyc_diagnostics_note("Windows pid=%lu minidump_handler=%s dbghelp=%s processors=%lu",
        (unsigned long)GetCurrentProcessId(), s_worker ? "ready" : "unavailable",
        s_dump_writer ? "ready" : "unavailable", (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    typedef LONG (WINAPI *VersionReader)(OSVERSIONINFOW *);
    VersionReader version_reader = (VersionReader)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW version = {0}; version.dwOSVersionInfoSize = sizeof(version);
    if (version_reader && !version_reader(&version))
        cyc_diagnostics_note("Windows version=%lu.%lu build=%lu", (unsigned long)version.dwMajorVersion,
            (unsigned long)version.dwMinorVersion, (unsigned long)version.dwBuildNumber);
    for (DWORD index = 0; index < 8; ++index) {
        DISPLAY_DEVICEW display = {0}; display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(NULL, index, &display, 0)) break;
        char name[512];
        if (WideCharToMultiByte(CP_UTF8, 0, display.DeviceString, -1, name, sizeof(name), NULL, NULL))
            cyc_diagnostics_note("display adapter=%s flags=%lu", name, (unsigned long)display.StateFlags);
    }
#else
    s_signal_fd = fd; s_old_abort = signal(SIGABRT, abort_signal);
    cyc_diagnostics_note("POSIX pid=%ld (Windows minidumps unavailable)", (long)getpid());
#endif
    return true;
}

void cyc_diagnostics_disable(void)
{
    if (!s_log) return;
    cyc_diagnostics_note("diagnostics stopped frame=%llu cycles=%llu pc=%04X",
        (unsigned long long)s_frame, (unsigned long long)s_cycles, s_pc);
#ifdef _WIN32
    if (s_worker) {
        SetUnhandledExceptionFilter(s_old_filter); signal(SIGABRT, s_old_abort);
        SetErrorMode(s_old_error_mode);
        InterlockedExchange(&s_shutdown, 1); SetEvent(s_request); WaitForSingleObject(s_worker, INFINITE);
        CloseHandle(s_worker); s_worker = NULL;
    }
    if (s_request) CloseHandle(s_request); if (s_done) CloseHandle(s_done);
    s_request = s_done = NULL;
    if (s_dbghelp) FreeLibrary(s_dbghelp); s_dbghelp = NULL;
    if (s_saved_out >= 0) { fflush(stdout); _dup2(s_saved_out, _fileno(stdout)); _close(s_saved_out); }
    if (s_saved_err >= 0) { fflush(stderr); _dup2(s_saved_err, _fileno(stderr)); _close(s_saved_err); }
#else
    signal(SIGABRT, s_old_abort); s_signal_fd = -1;
    if (s_saved_out >= 0) { fflush(stdout); dup2(s_saved_out, fileno(stdout)); close(s_saved_out); }
    if (s_saved_err >= 0) { fflush(stderr); dup2(s_saved_err, fileno(stderr)); close(s_saved_err); }
#endif
    s_saved_out = s_saved_err = -1; s_err_redirected = false;
    fclose(s_log); s_log = NULL;
}

void cyc_diagnostics_sync(void)
{
#if NESRECOMP_ENABLE_MODS
    if (nes_mod_feature_selected("nesrecomp.developer.diagnostics", "diagnostics")) cyc_diagnostics_enable();
    else cyc_diagnostics_disable();
#endif
}

#ifdef CYC_WITH_RECOMP_UI
static const RecompLauncherCModProvider *s_provider;
static RecompLauncherCModProvider s_wrapped;
static void log_selection(void)
{
    if (!s_log || !s_provider->feature_count || !s_provider->feature_get) return;
    int count = s_provider->feature_count(s_provider->ctx);
    for (int i = 0; i < count; ++i) {
        RecompLauncherCModFeature feature = {0};
        if (s_provider->feature_get(s_provider->ctx, i, &feature) && feature.enabled)
            cyc_diagnostics_note("enabled mod package=%s version=%s feature=%s", feature.package_id,
                feature.package_version, feature.id);
    }
}
static int enable_feature(void *ctx, const char *package, const char *feature, int enabled)
{
    int ok = s_provider->feature_enable(ctx, package, feature, enabled);
    cyc_diagnostics_sync();
    cyc_diagnostics_note("launcher feature package=%s feature=%s enabled=%d result=%d", package, feature, enabled, ok);
    return ok;
}
static int commit(void *ctx, const char *path)
{
    cyc_diagnostics_sync();
    cyc_diagnostics_note("PLAY: committing selected mods and verifying ROM");
    log_selection();
    int ok = s_provider->commit(ctx, path);
    cyc_diagnostics_note("PLAY commit result=%d%s%s", ok, ok ? "" : " error=",
        !ok && s_provider->last_error ? s_provider->last_error(ctx) : "");
    return ok;
}
#endif

const void *cyc_diagnostics_provider(const void *provider)
{
#ifdef CYC_WITH_RECOMP_UI
    if (!provider) return NULL;
    s_provider = (const RecompLauncherCModProvider *)provider;
    s_wrapped = *s_provider;
    log_selection();
    if (s_provider->feature_enable) s_wrapped.feature_enable = enable_feature;
    if (s_provider->commit) s_wrapped.commit = commit;
    return &s_wrapped;
#else
    return provider;
#endif
}

void cyc_diagnostics_frame(uint64_t frame, uint64_t cycles, unsigned pc)
{
    if (!s_log) return;
    s_frame = frame; s_cycles = cycles; s_pc = pc;
    if (frame == 1 || frame % 300 == 0)
        cyc_diagnostics_note("heartbeat frame=%llu cycles=%llu pc=%04X",
            (unsigned long long)frame, (unsigned long long)cycles, pc);
}

void cyc_diagnostics_exit(int code)
{
    s_exiting = true;
    cyc_diagnostics_note("exit code=%d frame=%llu cycles=%llu pc=%04X", code,
        (unsigned long long)s_frame, (unsigned long long)s_cycles, s_pc);
}

#if NESRECOMP_ENABLE_MODS
static void activate(void) { cyc_diagnostics_enable(); }
NES_MOD_CONSTRUCTOR(register_cycle_diagnostics) {
    nes_mod_register_activation_plugin("nesrecomp.diagnostics", activate);
}
#endif
