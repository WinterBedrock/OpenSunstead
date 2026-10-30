#pragma once
#include <Windows.h>
#include <mmsystem.h>
#include <string>
#include <cstdio>
#pragma comment(lib, "winmm.lib")

extern HMODULE g_hSelfModule;

// Plays an embedded RCDATA sound resource. Handles both WAV and MP3 by
// extracting the resource bytes to a temp file and driving it through MCI
// (PlaySound's SND_RESOURCE only supports WAV). Playback is asynchronous.
namespace SoundUtils {

    inline void PlayResource(int resId, const wchar_t* ext) {
        HMODULE mod = g_hSelfModule ? g_hSelfModule : GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
        if (!res) return;
        HGLOBAL h = LoadResource(mod, res);
        if (!h) return;
        void* data = LockResource(h);
        DWORD size = SizeofResource(mod, res);
        if (!data || !size) return;

        wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%ssunstead_snd_%d%s", tmpDir, resId, ext);

        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(f, data, size, &written, nullptr);
        CloseHandle(f);

        // Drive via MCI so MP3 works. Each alias is unique so concurrent sounds don't clash.
        wchar_t alias[64]; swprintf_s(alias, L"sndaihq_%d", resId);
        wchar_t cmd[MAX_PATH + 128];
        swprintf_s(cmd, L"open \"%s\" alias %s", path, alias);
        mciSendStringW(cmd, nullptr, 0, nullptr);
        swprintf_s(cmd, L"play %s", alias);
        mciSendStringW(cmd, nullptr, 0, nullptr);
        // Device is left open; it is released by the OS on process exit. Acceptable
        // for one-shot inject/uninject feedback.
    }
}