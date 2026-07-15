ScriptName DYF_PlayerAlias extends ReferenceAlias
{Dress Your Followers - player alias.
 Re-arms the PrismaUI mod-event bridge and resumes outfit management on every
 game load (SKSE registrations must be refreshed when a save is loaded).}

Event OnInit()
    DYF_Main main = GetOwningQuest() as DYF_Main
    main.RegisterModEvents()
    main.ResumeManagement()
EndEvent

Event OnPlayerLoadGame()
    DYF_Main main = GetOwningQuest() as DYF_Main
    main.RegisterModEvents()
    main.ResumeManagement()
EndEvent

; Cell/location transitions (fast travel, doors, dungeon entrances) are exactly
; when the engine re-dresses followers from their loose inventory. Re-assert the
; saved loadout immediately instead of waiting for the slow poll, so the wrong
; outfit never lingers for a few seconds first.
Event OnLocationChange(Location akOldLoc, Location akNewLoc)
    DYF_Main main = GetOwningQuest() as DYF_Main
    if main
        main.ReassertSoon()
    endif
EndEvent

Event OnCellAttach()
    DYF_Main main = GetOwningQuest() as DYF_Main
    if main
        main.ReassertSoon()
    endif
EndEvent
