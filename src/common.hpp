// Small shared helpers for the helper process (dlss.cpp uses rd::Log and rd::ExeDir).
#pragma once
#include <string>

namespace rd {
void Log(const char* fmt, ...);
std::wstring ExeDir();  // the helper's folder (= the game folder), with a trailing backslash
}  // namespace rd
