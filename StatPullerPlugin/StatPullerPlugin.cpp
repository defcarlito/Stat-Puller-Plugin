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
#define STAT_PULLER_VERSION "2.0"

BAKKESMOD_PLUGIN(StatPullerPlugin, "Stat Puller Plugin", STAT_PULLER_VERSION, PERMISSION_ALL)

void StatPullerPlugin::onLoad() {
	this->Log("StatPullerPlugin: Loaded Successfully!");

	this->LoadHooks();
}

void StatPullerPlugin::onUnload() {

}

void StatPullerPlugin::LoadHooks() {
	// Function TAGame.PRI_TA.OnScoredGoal - best way to get on goal scored event?

	gameWrapper->HookEvent("Function TAGame.AchievementManager_TA.HandleMatchEnded", std::bind(&StatPullerPlugin::OnMatchEnded, this, std::placeholders::_1));
	gameWrapper->HookEvent("Function TAGame.Team_TA.PostBeginPlay", std::bind(&StatPullerPlugin::OnMatchStarted, this, std::placeholders::_1));
	gameWrapper->HookEventWithCaller<ServerWrapper>(
		"Function TAGame.GameEvent_TA.EventPlayerRemoved",
		std::bind(&StatPullerPlugin::OnPlayerRemoved, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)
	);
}

struct PriRemovedParams {
	uintptr_t PRI;
};

void StatPullerPlugin::OnPlayerRemoved(ServerWrapper server, void* params, std::string eventName) {
	auto pri = PriWrapper(static_cast<PriRemovedParams*>(params)->PRI);
	if (!isMatchInProgress || pri.IsNull()) return;

	if (pri.IsLocalPlayerPRI()) {
		Log("StatPuller: Local player was removed early, running OnMatchEnd now.");
		OnMatchEnded("LocalPlayerLeftEarly");
	}
}

void StatPullerPlugin::OnMatchStarted(std::string eventName) {
	if (isMatchInProgress) return;
	isMatchInProgress = true;

	gameWrapper->SetTimeout([this](GameWrapper*) {
		if (!gameWrapper->IsInOnlineGame()) {
			Log("StatPuller: Ignored OnMatchStarted because it's not an online match.");
			isMatchInProgress = false;
			return;
		}

		hasSavedMatchData = false;

		// wait briefly before pulling MMR to ensure it's accurate
		gameWrapper->SetTimeout([this](GameWrapper*) {
			UniqueIDWrapper uid = gameWrapper->GetUniqueID();
			this->mmrBefore = gameWrapper->GetMMRWrapper().GetPlayerMMR(uid, 10);
		}, 1.0f); // inner 1s delay for MMR

		Log("StatPuller: Match has started.");
	}, 3.0f);
}

void StatPullerPlugin::OnMatchEnded(std::string name) {
	if (hasSavedMatchData) {
		Log("StatPuller: Match data already saved. Skipping duplicate OnMatchEnded call.");
		return;
	}
	Log("StatPuller: OnMatchEnded triggered.");
	hasSavedMatchData = true;
	isMatchInProgress = false;

	gameWrapper->SetTimeout([this](GameWrapper*) {

		ServerWrapper game = gameWrapper->GetOnlineGame();
		if (game.IsNull() || game.GetPlaylist().GetPlaylistId() != 10) return;
		int mmrAfter = gameWrapper->GetMMRWrapper().GetPlayerMMR(gameWrapper->GetUniqueID(), 10);

		json localMatchStats;
		localMatchStats["version"] = STAT_PULLER_VERSION;
		localMatchStats["mmr_before"] = mmrBefore;
		localMatchStats["mmr_after"] = mmrAfter;

		SaveMatchDataToFile(localMatchStats);
		RunFirebaseUploadScript();

		Log("StatPuller: Match data saved and uploaded.");
	}, 0.2f);
}


void StatPullerPlugin::SaveMatchDataToFile(const json& wrapped) {
	std::string folder = "C:\\Users\\harri\\Desktop\\StatPuller-write-to-firebase";
	std::string path = folder + "\\last-match-stats.json";
	std::ofstream file(path, std::ofstream::trunc);
	file << wrapped.dump(4);
	file.close();
}

void StatPullerPlugin::RunFirebaseUploadScript() {
	std::thread([] {
		ShellExecute(
			nullptr,
			L"open",
			L"pythonw.exe",
			L"\"C:\\Users\\harri\\Desktop\\StatPuller-write-to-firebase\\main.py\"",
			nullptr,
			SW_HIDE
		);
		}).detach();
}

void StatPullerPlugin::Log(std::string msg) {
	cvarManager->log(msg);
}