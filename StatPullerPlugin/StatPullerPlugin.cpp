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

#define PLAYLIST_TYPE "1s" // playlist type, currently only supports 1v1 ranked matches

BAKKESMOD_PLUGIN(StatPullerPlugin, "Stat Puller Plugin", STAT_PULLER_VERSION, PERMISSION_ALL)

void StatPullerPlugin::onLoad() {
	this->Log("StatPullerPlugin: Loaded Successfully!");

	this->LoadHooks();
}

void StatPullerPlugin::onUnload() {

}

void StatPullerPlugin::LoadHooks() {
	// Function TAGame.PRI_TA.OnScoredGoal - best way to get on goal scored event?

	gameWrapper->HookEvent("Function TAGame.GFxHUD_TA.HandleStatTickerMessage", std::bind(&StatPullerPlugin::OnStatUpdated, this, std::placeholders::_1));
	gameWrapper->HookEvent("Function TAGame.Car_TA.OnHitBall", std::bind(&StatPullerPlugin::OnStatUpdated, this, std::placeholders::_1));

	gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.EndState", std::bind(&StatPullerPlugin::OnReplayEnd, this, std::placeholders::_1));
	gameWrapper->HookEventWithCaller<ServerWrapper>(
		"Function GameEvent_Soccar_TA.ReplayPlayback.BeginState",
		std::bind(&StatPullerPlugin::OnReplayStart, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)
	);
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

void StatPullerPlugin::OnStatUpdated(std::string name) {
	if (!isMatchInProgress) return;

	Log("StatPuller: OnStatUpdated called");

	ServerWrapper game = gameWrapper->GetOnlineGame();
	if (game.IsNull()) return;

	ArrayWrapper<PriWrapper> pris = game.GetPRIs();
	for (int i = 0; i < pris.Count(); ++i) {
		PriWrapper pri = pris.Get(i);
		if (pri.IsNull()) continue;

		std::string playerName = pri.GetPlayerName().ToString();
		json playerData = {
			{"player", playerName},
			{"team", pri.GetTeamNum()},
			{"goals", pri.GetMatchGoals()},
			{"assists", pri.GetMatchAssists()},
			{"saves", pri.GetMatchSaves()},
			{"shots", pri.GetMatchShots()},
			{"score", pri.GetMatchScore()}
		};

		cachedPlayerStats[playerName] = playerData;
	}
}

void StatPullerPlugin::OnPlayerRemoved(ServerWrapper server, void* params, std::string eventName) {
	auto pri = PriWrapper(static_cast<PriRemovedParams*>(params)->PRI);
	if (!isMatchInProgress) {
		Log("Match is over, skipping player removal event.");
		return;
	}
	if (pri.IsNull()) {
		Log("StatPuller: Player removed but PRI is null. Skipping.");
		return;
	}

	std::string playerName = pri.GetPlayerName().ToString();
	Log("StatPuller: Caching stats, player left early");

	json playerData = {
		{"player", playerName},
		{"team", pri.GetTeamNum()},
		{"goals", pri.GetMatchGoals()},
		{"assists", pri.GetMatchAssists()},
		{"saves", pri.GetMatchSaves()},
		{"shots", pri.GetMatchShots()},
		{"score", pri.GetMatchScore()}
	};

	cachedPlayerStats[playerName] = playerData;
	if (pri.IsLocalPlayerPRI()) {
		hasMatchEndedEarly = true;
		Log("StatPuller: Local player was removed");
		OnMatchEnded("LocalPlayerLeftEarly"); // trigger match end logic
	}
}

// gets the current date and time when the match starts
void StatPullerPlugin::OnMatchStarted(std::string eventName) {
	if (isMatchInProgress) return;
	isMatchInProgress = true;

	gameWrapper->SetTimeout([this](GameWrapper*) {
		if (!gameWrapper->IsInOnlineGame()) {
			Log("StatPuller: Ignored OnMatchStarted because it's not an online match.");
			isMatchInProgress = false; // reset so it can run again if needed
			return;
		}

		hasSavedMatchData = false;
		hasMatchEndedEarly = false;
		cachedPlayerStats.clear();

		auto now = std::chrono::system_clock::now();
		std::time_t now_time = std::chrono::system_clock::to_time_t(now);
		matchStartUnix = static_cast<long long>(now_time);

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
	isInReplay = false;


	gameWrapper->SetTimeout([this](GameWrapper*) {
		auto now = std::chrono::system_clock::now();
		matchEndUnix = static_cast<long long>(std::chrono::system_clock::to_time_t(now));

		json fullMatchData;
		fullMatchData["version"] = STAT_PULLER_VERSION;
		fullMatchData["match_start"] = matchStartUnix;
		fullMatchData["match_end"] = matchEndUnix;
		fullMatchData["playlist"] = PLAYLIST_TYPE;
		fullMatchData["players"] = json::array();
		int winningTeam = -1;

		// Pull fresh data again
		ServerWrapper game = gameWrapper->GetOnlineGame();
		if (!game.IsNull()) {

			if (game.GetPlaylist().GetPlaylistId() != 10) {
				Log("StatPuller: Not ranked 1v1. Skipping.");
				return;
			}

			TeamWrapper winningTeamWrapper = game.GetWinningTeam();
			if (!winningTeamWrapper.IsNull()) {
				winningTeam = winningTeamWrapper.GetTeamIndex();
			}
			else {
				Log("StatPuller: Winning team is null. Defaulting to -1.");
			}

			ArrayWrapper<PriWrapper> pris = game.GetPRIs();
			for (int i = 0; i < pris.Count(); ++i) {
				PriWrapper pri = pris.Get(i);
				if (pri.IsNull()) continue;

				int team = pri.GetTeamNum();
				std::string playerName = pri.GetPlayerName().ToString();
				json playerData = {
					{"player", playerName},
					{"team", pri.GetTeamNum()},
					{"goals", pri.GetMatchGoals()},
					{"assists", pri.GetMatchAssists()},
					{"saves", pri.GetMatchSaves()},
					{"shots", pri.GetMatchShots()},
					{"score", pri.GetMatchScore()},
				};

				if (pri.IsLocalPlayerPRI()) {
					playerData["mmr_before"] = mmrBefore;
					UniqueIDWrapper uid = gameWrapper->GetUniqueID();
					int mmrAfter = gameWrapper->GetMMRWrapper().GetPlayerMMR(uid, 10);
					playerData["mmr_after"] = mmrAfter;
				}
				fullMatchData["players"].push_back(playerData);
			}
		}

		// Include any disconnected players
		for (const auto& [cachedName, cachedData] : cachedPlayerStats) {
			bool alreadyIncluded = false;
			for (const auto& entry : fullMatchData["players"]) {
				if (entry["player"] == cachedName) {
					alreadyIncluded = true;
					break;
				}
			}
			if (!alreadyIncluded) {
				Log("StatPuller: Using cached stats for missing player: " + cachedName);
				fullMatchData["players"].push_back(cachedData);
			}
		}
		fullMatchData["winning_team"] = winningTeam;
		json wrapped;
		wrapped[std::to_string(matchStartUnix)] = fullMatchData;

		SaveMatchDataToFile(wrapped);
		RunFirebaseUploadScript();
		cachedPlayerStats.clear();

		Log("StatPuller: Match data saved and uploaded.");
		}, 0.2f);
}


void StatPullerPlugin::OnReplayEnd(std::string name) {
	isInReplay = false;
	Log("StatPuller: Replay has ended.");
}

void StatPullerPlugin::OnReplayStart(ServerWrapper caller, void* params, std::string eventName)
{
	isInReplay = true;

	ReplayDirectorWrapper rd = caller.GetReplayDirector();
	if (rd.IsNull()) {
		Log("StatPuller: ReplayDirector is null");
		return;
	}

	ReplayScoreData data = rd.GetReplayScoreData();

	PriWrapper scorer((uintptr_t)data.ScoredBy);
	if (scorer.IsNull()) {
		Log("StatPuller: Scorer is null");
		return;
	}

	std::string scorerName = scorer.GetPlayerName().ToString();
	int team = scorer.GetTeamNum();

	Log("StatPuller: Scored by: " + scorerName + " on team " + std::to_string(team));
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