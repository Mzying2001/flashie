#pragma once

namespace SwfMimeFilter {

bool Initialize();
void Shutdown();

bool IsSupportedSwfUrl(const wchar_t* url);
bool Arm(const wchar_t* url);
void Cancel();
bool IsArmed();

} // namespace SwfMimeFilter
