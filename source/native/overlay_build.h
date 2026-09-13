#pragma once

// CMake passes explicit values. Standalone builds use owned DXGI proxies;
// the former native-table installer and discovery worker have been removed.
#ifndef MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN
#define MFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN 1
#endif

#ifndef MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY
#define MFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY 0
#endif

// Menu drawing can be disabled for an isolated forwarding-only check.
#ifndef MFG_UNLOCK_OVERLAY_MENU_DRAW
#define MFG_UNLOCK_OVERLAY_MENU_DRAW 1
#endif

// Deterministic coordinator fault points for the regression harness. Compiled
// only into dedicated test builds; absent from normal and shipping binaries.
#ifndef MFG_UNLOCK_OVERLAY_TEST_FAULTS
#define MFG_UNLOCK_OVERLAY_TEST_FAULTS 0
#endif

// Isolated comparison only; defaults preserve the menu-enabled build.
#ifndef MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK
#define MFG_UNLOCK_OVERLAY_SKIP_GPU_WORK 0
#endif
