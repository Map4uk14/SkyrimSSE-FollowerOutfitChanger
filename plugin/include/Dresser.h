#pragma once
// Follower Outfit Changer - PrismaUI dressing overlay.
// A non-pausing HTML overlay listing the crosshair follower's wearables with
// live checkboxes. The panel is a view + input surface only: toggles are handed
// to the Papyrus controller (DYF_ToggleItem mod event) which owns the proven
// equip + persistence engine. Papyrus reads the exact target follower back via
// the DYF_Native.GetPanelTarget() native this plugin registers.

namespace Dresser {
    // Installs the anti-auto-equip hook (call from SKSEPluginLoad, before the game
    // runs). This is the plugin's only engine hook: it refuses the engine's
    // auto-equip of armor that is not in a managed follower's saved loadout, which
    // is what stops the "wears everything, then snaps back" flicker at source.
    void InstallHooks();

    // Registers the SKSE co-save callbacks (call from SKSEPluginLoad). The hook's
    // loadout mirror rides in the co-save so it is restored DURING the load, before
    // the engine dresses actors - Papyrus pushes it too, but starts far too late to
    // beat that first auto-equip pass.
    void InitSerialization();

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
