/* XMRig
 * Copyright (c) 2018-2021 SChernykh   <https://github.com/SChernykh>
 * Copyright (c) 2016-2026 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include "hw/msr/Msr.h"
#include "backend/cpu/Cpu.h"
#include "base/io/log/Log.h"
#include "base/kernel/Platform.h"


#include <string>
#include <thread>
#include <vector>
#include <windows.h>


namespace xmrig {


static constexpr const wchar_t *kPawnIoRegistryKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
static constexpr const wchar_t *kPawnIoLibrary     = L"PawnIOLib.dll";


static std::wstring appendPath(std::wstring path, const wchar_t *name)
{
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path += L'\\';
    }

    path += name;

    return path;
}


static std::wstring executableDirectory()
{
    std::vector<wchar_t> path(MAX_PATH);

    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }

        if (length < path.size() - 1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            path.resize(length);
            break;
        }

        path.resize(path.size() * 2);
    }

    const auto pos = path.empty() ? std::wstring::npos : std::wstring(path.data(), path.size()).find_last_of(L"\\/");

    return pos == std::wstring::npos ? std::wstring() : std::wstring(path.data(), pos);
}


static std::wstring pawnIoDirectory()
{
    DWORD size = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPawnIoRegistryKey, L"InstallLocation", flags, nullptr, nullptr, &size) == ERROR_SUCCESS && size > sizeof(wchar_t)) {
        std::vector<wchar_t> path(size / sizeof(wchar_t) + 1);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kPawnIoRegistryKey, L"InstallLocation", flags, nullptr, path.data(), &size) == ERROR_SUCCESS) {
            return path.data();
        }
    }

    const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", nullptr, 0);
    if (length > 1) {
        std::vector<wchar_t> path(length);
        if (GetEnvironmentVariableW(L"ProgramFiles", path.data(), length) > 0) {
            return appendPath(path.data(), L"PawnIO");
        }
    }

    return {};
}


static bool readFile(const std::wstring &path, std::vector<unsigned char> &data)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size;
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= MAXDWORD;
    if (!validSize) {
        CloseHandle(file);
        return false;
    }

    data.resize(static_cast<size_t>(size.QuadPart));

    DWORD bytesRead = 0;
    const bool success = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr) && bytesRead == data.size();
    CloseHandle(file);

    return success;
}


class MsrPrivate
{
public:
    using Open    = HRESULT (STDAPICALLTYPE *)(PHANDLE);
    using Load    = HRESULT (STDAPICALLTYPE *)(HANDLE, const UCHAR *, SIZE_T);
    using Execute = HRESULT (STDAPICALLTYPE *)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
    using Close   = HRESULT (STDAPICALLTYPE *)(HANDLE);

    ~MsrPrivate()
    {
        if (handle && close) {
            close(handle);
        }

        if (library) {
            FreeLibrary(library);
        }
    }

    bool init()
    {
        const auto directory = pawnIoDirectory();
        if (directory.empty()) {
            LOG_WARN("%s " YELLOW_BOLD("PawnIO is not installed; download it from https://pawnio.eu"), Msr::tag());
            return false;
        }

        const auto libraryPath = appendPath(directory, kPawnIoLibrary);
        library = LoadLibraryExW(libraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library) {
            LOG_ERR("%s " RED("failed to load PawnIOLib.dll, error %u"), Msr::tag(), GetLastError());
            return false;
        }

        open    = reinterpret_cast<Open>(GetProcAddress(library, "pawnio_open"));
        load    = reinterpret_cast<Load>(GetProcAddress(library, "pawnio_load"));
        execute = reinterpret_cast<Execute>(GetProcAddress(library, "pawnio_execute"));
        close   = reinterpret_cast<Close>(GetProcAddress(library, "pawnio_close"));

        if (!open || !load || !execute || !close) {
            LOG_ERR("%s " RED_BOLD("installed PawnIOLib.dll has an incompatible API"), Msr::tag());
            return false;
        }

        HRESULT result = open(&handle);
        if (FAILED(result)) {
            LOG_ERR("%s " RED("failed to open PawnIO, HRESULT 0x%08lX"), Msr::tag(), static_cast<unsigned long>(result));
            return false;
        }

        const wchar_t *moduleName = nullptr;
        switch (Cpu::info()->vendor()) {
        case ICpuInfo::VENDOR_INTEL:
            moduleName = L"IntelMSR.bin";
            break;

        case ICpuInfo::VENDOR_AMD:
            moduleName = L"AMDFamily17.bin";
            break;

        default:
            LOG_ERR("%s " RED_BOLD("PawnIO has no MSR module for this CPU vendor"), Msr::tag());
            return false;
        }

        std::vector<unsigned char> module;
        const auto modulePath = appendPath(executableDirectory(), moduleName);
        if (!readFile(modulePath, module)) {
            LOG_ERR("%s " RED("failed to read PawnIO module \"%ls\", error %u"), Msr::tag(), modulePath.c_str(), GetLastError());
            return false;
        }

        result = load(handle, module.data(), module.size());
        if (FAILED(result)) {
            LOG_ERR("%s " RED("failed to load PawnIO module \"%ls\", HRESULT 0x%08lX"), Msr::tag(), moduleName, static_cast<unsigned long>(result));
            return false;
        }

        return true;
    }

    bool read(uint32_t reg, uint64_t &value) const
    {
        const ULONG64 input = reg;
        ULONG64 output      = 0;
        SIZE_T returned     = 0;
        const HRESULT result = execute(handle, "ioctl_read_msr", &input, 1, &output, 1, &returned);

        if (SUCCEEDED(result) && returned == 1) {
            value = output;
            return true;
        }

        return false;
    }

    bool write(uint32_t reg, uint64_t value) const
    {
        const ULONG64 input[2] = { reg, value };
        SIZE_T returned        = 0;

        return SUCCEEDED(execute(handle, "ioctl_write_msr", input, 2, nullptr, 0, &returned));
    }

    HMODULE library = nullptr;
    HANDLE handle   = nullptr;
    Open open       = nullptr;
    Load load       = nullptr;
    Execute execute = nullptr;
    Close close     = nullptr;
    bool available  = false;
};


} // namespace xmrig


xmrig::Msr::Msr() : d_ptr(new MsrPrivate())
{
    d_ptr->available = d_ptr->init();
}


xmrig::Msr::~Msr()
{
    delete d_ptr;
}


bool xmrig::Msr::isAvailable() const
{
    return d_ptr->available;
}


bool xmrig::Msr::write(Callback &&callback)
{
    const auto &units = Cpu::info()->units();
    bool success      = false;

    std::thread thread([&callback, &units, &success]() {
        for (int32_t pu : units) {
            if (!Platform::setThreadAffinity(pu)) {
                continue;
            }

            if (!callback(pu)) {
                return;
            }
        }

        success = true;
    });

    thread.join();

    return success;
}


bool xmrig::Msr::rdmsr(uint32_t reg, int32_t cpu, uint64_t &value) const
{
    assert(cpu < 0);

    return d_ptr->read(reg, value);
}


bool xmrig::Msr::wrmsr(uint32_t reg, uint64_t value, int32_t cpu)
{
    assert(cpu < 0);

    return d_ptr->write(reg, value);
}
