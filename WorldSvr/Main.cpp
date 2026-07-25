#include <iostream>
#include <Core.h>
#include <Proc/Proc.h>
#include "Memory/Memory.h"
#include "Game/MacroBM3/MacroBM3.h"
#include "Game/EventAlert/EventAlert.h"
#include "Game/Warp/Warp.h"
#include "Game/WindowList/WindowList.h"
#include "Game/AutoPlay/AutoPlay.h"
#include "Game/Management/Management.h"

void LoadConfigs()
{
    static bool done = false;
    if (done) return;               // idempotent: nu inregistra de doua ori
    done = true;

    Management::WriteLogs("./AutoPlay.log", "LoadConfigs(): inregistrez modulele");
    INIT(Proc);
    INIT(Warp);
    INIT(AutoPlay);
    EventAlert::Initialize();
    MacroBM3::Initialize();
    WindowList::Initialize();
    Management::WriteLogs("./AutoPlay.log", "LoadConfigs(): gata");
}

extern "C" void Init()
{
    Management::WriteLogs("./AutoPlay.log", ">>> Init() apelat de binarul host <<<");
    LoadConfigs();
}

// DIAG: ruleaza automat cand lib.so e incarcat (LD_PRELOAD). Doar logheaza -
// nu apeleaza LoadConfigs aici (harta de proceduri a jocului poate sa nu fie
// inca pregatita la momentul incarcarii .so).
__attribute__((constructor))
static void OnPluginLoad()
{
    Management::WriteLogs("./AutoPlay.log", "=== constructor: lib.so incarcat via LD_PRELOAD ===");
}