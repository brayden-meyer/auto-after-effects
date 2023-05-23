#include "AEConfig.h"
#ifndef AE_OS_WIN
#error This package targets 64-bit Windows After Effects.
#endif
resource 'PiPL' (16000) {
    {
        Kind { AEGP },
        Name { "Sound Keys Bridge" },
        Category { "General Plugin" },
        Version { 65536 },
        CodeWin64X86 { "EntryPointFunc" },
    }
};
