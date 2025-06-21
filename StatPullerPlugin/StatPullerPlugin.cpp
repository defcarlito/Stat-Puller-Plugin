#include "pch.h"  
#include "StatPullerPlugin.h"  

#include <fstream>
#include "json.hpp"
using json = nlohmann::json;

#include <filesystem>
namespace fs = std::filesystem;

#include "bakkesmod/wrappers/MMRWrapper.h"

#include <chrono>
#include <iomanip>
#include <sstream>

#include <windows.h>
#include <shellapi.h>
#include <thread>

#include <set>

// version:
// major: changes to exported .json data structure, new data fields
// minor: patch, bug fixes, small changes
#define STAT_PULLER_VERSION "4.0"

// file path to python script: 
#define PYTHON_SCRIPT_PATH "C:\\Users\\harri\\Desktop\\StatPuller-write-to-firebase\\"

BAKKESMOD_PLUGIN(StatPullerPlugin, "Stat Puller Plugin", STAT_PULLER_VERSION, PERMISSION_ALL)

void StatPullerPlugin::onLoad() {
	this->Log("StatPullerPlugin: Loaded Successfully!");

	this->LoadHooks();
}

void StatPullerPlugin::onUnload() 
{

}

void StatPullerPlugin::LoadHooks() 
{
	// Function TAGame.PRI_TA.OnScoredGoal - best way to get on goal scored event?

	gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.OnAllTeamsCreated", std::bind(&StatPullerPlugin::OnMatchStarted, this, std::placeholders::_1));
	gameWrapper->HookEventWithCaller<ServerWrapper>(
		"Function TAGame.GameEvent_Soccar_TA.EventMatchEnded",
		std::bind(&StatPullerPlugin::OnGameComplete,
			this,
			std::placeholders::_1,
			std::placeholders::_2,
			std::placeholders::_3)
	);

	gameWrapper->HookEventWithCaller<ServerWrapper>(
		"Function TAGame.GameEvent_Soccar_TA.Destroyed",
		std::bind(&StatPullerPlugin::OnGameComplete,
			this,
			std::placeholders::_1,
			std::placeholders::_2,
			std::placeholders::_3)
	);
}

void StatPullerPlugin::OnMatchStarted(std::string eventName)
{
	gameWrapper->SetTimeout([this](GameWrapper*) 
	{
		if (!gameWrapper->IsInOnlineGame() || gameWrapper->IsInReplay())
		{
			Log("StatPuller: Ignored OnMatchStarted because it's not an online match.");
			return;
		}

		isReplaySaved = false;
		isMatchInProgress = true;
		wasEarlyExit = false;
		mmrBefore = -1;
		mmrAfter = -1;

		gameWrapper->SetTimeout([this](GameWrapper*) 
		{
			const int playlist = gameWrapper->GetMMRWrapper().GetCurrentPlaylist();
			const UniqueIDWrapper uid = gameWrapper->GetUniqueID();
			mmrBefore = gameWrapper->GetMMRWrapper().GetPlayerMMR(uid, playlist);
		}, 1.0f);

		Log("StatPuller: Match has started.");
	}, 3.0f);
}

void StatPullerPlugin::OnGameComplete(ServerWrapper server,
	void*,
	std::string eventName)
{
	if (!isMatchInProgress || isReplaySaved || (gameWrapper->IsInReplay())) return;
	isReplaySaved = true;
	isMatchInProgress = false;

	// export the replay while 'server' is still alive
	TrySaveReplay(server, wasEarlyExit ? "early-exit" : "match-end");

	gameWrapper->SetTimeout([this](GameWrapper*) 
	{
		const int playlist = gameWrapper->GetMMRWrapper().GetCurrentPlaylist();
		mmrAfter = gameWrapper->GetMMRWrapper().GetPlayerMMR(gameWrapper->GetUniqueID(), playlist);
		json localMatchStats;
		localMatchStats["version"] = STAT_PULLER_VERSION;
		localMatchStats["mmr_before"] = mmrBefore;
		localMatchStats["mmr_after"] = mmrAfter;

		Log("MMR Before Match: " + std::to_string(mmrBefore));
		Log("MMR After Match: " + std::to_string(mmrAfter));

		SaveMatchDataToFile(localMatchStats);
		RunFirebaseUploadScript();

		Log("StatPuller: Match data saved and uploaded.");
	}, 0.2f);


}

struct PriRemovedParams {
	uintptr_t PRI;
};

void StatPullerPlugin::SaveMatchDataToFile(const json& wrapped) {
	std::string folder = PYTHON_SCRIPT_PATH;
	std::string path = folder + "last-match-stats.json";
	std::ofstream file(path, std::ofstream::trunc);
	file << wrapped.dump(4);
	file.close();
}

void StatPullerPlugin::TrySaveReplay(ServerWrapper server, const std::string& label)
{
	if (server.IsNull()) {
		Log("TrySaveReplay: Server is null, skipping replay save.");
		return;
	}

	ReplayDirectorWrapper replayDirector = server.GetReplayDirector();
	if (replayDirector.IsNull()) {
		Log("TrySaveReplay: ReplayDirector is null.");
		return;
	}

	ReplaySoccarWrapper soccarReplay = replayDirector.GetReplay();
	if (soccarReplay.memory_address == NULL) {
		Log("TrySaveReplay: Replay object is null.");
		return;
	}

	soccarReplay.StopRecord();

	std::string replayName = "last-match-replay";
	std::string replayPath = fs::path(PYTHON_SCRIPT_PATH + replayName + ".replay").string();

	soccarReplay.ExportReplay(replayPath);

	// Optional: confirm file creation
	if (fs::exists(replayPath)) {
		Log("Replay saved successfully: " + replayPath);
	}
	else {
		Log("Replay export failed or was not saved to expected path.");
	}
}

void StatPullerPlugin::RunFirebaseUploadScript() {
    std::string scriptPath = std::string(PYTHON_SCRIPT_PATH) + "main.py";
	std::wstring wScriptPath(scriptPath.begin(), scriptPath.end());

	std::thread([wScriptPath] {

		ShellExecute(
			nullptr,
			L"open",
			L"pythonw.exe",
			wScriptPath.c_str(),
			nullptr,
			SW_HIDE
		);
		}).detach();
}

void StatPullerPlugin::Log(std::string msg) {
	cvarManager->log(msg);
}