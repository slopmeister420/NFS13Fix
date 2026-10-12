#pragma once
#include <string>

// path normalisation for the mods index. its own header so the tests exercise
// the same code the loader runs.
namespace pathkey {

// lowercases and converts '/' to '\'
inline std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s)
        c = (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a')
                                     : (c == L'/' ? L'\\' : c);
    return s;
}

// normalises a path the game passed into an index key: lowercased, backslashes,
// relative to gameDir (itself lowercased, no trailing slash). empty when the
// path is absolute but outside the game directory.
inline std::wstring Make(const wchar_t* path, const std::wstring& gameDir)
{
    if (!path || !*path)
        return {};

    std::wstring s = Lower(path);
    if (s.compare(0, 4, L"\\\\?\\") == 0)
        s.erase(0, 4);

    if (!gameDir.empty() && s.size() > gameDir.size() + 1 &&
        s.compare(0, gameDir.size(), gameDir) == 0 && s[gameDir.size()] == L'\\')
        s.erase(0, gameDir.size() + 1);
    else if (s.size() > 2 && s[1] == L':')
        return {};

    while (s.compare(0, 2, L".\\") == 0)
        s.erase(0, 2);
    while (!s.empty() && s.front() == L'\\')
        s.erase(0, 1);
    return s;
}

} // namespace pathkey
