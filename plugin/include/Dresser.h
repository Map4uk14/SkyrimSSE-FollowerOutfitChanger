#pragma once
// Dress Your Followers - PrismaUI dressing overlay.
// A non-pausing HTML overlay listing the crosshair follower's wearables with
// live checkboxes. The panel is a view + input surface only: toggles are handed
// to the Papyrus controller (DYF_ToggleItem mod event) which owns the proven
// equip + persistence engine. Papyrus reads the exact target follower back via
// the DYF_Native.GetPanelTarget() native this plugin registers.

namespace Dresser {
    // Called once PrismaUI is available (SKSE kDataLoaded). Creates the view and
    // registers the input toggle + JS listeners. Safe no-op if PrismaUI missing.
    void Init();

    // Called on kPostLoadGame / kNewGame: close the panel and forget the target.
    // The view (a browser page) outlives save loads, so without this a panel left
    // open would carry a target from the previous save into the new one.
    void OnGameLoaded();

    // Registers the DYF_Native Papyrus functions (call from SKSEPluginLoad).
    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
}
