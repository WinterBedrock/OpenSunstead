// todo recode
#pragma once

namespace ColorCodeHook {

enum class Code {
    Theme = 0,
    Accent = 1,
    Custom = 2,
};

void Initialize();

void Shutdown();

bool IsAvailable();

const char* CodeString(Code c);

const char* ThemeCode();
} // namespace ColorCodeHook
