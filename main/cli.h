// Command line on the "UART" port: report | clear | label <name> | help
#pragma once

#include <stdbool.h>

// Start the CLI task and print the banner. `store_ok` is false when the
// session log could not be opened at boot.
void cli_start(bool store_ok);
