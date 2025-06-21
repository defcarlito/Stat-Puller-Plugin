#pragma once  

#include "bakkesmod/plugin/bakkesmodplugin.h"  
#include "bakkesmod/wrappers/GameObject/Stats/StatEventWrapper.h"   

#include "json.hpp"  
using json = nlohmann::json;  

#pragma comment ( lib, "pluginsdk.lib" )  

struct StatTickerParams {  
    uintptr_t Receiver;  
    uintptr_t Victim;  
    uintptr_t StatEvent;  
};  

struct StatEventParams {  
    uintptr_t PRI;  
    uintptr_t StatEvent;  
};  

class StatPullerPlugin : public BakkesMod::Plugin::BakkesModPlugin  
{  
public:  
    virtual void onLoad() override;  
    virtual void onUnload() override;  

    void LoadHooks();  

    void OnMatchStarted(std::string eventName);  
    void OnGameComplete(ServerWrapper server,  
        void* params,  
        std::string   eventName);  

    void onStatTickerMessage(void* params);  
    void UpdateClock();

    void SaveMatchDataToFile(const json& wrapped);  
    void RunFirebaseUploadScript();
    void TrySaveReplay(ServerWrapper server, const std::string& label);

private:  
    void Log(std::string msg);  

    std::vector<json> goalEvents;

    int mmrAfter = -1;  
    int mmrBefore = -1;  

    int simulatedClock = 300;
    int playlist = -1;

    bool isReplaySaved = false;  
    bool wasEarlyExit = false;  
    bool isMatchInProgress = false;  
};
