#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

struct AsioDriverInfo {
    std::wstring name;
    CLSID clsid;
};

std::vector<AsioDriverInfo> EnumerateAsioDrivers(REFCLSID exclude1, REFCLSID exclude2);

bool LoadSelectedClsid(CLSID& out);
bool SaveSelectedClsid(REFCLSID clsid);

int ShowAsioDevicePicker(const std::vector<AsioDriverInfo>& drivers, const CLSID* preselect);