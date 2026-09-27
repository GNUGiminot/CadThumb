#pragma once

namespace ct {

// Restarts Explorer of the current session through the Restart Manager: Explorer shuts down gracefully
// and is started again in the user's normal context (also when the caller is elevated).
// Needed after (un)registering thumbnail handlers: a running Explorer — the desktop in particular —
// keeps its old per-extension handler info until it restarts.
bool RestartExplorer();

} // namespace ct
