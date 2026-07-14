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
