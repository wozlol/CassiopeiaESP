// The layer above every mode, so the launcher can always be reached.
// - Hold GUI and Esc for 2 seconds in any mode, even one that has frozen, and the device returns to the loader.
// - A mode that crashes or hangs three times in a row (panic, watchdog, brownout) is not started a fourth time, the loader starts instead.
// Call launcher_guard_start() first thing in app_main of every mode.
#pragma once
void launcher_guard_start(void);
// Restarts into the loader now.
void launcher_guard_go_loader(void);
