#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"

#include "json.hpp"
using json = nlohmann::json;

#pragma comment ( lib, "pluginsdk.lib" )

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
	void OnPlayerRemoved(ServerWrapper server, void* params, std::string eventName);

	void SaveMatchDataToFile(const json& wrapped);
	void RunFirebaseUploadScript();
	void TrySaveReplay(ServerWrapper server, const std::string& label);

private:

	void Log(std::string msg);
	int mmrAfter = -1;
	int mmrBefore = -1;

	bool isReplaySaved = false;
	bool wasEarlyExit = false;
	bool isMatchInProgress = false;

	
};



