#pragma once

namespace FlashLoader {

// Phase 1: Load Flash.ocx and obtain its class factory.
// Must be called AFTER OleInitialize on the owning STA. Idempotent on that STA.
bool Activate();

// Phase 2: Atomically install inline hooks (detours) on COM,
// registry, file, Flash host identity, WLDP, and TypeLib APIs. Force-loads
// mshtml.dll/urlmon.dll/ieframe.dll and resolves every required
// target before patching. Returns false without leaving a partial
// hook set if resolution or the Detours transaction fails. Call
// BEFORE browser creation so hooks are in place when MSHTML initializes.
bool InstallHooks();

// Cleanup. Must be called on the Flash activation STA and BEFORE
// OleUninitialize. Vtable restoration, Detours detach, or class revocation
// failures are logged and leave the remaining state intact for a retry.
void Deactivate();

} // namespace FlashLoader
