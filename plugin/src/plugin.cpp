#include "Plugin.h"

void OnMessage(SKSE::MessagingInterface::Message* a_message) {
    switch (a_message->type) {
        // PrismaUI.dll is loaded by kPostLoad; its view system is ready by
        // kDataLoaded, which is when we create our view.
        case SKSE::MessagingInterface::kDataLoaded:
            Dresser::Init();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            Dresser::OnGameLoaded();
            break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    SetupLog();
    Dresser::InstallHooks();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    SKSE::GetPapyrusInterface()->Register(Dresser::RegisterPapyrus);
    logger::info("Dress Your Followers (C++) loaded");
    return true;
}
