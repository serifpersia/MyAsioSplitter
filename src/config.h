#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct AsioDriverInfo {
    std::wstring name;
    CLSID clsid;
};

// Enumerates HKLM\SOFTWARE\ASIO, excluding the two CLSIDs owned by this DLL.
std::vector<AsioDriverInfo> EnumerateAsioDrivers(REFCLSID exclude1, REFCLSID exclude2);

// %APPDATA%\MyAsioSplitter\config.ini
bool LoadSelectedClsid(CLSID& out);
bool SaveSelectedClsid(REFCLSID clsid);

// Modal picker. Returns an index into drivers, or -1 when cancelled.
int ShowAsioDevicePicker(const std::vector<AsioDriverInfo>& drivers, const CLSID* preselect);
