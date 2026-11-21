#pragma once
#include <windows.h>

// Replaces the title executable's msvcp140 _Cnd_wait import so that a condition-variable wait on
// an application single-threaded apartment (the CoreApplication view thread) keeps servicing
// incoming COM calls. See EraStaWait.cpp for why.
void EraInstallStaWaitPatch(HMODULE titleModule);
