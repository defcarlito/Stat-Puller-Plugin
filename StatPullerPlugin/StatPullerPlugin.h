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

	void OnGoalScored(std::string name);

	void OnReplayStart(ServerWrapper caller, void* params, std::string eventName);
	void OnReplayEnd(std::string eventName);

	void OnMatchEnded(std::string eventName);
	void OnMatchStarted(std::string eventName);

	void OnPlayerRemoved(ServerWrapper server, void* params, std::string eventName);

	void SaveMatchDataToFile(const json& wrapped);
	void RunFirebaseUploadScript();

private:

	void Log(std::string msg);
	int mmrAfter = -1;
	int mmrBefore = -1;

	std::string playlistType;

	long long matchStartUnix = 0;
	long long matchEndUnix = 0;

	bool hasLocalPlayerLeftEarly = false;


	// bools used to prevent repeat calls
	bool isMatchInProgress = false;
	bool isInReplay = false;
	bool hasSavedMatchData = false;

	std::unordered_map<std::string, json> cachedPlayerStats;

	
};

