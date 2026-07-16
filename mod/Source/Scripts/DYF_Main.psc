ScriptName DYF_Main extends Quest
{Dress Your Followers - main controller (PrismaUI edition).

 The dressing UI is a PrismaUI HTML overlay driven by the SKSE plugin
 (DressYourFollowers.dll). Look at a follower and press F4 to open it; each
 checkbox toggle sends a "DYF_ToggleItem" mod event to OnDYFToggleItem below,
 which owns all equip/unequip and persistence. This script no longer opens a
 vanilla trade menu - that flow and the in-menu try-on were removed when the UI
 moved to PrismaUI.

 Persistence: a dressed follower is added to a managed registry and their worn
 loadout is re-asserted on load and on a poll, so the engine's auto-equip
 cannot quietly redress them.}

; StorageUtil (PapyrusUtil) keys. MANAGED_KEY is a global list of dressed
; followers; LOADOUT_KEY is per-actor, holding the armor they should wear.
string Property MANAGED_KEY = "DYF_Managed" AutoReadOnly
string Property LOADOUT_KEY = "DYF_Loadout" AutoReadOnly

; Re-assert cadence while at least one managed follower is loaded.
float Property POLL_INTERVAL = 3.0 AutoReadOnly

; After a cell/location change the engine re-dresses followers from their loose
; inventory; a follower may also stream in a moment after the transition. So on
; those events we run an immediate re-assert and then a short burst of quick
; follow-up ticks (FAST_TICKS x FAST_INTERVAL) before relaxing back to the normal
; poll - this closes the "wears random gear for ~3s, then snaps back" window.
float Property FAST_INTERVAL = 0.5 AutoReadOnly
int Property FAST_TICKS = 6 AutoReadOnly

; Remaining quick follow-up ticks in the current burst (persists in the save;
; harmless if a burst is mid-flight across a load).
int fastTicksRemaining = 0

; -------------------------------------------------------------------
; Properties (filled in the Creation Kit)
; -------------------------------------------------------------------

Faction Property CurrentFollowerFaction Auto
{Vanilla CurrentFollowerFaction [FACT:0005C84E] - fallback follower check}

Outfit Property EmptyOutfit Auto
{DYF_EmptyOutfit - an outfit containing no items. Optional: if assigned, a
 managed follower's default-outfit reversion equips nothing instead of their
 default gear, leaving our equipped clothing in place. If left empty we rely
 purely on the re-assert poll (which leaves their original armor in inventory).}

; -------------------------------------------------------------------
; Lifecycle
; -------------------------------------------------------------------

Event OnInit()
    RegisterModEvents()
EndEvent

; Called by DYF_PlayerAlias on every game load. Mod-event registrations do not
; survive a save load, so the PrismaUI bridge is re-armed here.
Function RegisterModEvents()
    RegisterForModEvent("DYF_ToggleItem", "OnDYFToggleItem")
    RegisterForModEvent("DYF_UndressAll", "OnDYFUndressAll")
    RegisterForModEvent("DYF_PresetSave", "OnDYFPresetSave")
    RegisterForModEvent("DYF_PresetApply", "OnDYFPresetApply")
EndFunction

; Called by DYF_PlayerAlias on load. Re-assert every managed follower's outfit
; and resume the poll (SKSE update registrations don't survive a load).
Function ResumeManagement()
    SyncSettings()
    if StorageUtil.FormListCount(none, MANAGED_KEY) > 0
        ; Rebuild the plugin's loadout mirror BEFORE re-asserting: its hook refuses
        ; the engine's auto-equip only for loadouts it knows about, and it starts
        ; every launch empty.
        SyncLoadouts()
        ReassertAllLoaded()
        RegisterForSingleUpdate(POLL_INTERVAL)
    endif
EndFunction

; Mirror one follower's loadout down to the SKSE plugin, whose anti-auto-equip hook
; consults it (see DYF_Native.SetLoadout). Call after ANY change to the loadout -
; if the mirror goes stale the hook stops refusing the engine's auto-equip and the
; old "wears everything, then snaps back" flicker returns.
Function PushLoadout(Actor akFollower)
    if akFollower
        DYF_Native.SetLoadout(akFollower, StorageUtil.FormListToArray(akFollower, LOADOUT_KEY))
    endif
EndFunction

; Re-push the whole mirror on load. The plugin restores it from its own co-save
; before the engine dresses anyone, so this is a late confirmation and a safety net
; (e.g. a save made before the co-save existed), not the primary path.
Function SyncLoadouts()
    int n = StorageUtil.FormListCount(none, MANAGED_KEY)
    int i = 0
    while i < n
        Actor a = StorageUtil.FormListGet(none, MANAGED_KEY, i) as Actor
        if a
            PushLoadout(a)
            PushPresets(a)
        endif
        i += 1
    endwhile
EndFunction

; Push MCM-configured plugin settings (overlay hotkey + accent colour) down to the
; SKSE plugin. The plugin resets these to its own defaults on every launch, so this
; runs on each game load and whenever the MCM value changes (see DYF_MCM). A key of
; 0 means the MCM has not been read yet - leave the plugin default in place; -1
; means the user deliberately unbound the key.
Function SyncSettings()
    int k = MCM.GetModSettingInt("DressYourFollowers", "iOpenKey:General")
    if k != 0
        DYF_Native.SetToggleKey(k)
    endif
    int accent = MCM.GetModSettingInt("DressYourFollowers", "iAccent:General")
    if accent != 0
        DYF_Native.SetAccentColor(accent)
    endif
EndFunction

; -------------------------------------------------------------------
; Helpers
; -------------------------------------------------------------------

; Mirrors the plugin's IsFollower (teammate OR follower faction). No combat or
; hostility test here: the plugin has already applied the equip by the time this
; runs, so refusing to bookkeep would desync the loadout and the next poll would
; strip the piece the user just put on.
bool Function IsValidTarget(Actor akTarget)
    if akTarget == none || akTarget.IsDead()
        return false
    endif
    if akTarget.IsPlayerTeammate()
        return true
    endif
    ; Fallback: follower frameworks (NFF/EFF/AFT) toggle teammate status themselves
    if CurrentFollowerFaction && akTarget.IsInFaction(CurrentFollowerFaction)
        return true
    endif
    return false
EndFunction

; -------------------------------------------------------------------
; PrismaUI panel bridge
; -------------------------------------------------------------------

; Sent by the PrismaUI dressing overlay (via the SKSE plugin). The clicked armor
; arrives as the event sender; strArg is "equip" or "unequip". The plugin also
; tells us exactly which follower the panel targets through DYF_Native, so we act
; on the right actor even while the overlay holds focus (the crosshair is not
; reliable then).
;
; The follower's saved loadout is the single source of truth for what they should
; wear. The plugin already applied the equip/unequip on the game thread before
; sending this event; here we only edit the loadout to match the same intent
; (add on equip, remove on unequip). This runs WITHOUT any Utility.Wait, so the
; whole toggle is one atomic step - no other toggle or poll tick can interleave
; and corrupt the state, and we never read half-applied worn gear.
Event OnDYFToggleItem(string eventName, string strArg, float numArg, Form sender)
    if !MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        return
    endif
    Form piece = sender
    Armor armPiece = sender as Armor    ; none for weapons
    Actor follower = DYF_Native.GetPanelTarget()
    if piece == none || (armPiece == none && (sender as Weapon) == none) || !IsValidTarget(follower)
        return
    endif

    ; First touch: register + seed the loadout with their current look so the
    ; engine has nothing of its own to revert to.
    EnsureManaged(follower)

    if strArg == "unequip"
        StorageUtil.FormListRemove(follower, LOADOUT_KEY, piece, true)
    else
        ; Adding a piece: drop any loadout piece that fights it for the same slot,
        ; so the new one wins cleanly and the loadout only ever describes a look the
        ; follower can actually wear.
        if armPiece
            int pieceMask = armPiece.GetSlotMask()
            Form[] load = StorageUtil.FormListToArray(follower, LOADOUT_KEY)
            int i = 0
            while i < load.Length
                Armor other = load[i] as Armor
                if other && other != armPiece && Math.LogicalAnd(other.GetSlotMask(), pieceMask) != 0
                    StorageUtil.FormListRemove(follower, LOADOUT_KEY, other, true)
                endif
                i += 1
            endwhile
        else
            PurgeConflictingWeapons(follower, sender as Weapon)
        endif
        if !StorageUtil.FormListHas(follower, LOADOUT_KEY, piece)
            StorageUtil.FormListAdd(follower, LOADOUT_KEY, piece)
        endif
    endif

    ; NOTE: we do NOT re-apply equipment here. The SKSE plugin already equipped/
    ; unequipped the piece instantly on the game thread before sending this event,
    ; so calling ReassertOutfit now would be a redundant second apply - and if it
    ; read the worn state before the plugin's change registered, it re-equipped and
    ; caused a visible equip/unequip/equip flicker. Papyrus only keeps the books
    ; (loadout + management) here; the 3s poll below re-asserts as the safety net.
    PushLoadout(follower)
    follower.QueueNiNodeUpdate()

    ; Repaint the overlay from real worn state and keep the poll alive.
    SendPanelRefresh()
    RegisterForSingleUpdate(POLL_INTERVAL)
EndEvent

; Sent by the overlay's "Unequip all" button. The plugin has already stripped
; every worn piece on the game thread; all that is left is the durable state.
;
; Clearing the loadout is what makes it stick: ReassertOutfit re-equips loadout
; pieces and strips anything worn that is NOT in the loadout, so an empty loadout
; turns the poll into the thing that keeps them undressed. Without this the very
; next tick would put the whole outfit straight back on. Their gear stays in their
; inventory, so re-dressing them is just ticking the boxes again.
Event OnDYFUndressAll(string eventName, string strArg, float numArg, Form sender)
    if !MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        return
    endif
    Actor follower = DYF_Native.GetPanelTarget()
    if !IsValidTarget(follower)
        return
    endif
    ; First touch: register them so the poll runs at all. EnsureManaged seeds the
    ; loadout from the worn set - which the plugin has just emptied - and neutralises
    ; their default outfit, so the engine has nothing of its own to revert to.
    EnsureManaged(follower)
    StorageUtil.FormListClear(follower, LOADOUT_KEY)
    ; An empty loadout pushed down is what tells the hook to refuse EVERY armor
    ; equip on them - that is what keeps them stripped with no flicker.
    PushLoadout(follower)
    follower.QueueNiNodeUpdate()
    SendPanelRefresh()
    RegisterForSingleUpdate(POLL_INTERVAL)
EndEvent

; -------------------------------------------------------------------
; Outfit presets (three per-follower slots)
; -------------------------------------------------------------------

; Per-follower StorageUtil key of one preset slot ("1".."3"). Each preset is a
; FormList shaped exactly like LOADOUT_KEY - a saved outfit is just a saved loadout.
string Function PresetKey(string slot)
    return "DYF_Preset" + slot
EndFunction

; Tell the plugin which slots are occupied (bit 0 = slot 1) so the overlay can
; draw filled vs empty preset buttons. UI-only; the contents stay here.
Function PushPresets(Actor akFollower)
    int mask = 0
    if StorageUtil.FormListCount(akFollower, PresetKey("1")) > 0
        mask += 1
    endif
    if StorageUtil.FormListCount(akFollower, PresetKey("2")) > 0
        mask += 2
    endif
    if StorageUtil.FormListCount(akFollower, PresetKey("3")) > 0
        mask += 4
    endif
    DYF_Native.SetPresets(akFollower, mask)
EndFunction

; Overlay "save the current outfit into slot N" (strArg = "1".."3"). The current
; loadout IS the outfit, so saving is a list copy. On a fresh follower
; EnsureManaged seeds the loadout from what they are wearing right now, so Save
; captures their current look even before any toggles.
Event OnDYFPresetSave(string eventName, string strArg, float numArg, Form sender)
    if !MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        return
    endif
    Actor follower = DYF_Native.GetPanelTarget()
    if !IsValidTarget(follower)
        return
    endif
    EnsureManaged(follower)
    string pkey = PresetKey(strArg)
    StorageUtil.FormListClear(follower, pkey)
    Form[] load = StorageUtil.FormListToArray(follower, LOADOUT_KEY)
    int i = 0
    while i < load.Length
        StorageUtil.FormListAdd(follower, pkey, load[i])
        i += 1
    endwhile
    PushPresets(follower)
    SendPanelRefresh()
EndEvent

; Overlay "apply outfit N": the preset becomes the loadout, then one
; ReassertOutfit pass makes the worn state match - it equips what they still
; carry and strips worn armor that is not in the outfit. Preset items no longer
; in their inventory stay in the loadout on purpose: ReassertOutfit skips absent
; pieces, and the outfit completes itself if the piece ever comes back.
Event OnDYFPresetApply(string eventName, string strArg, float numArg, Form sender)
    if !MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        return
    endif
    Actor follower = DYF_Native.GetPanelTarget()
    if !IsValidTarget(follower)
        return
    endif
    string pkey = PresetKey(strArg)
    if StorageUtil.FormListCount(follower, pkey) <= 0
        return ; empty slot - the overlay greys these out, but never trust the UI
    endif
    EnsureManaged(follower)
    StorageUtil.FormListClear(follower, LOADOUT_KEY)
    Form[] outfitItems = StorageUtil.FormListToArray(follower, pkey)
    int i = 0
    while i < outfitItems.Length
        StorageUtil.FormListAdd(follower, LOADOUT_KEY, outfitItems[i])
        i += 1
    endwhile
    ; Arm the mirror BEFORE touching worn state - our own equips run through the
    ; anti-auto-equip hook and would be refused with a stale mirror.
    PushLoadout(follower)
    ReassertOutfit(follower)
    follower.QueueNiNodeUpdate()
    SendPanelRefresh()
    RegisterForSingleUpdate(POLL_INTERVAL)
EndEvent

; Does this weapon claim both hands? Weapon types are the engine's own enum, the
; same one the overlay draws its icons from: 5 greatsword, 6 battleaxe, 7 bow,
; 9 crossbow. Everything else (sword/dagger/axe/mace/staff) takes a single hand,
; and staves dual-wield like any other one-hander.
bool Function IsTwoHanded(Weapon akWeapon)
    int t = akWeapon.GetWeaponType()
    return t == 5 || t == 6 || t == 7 || t == 9
EndFunction

; Keep the loadout to a set of weapons the engine can actually hold at once: a
; two-hander/bow/crossbow claims both hands and evicts everything else, a one-hander
; evicts any two-hander and keeps at most one companion. Without this, ticking two
; swords then a greatsword left all three in the loadout, describing a look no actor
; can wear.
;
; Evicted pieces stay in the follower's inventory - this only unticks them.
Function PurgeConflictingWeapons(Actor akFollower, Weapon akNew)
    if akNew == none
        return
    endif
    bool newTwoHanded = IsTwoHanded(akNew)
    Form[] victims = Utility.CreateFormArray(34)
    int victimCount = 0
    int handsUsed = 1   ; the newcomer takes one

    Form[] load = StorageUtil.FormListToArray(akFollower, LOADOUT_KEY)
    int i = 0
    while i < load.Length
        Weapon other = load[i] as Weapon
        if other && other != akNew
            bool evict = true
            if !newTwoHanded && !IsTwoHanded(other) && handsUsed < 2
                handsUsed += 1      ; a one-hander can share with a one-hander
                evict = false
            endif
            if evict && victimCount < 34
                victims[victimCount] = other
                victimCount += 1
                StorageUtil.FormListRemove(akFollower, LOADOUT_KEY, other, true)
            endif
        endif
        i += 1
    endwhile

    i = 0
    while i < victimCount
        akFollower.UnequipItem(victims[i], false, true)
        i += 1
    endwhile
EndFunction

; Register a follower for management the first time the panel touches them,
; preserving their current look. The current worn set becomes their initial
; loadout. If EmptyOutfit is assigned we also neutralise their default outfit
; (SetOutfit strips its items, so we re-add and re-equip the kept look) - then the
; engine has nothing of its own to revert to. If it is not assigned, the poll's
; strip step keeps unwanted gear off instead.
Function EnsureManaged(Actor akFollower)
    if StorageUtil.FormListHas(none, MANAGED_KEY, akFollower)
        return
    endif

    Form[] keep = new Form[34]
    int keepCount = 0
    int bit = 0
    int mask = 1
    while bit < 32
        Armor worn = akFollower.GetWornForm(mask) as Armor
        if worn && keep.Find(worn) < 0 && keepCount < 34
            keep[keepCount] = worn
            keepCount += 1
        endif
        bit += 1
        mask *= 2
    endwhile

    ; Also preserve any weapons they have drawn/equipped (not covered by biped
    ; slots) so the loadout keeps their current sidearm(s) too.
    Weapon rWpn = akFollower.GetEquippedWeapon(false)
    if rWpn && keep.Find(rWpn) < 0 && keepCount < 34
        keep[keepCount] = rWpn
        keepCount += 1
    endif
    Weapon lWpn = akFollower.GetEquippedWeapon(true)
    if lWpn && keep.Find(lWpn) < 0 && keepCount < 34
        keep[keepCount] = lWpn
        keepCount += 1
    endif

    StorageUtil.FormListAdd(none, MANAGED_KEY, akFollower)

    ; Seed the loadout directly from the worn set (no re-read of worn state, so
    ; nothing here can race a just-applied equip).
    StorageUtil.FormListClear(akFollower, LOADOUT_KEY)
    int i = 0
    while i < keepCount
        StorageUtil.FormListAdd(akFollower, LOADOUT_KEY, keep[i])
        i += 1
    endwhile

    ; Push the seeded loadout BEFORE the SetOutfit block below: from here on the
    ; plugin's hook refuses engine auto-equips on this follower, and the pieces we
    ; are about to re-equip are in the loadout, so they are allowed through.
    PushLoadout(akFollower)

    if EmptyOutfit
        akFollower.SetOutfit(EmptyOutfit)
        i = 0
        while i < keepCount
            Armor p = keep[i] as Armor
            if p
                if akFollower.GetItemCount(p) <= 0
                    akFollower.AddItem(p, 1, true) ; SetOutfit stripped it - put it back
                endif
                ; Only re-equip pieces SetOutfit actually knocked off; re-equipping
                ; ones still worn would flicker them for no reason.
                if !akFollower.IsEquipped(p)
                    akFollower.EquipItemEx(p, 0, false, false)
                endif
            endif
            i += 1
        endwhile
    endif
EndFunction

; Fire-and-forget signal to the SKSE plugin: re-read the target follower's worn
; gear and repaint the overlay checkboxes from it. Sent after every toggle and on
; each poll tick while the overlay is open, so the UI always shows the truth.
Function SendPanelRefresh()
    int h = ModEvent.Create("DYF_PanelRefresh")
    if h
        ModEvent.Send(h)
    endif
EndFunction

; -------------------------------------------------------------------
; Persistence (Spec 4)
; -------------------------------------------------------------------

; Re-equip any loadout piece the follower still carries but isn't wearing.
; The only thing we refuse to do is equip a piece whose slot is already held by
; ANOTHER LOADOUT piece that's on - that (and only that) would be the same-slot
; ping-pong. A slot held by non-loadout gear (e.g. their default armor that the
; engine re-equipped on a cell change) is fair game: we equip over it so the
; saved outfit is restored. That distinction is what makes it persist.
Function ReassertOutfit(Actor akActor)
    Form[] load = StorageUtil.FormListToArray(akActor, LOADOUT_KEY)

    ; Slots already satisfied by a loadout piece that is currently worn.
    int loadoutWorn = 0
    int i = 0
    while i < load.Length
        Armor p = load[i] as Armor
        if p && akActor.IsEquipped(p)
            loadoutWorn = Math.LogicalOr(loadoutWorn, p.GetSlotMask())
        endif
        i += 1
    endwhile

    i = 0
    while i < load.Length
        Armor piece = load[i] as Armor
        if piece
            if akActor.GetItemCount(piece) > 0 && !akActor.IsEquipped(piece)
                int pieceMask = piece.GetSlotMask()
                if Math.LogicalAnd(pieceMask, loadoutWorn) == 0
                    akActor.EquipItemEx(piece, 0, false, false)
                    loadoutWorn = Math.LogicalOr(loadoutWorn, pieceMask)
                endif
            endif
        endif
        i += 1
    endwhile

    ; Weapons share the hand slots, so they get the same "already satisfied"
    ; treatment as armor slots: if ANY loadout weapon is in hand, the hands are
    ; considered dressed and we equip nothing (blindly equipping every listed
    ; weapon made two loadout weapons steal the hand from each other on every
    ; poll tick). Never touch weapons in combat - that is the combat AI's call.
    if !akActor.IsInCombat()
        bool handSatisfied = false
        i = 0
        while i < load.Length && !handSatisfied
            Weapon w = load[i] as Weapon
            if w && akActor.IsEquipped(w)
                handSatisfied = true
            endif
            i += 1
        endwhile
        i = 0
        while i < load.Length && !handSatisfied
            Weapon w = load[i] as Weapon
            if w && akActor.GetItemCount(w) > 0
                akActor.EquipItemEx(w, 0, false, false)
                handSatisfied = true
            endif
            i += 1
        endwhile
    endif

    ; Enforce "only the saved loadout is worn": strip anything the engine
    ; re-dressed that is NOT part of the loadout (e.g. their default gear the
    ; auto-equip put back). This is what makes an unequip stick even without a
    ; dedicated empty outfit assigned in the CK - the piece is removed again on
    ; the next poll tick instead of quietly returning for good.
    int bit = 0
    int mask = 1
    while bit < 32
        Armor worn = akActor.GetWornForm(mask) as Armor
        if worn && load.Find(worn) < 0
            akActor.UnequipItem(worn, false, true)
        endif
        bit += 1
        mask *= 2
    endwhile
EndFunction

; Immediate re-assert triggered by a cell/location change (see FAST_* above).
; Corrects the outfit now, then kicks off a short fast-tick burst so followers
; that stream in just after the transition are caught within half a second
; instead of on the slow poll.
Function ReassertSoon()
    if !MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        return
    endif
    if StorageUtil.FormListCount(none, MANAGED_KEY) <= 0
        return
    endif
    ReassertAllLoaded()
    fastTicksRemaining = FAST_TICKS
    RegisterForSingleUpdate(FAST_INTERVAL)
EndFunction

Function ReassertAllLoaded()
    int n = StorageUtil.FormListCount(none, MANAGED_KEY)
    int i = 0
    while i < n
        Actor a = StorageUtil.FormListGet(none, MANAGED_KEY, i) as Actor
        if a && !a.IsDead() && a.Is3DLoaded()
            ReassertOutfit(a)
        endif
        i += 1
    endwhile
EndFunction

; Managed-follower names for the MCM read-only list (Spec 6). Element 0 is a
; count summary so the collapsed menu row shows "N dressed" at a glance; the rest
; are the follower display names. Returns a single "None dressed yet" entry when
; the registry is empty.
string[] Function GetManagedNames()
    int n = StorageUtil.FormListCount(none, MANAGED_KEY)
    if n <= 0
        string[] empty = new string[1]
        empty[0] = "None dressed yet"
        return empty
    endif
    string[] names = Utility.CreateStringArray(n + 1)
    names[0] = n + " dressed"
    int i = 0
    while i < n
        Actor a = StorageUtil.FormListGet(none, MANAGED_KEY, i) as Actor
        if a
            names[i + 1] = a.GetDisplayName()
        else
            names[i + 1] = "(unknown)"
        endif
        i += 1
    endwhile
    return names
EndFunction

Event OnUpdate()
    if MCM.GetModSettingBool("DressYourFollowers", "bModEnabled:General")
        ReassertAllLoaded()
    endif
    ; While the overlay is open, keep its checkboxes in sync with reality in case
    ; the engine re-dressed something between clicks. GetPanelTarget is non-none
    ; only while a panel is open.
    if DYF_Native.GetPanelTarget()
        SendPanelRefresh()
    endif
    ; Keep polling only while there is someone to manage. While a burst is
    ; active (just after a transition) tick quickly, then relax to the slow poll.
    if StorageUtil.FormListCount(none, MANAGED_KEY) > 0
        if fastTicksRemaining > 0
            fastTicksRemaining -= 1
            RegisterForSingleUpdate(FAST_INTERVAL)
        else
            RegisterForSingleUpdate(POLL_INTERVAL)
        endif
    endif
EndEvent

; MCM "Release all followers": stop managing everyone. They keep whatever they
; are wearing; the mod simply stops re-asserting it. Used before uninstalling.
Function ClearAllManaged()
    int n = StorageUtil.FormListCount(none, MANAGED_KEY)
    int i = 0
    while i < n
        Form a = StorageUtil.FormListGet(none, MANAGED_KEY, i)
        if a
            StorageUtil.FormListClear(a, LOADOUT_KEY)
            StorageUtil.FormListClear(a, PresetKey("1"))
            StorageUtil.FormListClear(a, PresetKey("2"))
            StorageUtil.FormListClear(a, PresetKey("3"))
        endif
        i += 1
    endwhile
    StorageUtil.FormListClear(none, MANAGED_KEY)
    ; Drop the plugin's mirror too, or its hook would keep refusing the engine's
    ; auto-equip for followers we no longer manage - they'd stay frozen in whatever
    ; they had on. Releasing means handing them back to vanilla.
    DYF_Native.ClearAllLoadouts()
EndFunction
