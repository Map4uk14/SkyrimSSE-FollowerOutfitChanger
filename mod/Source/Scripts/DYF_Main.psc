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
EndFunction

; Called by DYF_PlayerAlias on load. Re-assert every managed follower's outfit
; and resume the poll (SKSE update registrations don't survive a load).
Function ResumeManagement()
    if StorageUtil.FormListCount(none, MANAGED_KEY) > 0
        ReassertAllLoaded()
        RegisterForSingleUpdate(POLL_INTERVAL)
    endif
EndFunction

; -------------------------------------------------------------------
; Helpers
; -------------------------------------------------------------------

; Corner notification, gated by the MCM "Show status messages" toggle.
Function Notify(string asText)
    if MCM.GetModSettingBool("DressYourFollowers", "bNotify:General")
        Debug.Notification(asText)
    endif
EndFunction

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
        Notify("Removed: " + piece.GetName())
    else
        ; Adding an armor piece: drop any loadout piece that fights for the same
        ; biped slot so the new one wins cleanly. Weapons have no biped slot, so
        ; they skip this (the engine handles hand slots itself).
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
        endif
        if !StorageUtil.FormListHas(follower, LOADOUT_KEY, piece)
            StorageUtil.FormListAdd(follower, LOADOUT_KEY, piece)
        endif
        Notify("Equipped: " + piece.GetName())
    endif

    ; NOTE: we do NOT re-apply equipment here. The SKSE plugin already equipped/
    ; unequipped the piece instantly on the game thread before sending this event,
    ; so calling ReassertOutfit now would be a redundant second apply - and if it
    ; read the worn state before the plugin's change registered, it re-equipped and
    ; caused a visible equip/unequip/equip flicker. Papyrus only keeps the books
    ; (loadout + management) here; the 3s poll below re-asserts as the safety net.
    follower.QueueNiNodeUpdate()

    ; Repaint the overlay from real worn state and keep the poll alive.
    SendPanelRefresh()
    RegisterForSingleUpdate(POLL_INTERVAL)
EndEvent

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
    ; Keep polling only while there is someone to manage.
    if StorageUtil.FormListCount(none, MANAGED_KEY) > 0
        RegisterForSingleUpdate(POLL_INTERVAL)
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
        endif
        i += 1
    endwhile
    StorageUtil.FormListClear(none, MANAGED_KEY)
    Notify("Dress Your Followers: released all managed followers.")
EndFunction
