/*
 * steamwebhelper wrapper for Steam (Windows) under Wine on macOS.
 *
 * Under Wine 11 on macOS, Steam's CEF UI paints black when its browser runs
 * with separate GPU/renderer processes. Steam's own -cef-* switches do not
 * make Chromium collapse into one process, so this wrapper takes the place of
 * steamwebhelper.exe and starts Valve's original (renamed to
 * steamwebhelper_real.exe) with the needed Chromium switches in front of the
 * original arguments.
 *
 * Valve's binary is not modified; it is only renamed. Remove the wrapper by
 * renaming steamwebhelper_real.exe back to steamwebhelper.exe.
 *
 * Build: x86_64-w64-mingw32-clang -municode -mwindows -O2 -o steamwebhelper.exe
 */
#include <windows.h>
#include <wchar.h>
#include <stdlib.h>

static const wchar_t extra_flags[] = L"--disable-gpu --single-process";
static const wchar_t real_name[] = L"steamwebhelper_real.exe";

/* Skip argv[0] in a Windows command line, honouring quotes. */
static const wchar_t *skip_argv0(const wchar_t *cmd)
{
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"')
            cmd++;
        if (*cmd == L'"')
            cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t')
            cmd++;
    }
    while (*cmd == L' ' || *cmd == L'\t')
        cmd++;
    return cmd;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR args, int show)
{
    (void)inst; (void)prev; (void)args; (void)show;
    wchar_t real[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, real, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return 1;
    wchar_t *slash = wcsrchr(real, L'\\');
    if (!slash || (size_t)(slash + 1 - real) + wcslen(real_name) >= MAX_PATH)
        return 1;
    wcscpy(slash + 1, real_name);

    const wchar_t *rest = skip_argv0(GetCommandLineW());
    size_t cap = wcslen(real) + wcslen(extra_flags) + wcslen(rest) + 8;
    wchar_t *cmdline = calloc(cap, sizeof(wchar_t));
    if (!cmdline)
        return 1;
    _snwprintf(cmdline, cap, L"\"%ls\" %ls %ls", real, extra_flags, rest);

    STARTUPINFOW si;
    GetStartupInfoW(&si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(real, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return (int)GetLastError();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
