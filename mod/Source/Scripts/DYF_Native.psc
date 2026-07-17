ScriptName DYF_Native Hidden
{Native bridge to the Follower Outfit Changer SKSE plugin (PrismaUI overlay).}

; The follower the PrismaUI dressing panel currently has open. The plugin sets
; this when the panel opens, so the controller acts on exactly that actor
; instead of guessing from the crosshair while a menu/overlay has focus.
Actor Function GetPanelTarget() global native

; Push the MCM-bound overlay open/close key (DirectX scancode; -1 = unbound) to
; the plugin. The plugin defaults to F4 each launch, so this is re-sent on load.
Function SetToggleKey(int aKeyCode) global native

; Push the MCM overlay accent colour (0xRRGGBB) to the plugin, which forwards it
; to the overlay CSS the next time the panel opens.
Function SetAccentColor(int aRgb) global native

; Push the MCM overlay size (percent, 100 = default) to the plugin, which applies
; it to the overlay the next time the panel opens.
Function SetUiScale(int aPercent) global native

; Mirror a managed follower's saved loadout down to the plugin. The plugin's
; anti-auto-equip hook runs inside the engine's equip path and cannot call into
; the VM, so it needs its own copy: it refuses any armor equip on a managed
; follower unless the piece is in this list. An EMPTY list is meaningful - the
; follower is managed and should wear nothing. Call this after every change to the
; loadout. The plugin also keeps this mirror in its own co-save, so on load it is
; already armed before the engine dresses anyone; our push on load just confirms it.
Function SetLoadout(Actor akActor, Form[] akItems) global native

; Make the follower's worn ARMOR match the mirror right now, on the game thread
; (strip non-loadout armor, equip carried loadout armor). Called after a bulk
; loadout change (preset apply / undo) so the outfit swaps instantly and the
; panel repaint reads the finished state. Weapons stay with ReassertOutfit.
; Requires PushLoadout FIRST - this syncs against the mirror.
Function SyncWornArmor(Actor akActor) global native

; Tell the plugin which outfit-preset slots hold a saved outfit for this follower
; (bit 0 = slot 1), so the overlay can draw filled vs empty preset buttons. UI-only:
; the preset contents live in StorageUtil. Pushed on load and after every save.
Function SetPresets(Actor akActor, int aMask) global native

; Release everyone: drops the plugin's whole mirror so those followers go back to
; vanilla auto-equip. Used by the MCM's "Release all followers".
Function ClearAllLoadouts() global native

; The default outfit (DOFT) currently on the actor's base. Read by EnsureManaged
; BEFORE it bakes the empty outfit in, so "Release all followers" can restore the
; original - vanilla Papyrus has SetOutfit but no getter. None if the base has no
; default outfit.
Outfit Function GetDefaultOutfit(Actor akActor) global native
