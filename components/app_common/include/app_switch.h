#pragma once
#include <stdbool.h>
// Restarts the handheld straight into another program, by the name it was built with (like "mode1" or "library").
// Returns false, and does nothing, when no installed slot holds that program. On success it does not return.
bool app_switch(const char *project_name);
