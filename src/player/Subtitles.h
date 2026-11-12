// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#pragma once
#include <string>
#include <vector>
#include "FFPlayer.h"

std::wstring CleanSubtitle( const std::string& raw );

bool LoadSrt( const std::string& path, std::vector<SubtitleCue>& out );
bool ParseSrt( std::string data, std::vector<SubtitleCue>& out );

class Font;
void DrawSubtitle( Font& font, const std::wstring& text, float bottom );
