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
#define STAT_PULLER_VERSION "5.0"

// full file path to python script ex: "C:\\Users\\(user)\\Desktop\\StatPuller-Build-Match-Summary\\"
#define PYTHON_SCRIPT_PATH "C:\\Users\\harri\\Desktop\\StatPuller-Build-Match-Summary\\"

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

	gameWrapper->HookEventWithCallerPost<ServerWrapper>(
		"Function TAGame.GFxHUD_TA.HandleStatTickerMessage",
		[this](ServerWrapper caller, void* params, std::string eventname) {
			onStatTickerMessage(params);
		});

	gameWrapper->HookEvent(
		"Function TAGame.GameEvent_Soccar_TA.OnGameTimeUpdated",
		[this](std::string eventName) {
			UpdateClock();
		}
	);
}

void StatPullerPlugin::OnMatchStarted(std::string eventName)
{
	simulatedClock = 300;
	goalEvents.clear();

	gameWrapper->SetTimeout([this](GameWrapper*) 
	{
		if (!gameWrapper->IsInOnlineGame() || gameWrapper->IsInReplay())
		{
			Log("StatPuller: Ignored OnMatchStarted because it's not an online match.");
			return;
		}

		ServerWrapper game = gameWrapper->GetOnlineGame();
		playlist = game.GetPlaylist().GetPlaylistId();

		if (playlist != 10 && playlist != 11 ) {
			Log("StatPuller: Not ranked 1v1 nor 2v2. Skipping.");
			return;
		}

		isReplaySaved = false;
		isMatchInProgress = true;
		wasEarlyExit = false;
		mmrBefore = -1;
		mmrAfter = -1;

		gameWrapper->SetTimeout([this](GameWrapper*) 
		{
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

	if (playlist != 10 && playlist != 11) return;

	isReplaySaved = true;
	isMatchInProgress = false;

	TrySaveReplay(server, wasEarlyExit ? "early-exit" : "match-end");

	gameWrapper->SetTimeout([this](GameWrapper*) 
	{
		mmrAfter = gameWrapper->GetMMRWrapper().GetPlayerMMR(gameWrapper->GetUniqueID(), playlist);
		json localMatchStats;
		localMatchStats["Version"] = STAT_PULLER_VERSION;
		localMatchStats["MMR_Before"] = mmrBefore;
		localMatchStats["MMR_After"] = mmrAfter;
		localMatchStats["Goals"] = goalEvents;
		localMatchStats["Playlist"] = playlist;

		SaveMatchDataToFile(localMatchStats);
		RunPythonScript("build_summary.py");

		Log("StatPuller: Match data saved and uploaded.");
	}, 0.2f);


}

void StatPullerPlugin::onStatTickerMessage(void* params)
{
	StatTickerParams* pStruct = (StatTickerParams*)params;
	StatEventWrapper statEvent = StatEventWrapper(pStruct->StatEvent);
	PriWrapper receiver = PriWrapper(pStruct->Receiver);


	if (statEvent.GetEventName() == "Goal") 
	{
		if (!isMatchInProgress) {
			Log("StatPuller: Not an online game.");
			return;
		}

		if (!receiver || receiver.IsNull()) 
		{
			Log("StatPuller: Receiver PRI is null.");
			return;
		}

		std::string scorerName = receiver.GetPlayerName().ToString();
		int teamNum = receiver.GetTeamNum(); // 0 = blue, 1 = orange
		Log("Goal scored by: " + scorerName + " on team " + std::to_string(teamNum) + " at " + std::to_string(simulatedClock));

		json goal;
		goal["ScorerName"] = scorerName;
		goal["ScorerTeam"] = teamNum;
		goal["GoalTimeSeconds"] = simulatedClock;

		goalEvents.push_back(goal);

		if (receiver.IsLocalPlayerPRI())
		{
			gameWrapper->SetTimeout([this](GameWrapper*)
				{
					RunPythonScript("clip.py");
					Log("StatPuller: Local player scored. Clipping.");
				}, 2.0f);
		}
	}
}

void StatPullerPlugin::UpdateClock() {  
	simulatedClock -= 1;
}

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

	Log("StatPuller: Replay saved successfully: " + replayPath);
}

void StatPullerPlugin::RunPythonScript(const std::string& scriptFileName) {
	std::string scriptPath = "\"" + std::string(PYTHON_SCRIPT_PATH) + scriptFileName + "\"";
	std::wstring wScriptPath(scriptPath.begin(), scriptPath.end());
	Log("Calling Python script: " + scriptPath);

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