#include "Service.h"
#include <windows.h>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>

static bool WaitForServiceDeletion(const wchar_t* name, DWORD timeoutMs)
{
    auto deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return false;
        SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
        if (!svc) {
            DWORD err = GetLastError();
            CloseServiceHandle(scm);
            if (err == ERROR_SERVICE_DOES_NOT_EXIST)
                return true; // Deleted
            return false;   // Other error
        }
        // Still exists
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (GetTickCount64() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

static bool InstallService()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        std::wcerr << L"GetModuleFileName failed: " << GetLastError() << L"\n";
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            std::wcerr << L"OpenSCManager failed: Access denied (Run as Administrator).\n";
        else
            std::wcerr << L"OpenSCManager failed: " << err << L"\n";
        return false;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        DataSenderService::ServiceName,
        DataSenderService::ServiceDisplayName,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,          // Or SERVICE_DEMAND_START if you prefer manual start
        SERVICE_ERROR_NORMAL,
        path,
        nullptr, nullptr, nullptr,
        L"NT AUTHORITY\\LocalService",
        nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS)
            std::wcerr << L"Service already exists.\n";
        else if (err == ERROR_SERVICE_MARKED_FOR_DELETE)
            std::wcerr << L"Service is marked for deletion (1072). Fully remove it and retry.\n";
        else
            std::wcerr << L"CreateService failed: " << err << L"\n";
        CloseServiceHandle(scm);
        return false;
    }

    std::wcout << L"Installed successfully. Attempting to start...\n";

    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            std::wcout << L"Service is already running.\n";
        } else {
            std::wcerr << L"StartService failed: " << err << L"\n";
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    } else {
        std::wcout << L"Start command sent.\n";
    }

    // Optionally wait a short period for RUNNING state
    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    for (int i = 0; i < 25; ++i) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&ssp),
                                  sizeof(ssp), &bytesNeeded))
            break;
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            std::wcout << L"Service RUNNING (PID=" << ssp.dwProcessId << L").\n";
            break;
        }
        if (ssp.dwCurrentState == SERVICE_START_PENDING)
            Sleep(200);
        else
            break;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool UninstallService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::wcerr << L"OpenSCManager failed: " << GetLastError() << L"\n";
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, DataSenderService::ServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST)
            std::wcerr << L"Service does not exist.\n";
        else
            std::wcerr << L"OpenService failed: " << err << L"\n";
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        for (int i = 0; i < 50; ++i) {
            if (!QueryServiceStatus(svc, &status)) break;
            if (status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
        }
    }

    if (!DeleteService(svc)) {
        std::wcerr << L"DeleteService failed: " << GetLastError() << L"\n";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return false;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    // Optionally wait for final deletion
    WaitForServiceDeletion(DataSenderService::ServiceName, 5000);

    std::wcout << L"Uninstalled (marked for deletion).\n";
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"-install") == 0) {
            return InstallService() ? 0 : 1;
        }
        if (_wcsicmp(argv[i], L"-uninstall") == 0) {
            return UninstallService() ? 0 : 1;
        }
        if (_wcsicmp(argv[i], L"-console") == 0) {
            std::wcout << L"Console stub.\n";
            return 0;
        }
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)DataSenderService::ServiceName, (LPSERVICE_MAIN_FUNCTIONW)DataSenderService::ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        return GetLastError();
    }
    return 0;
}
