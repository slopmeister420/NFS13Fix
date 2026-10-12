#pragma once
#include <windows.h>

// NFS13Fix.log next to the asi, rewritten each launch when logging is on
namespace fixlog {

void Init(HMODULE self, bool enabled);
void Write(const char* fmt, ...);

} // namespace fixlog

#define LOGF(...) ::fixlog::Write(__VA_ARGS__)
