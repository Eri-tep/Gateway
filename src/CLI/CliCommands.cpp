#include "CliCommon.h"

// ============================================================================
// SHARED CLI SCRATCH BUFFER (BSS Memory Optimization)
// Telnet CLI commands execute sequentially in the single Task_Telnet context.
// Sharing this 5KB buffer reclaims ~26KB of static BSS RAM.
// ============================================================================
char g_cli_scratch_buf[5120];
