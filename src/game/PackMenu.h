// Tessera - in-world resource pack menu (F3+P).
#pragma once

namespace ll::io {
class Logger;
}

namespace tessera::game::packmenu {

// The menu is a server-style form that the client shows and answers by itself:
// the request is handed to the client's own packet handler and the answer is
// taken out of the outgoing packets, so it also works on multiplayer servers.
bool install(ll::io::Logger& logger);
void uninstall();

// Main thread, once per frame: the F3+P key and forms waiting to be shown.
void frame();

// Logs the global, level and full pack stacks (diagnostics).
void logStacks(char const* when);

} // namespace tessera::game::packmenu
