// Windows main-thread instruction-pointer sampler. Diagnostic attribution only:
// thread suspension perturbs execution, so elapsed time is not benchmark evidence.
// Build: g++ -O2 -static tools/host_sample.cpp -o host_sample.exe
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

static bool same_architecture(HANDLE child) {
    using Query = BOOL (WINAPI *)(HANDLE, USHORT*, USHORT*);
    auto address = GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process2");
    Query query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    if (query) {
        USHORT self_process, self_native, child_process, child_native;
        if (!query(GetCurrentProcess(), &self_process, &self_native) ||
            !query(child, &child_process, &child_native)) return false;
        return (self_process ? self_process : self_native) ==
               (child_process ? child_process : child_native);
    }
    BOOL self_wow, child_wow;
    return IsWow64Process(GetCurrentProcess(), &self_wow) &&
           IsWow64Process(child, &child_wow) && self_wow == child_wow;
}

static void stop_owned_child(HANDLE child) {
    // Only the process created below is eligible for termination.
    TerminateProcess(child, 124);
    WaitForSingleObject(child, 5000);
}

static std::string quote(const char* argument) {
    std::string result = "\"";
    unsigned slashes = 0;
    for (const char* p = argument; *p; ++p) {
        if (*p == '\\') { ++slashes; continue; }
        result.append(slashes * (*p == '"' ? 2 : 1), '\\');
        slashes = 0;
        if (*p == '"') result += '\\';
        result += *p;
    }
    result.append(slashes * 2, '\\');
    return result + '"';
}

int main(int argc, char** argv) {
    DWORD timeout_ms = 60000;
    if (argc > 2 && std::string(argv[1]) == "--timeout-ms") {
        char* end = nullptr;
        const unsigned long value = std::strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || !value || value > 3600000) return 2;
        timeout_ms = static_cast<DWORD>(value);
        argv += 2; argc -= 2;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: host_sample [--timeout-ms 1..3600000] samples.csv target.exe [arguments...]\n");
        return 2;
    }
    FILE* output = std::fopen(argv[1], "w");
    if (!output) return 2;
    const std::string child_log = std::string(argv[1]) + ".child.log";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileA(child_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { std::fclose(output); return 2; }
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) { CloseHandle(log); std::fclose(output); return 2; }
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        CloseHandle(log); CloseHandle(input); std::fclose(output); return 3;
    }
    std::string command;
    for (int i = 2; i < argc; ++i) {
        if (i != 2) command += ' ';
        command += quote(argv[i]);
    }
    std::vector<char> buffer(command.begin(), command.end());
    buffer.push_back(0);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log;
    startup.hStdInput = input;
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(argv[2], buffer.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        CloseHandle(job); CloseHandle(log); CloseHandle(input); std::fclose(output); return 3;
    }
    CloseHandle(log); CloseHandle(input);
    if (!AssignProcessToJobObject(job, process.hProcess) || !same_architecture(process.hProcess) ||
        ResumeThread(process.hThread) == DWORD(-1)) {
        std::fprintf(stderr, "Owned child setup failed (job, architecture, or initial resume)\n");
        stop_owned_child(process.hProcess);
        CloseHandle(job); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        std::fclose(output); return 3;
    }
    const ULONGLONG started = GetTickCount64();
    // The process loader needs to publish its executable before Toolhelp can see it.
    Sleep(20);
    MODULEENTRY32 module{};
    module.dwSize = sizeof(module);
    bool identified = false;
    for (unsigned attempt = 0; attempt < 50 && GetTickCount64() - started < timeout_ms; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process.dwProcessId);
        identified = snapshot != INVALID_HANDLE_VALUE && Module32First(snapshot, &module);
        if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        if (identified || WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) break;
        Sleep(10);
    }
    if (!identified) {
        std::fprintf(stderr, "Missing module identity; bounded capture refused\n");
        stop_owned_child(process.hProcess);
        CloseHandle(job); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        std::fclose(output); return 4;
    }
    if (identified) {
        std::fprintf(output, "# module_base=%llx module_size=%lu\n",
                     (unsigned long long)module.modBaseAddr, module.modBaseSize);
        std::fprintf(output, "# module=%s\n", module.szExePath);
    }
    std::map<unsigned long long, unsigned long long> counts;
    unsigned errors = 0;
    bool failed = false;
    for (;;) {
        const DWORD waited = WaitForSingleObject(process.hProcess, 2);
        if (waited == WAIT_OBJECT_0) break;
        if (waited != WAIT_TIMEOUT || GetTickCount64() - started >= timeout_ms) {
            ++errors; failed = true; stop_owned_child(process.hProcess); break;
        }
        if (SuspendThread(process.hThread) == DWORD(-1)) { ++errors; continue; }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(process.hThread, &context)) {
#if defined(_WIN64)
            ++counts[context.Rip];
#else
            ++counts[context.Eip];
#endif
        } else ++errors;
        if (ResumeThread(process.hThread) == DWORD(-1)) {
            ++errors; failed = true; stop_owned_child(process.hProcess); break;
        }
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code == STILL_ACTIVE) {
        ++errors; failed = true; stop_owned_child(process.hProcess); exit_code = 124;
    }
    std::fprintf(output, "# sample_errors=%u child_exit=%lu\n", errors, exit_code);
    for (const auto& item : counts) std::fprintf(output, "%llx,%llu\n", item.first, item.second);
    const bool write_failed = std::ferror(output) != 0;
    const bool close_failed = std::fclose(output) != 0;
    CloseHandle(job); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (write_failed || close_failed) {
        std::fprintf(stderr, "Sample output write/close failed; reject partial capture\n");
        return 6;
    }
    return failed ? 124 : errors ? 5 : exit_code;
}
