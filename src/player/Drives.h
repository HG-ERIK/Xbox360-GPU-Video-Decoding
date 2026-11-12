// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <string>
#include <vector>

struct Drive
{
    std::string  root;
    std::wstring name;
};

void               MountDrives();
std::vector<Drive> AvailableDrives();
