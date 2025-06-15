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

// TODO: later add feature that clips last 15 seconds at a goal


BAKKESMOD_PLUGIN(StatPullerPlugin, "Stat Puller Plugin", "1.0", PERMISSION_ALL)  

void StatPullerPlugin::onLoad() {  
    this->Log("StatPullerPlugin: Loaded Successfully!");  

    this->LoadHooks();  
}  

void StatPullerPlugin::onUnload() {  

}  

void StatPullerPlugin::LoadHooks() {  

	// runs when replay starts
    gameWrapper->HookEventWithCaller<ServerWrapper>(  
        "Function GameEvent_Soccar_TA.ReplayPlayback.BeginState",  
        std::bind(&StatPullerPlugin::OnReplayStart, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)  
    );  

   // runs when match ends
    gameWrapper->HookEvent("Function TAGame.AchievementManager_TA.HandleMatchEnded", std::bind(&StatPullerPlugin::OnMatchEnded, this, std::placeholders::_1));  

	// runs when goal is scored anytime (anytime the ball explodes)
    gameWrapper->HookEvent("Function TAGame.Ball_TA.Explode", std::bind(&StatPullerPlugin::OnGoalScored, this, std::placeholders::_1));

	// runs on match start
	gameWrapper->HookEvent("Function TAGame.Team_TA.PostBeginPlay", std::bind(&StatPullerPlugin::OnMatchStarted, this, std::placeholders::_1));

	// runs when replay ends
	gameWrapper->HookEvent("Function GameEvent_Soccar_TA.ReplayPlayback.EndState", std::bind(&StatPullerPlugin::OnReplayEnd, this, std::placeholders::_1));

}

// gets the current date and time when the match starts
void StatPullerPlugin::OnMatchStarted(std::string eventName) {
	if (isMatchInProgress) return; // prevent multiple calls
	isMatchInProgress = true;

	Log("StatPuller: Match has started.");

	// get current time (before match)
	auto now = std::chrono::system_clock::now();
	std::time_t now_time = std::chrono::system_clock::to_time_t(now);
	matchStartUnix = static_cast<long long>(now_time); // store unix timestamp of match start

	UniqueIDWrapper uid = gameWrapper->GetUniqueID();
	this->mmrBefore = gameWrapper->GetMMRWrapper().GetPlayerMMR(uid, 10);
}

void StatPullerPlugin::OnMatchEnded(std::string name) {

	// reset state variables
	isMatchInProgress = false; // indicate that the match has ended
	isInReplay = false; // reset replay state

	gameWrapper->SetTimeout([this](GameWrapper*) {

		// check if we are in a game
		ServerWrapper game = gameWrapper->GetOnlineGame();
		if (game.IsNull()) {
			Log("StatPuller: No online game found.");
			return;
		}

		// check if the match is a ranked 1v1 match
		int playlistId = game.GetPlaylist().GetPlaylistId();
		if (playlistId != 10) { // TODO: later include other playlists
			Log("StatPuller: Not ranked 1v1. Skipping.");
			return;
		}
		
		playlistType = "1s";

		// check if the match is a private match
		ArrayWrapper<PriWrapper> pris = game.GetPRIs();
		if (pris.Count() == 0) {
			Log("StatPuller: No player stats found.");
			return;
		}



		// get current time (after match)
		auto now = std::chrono::system_clock::now();
		std::time_t now_time = std::chrono::system_clock::to_time_t(now);
		matchEndUnix = static_cast<long long>(now_time); // store unix timestamp of match end

		// get my mmr after the match
		UniqueIDWrapper uid = gameWrapper->GetUniqueID();
		int mmrAfter = gameWrapper->GetMMRWrapper().GetPlayerMMR(uid, 10);

		json fullMatchData;
		fullMatchData["version"] = "1.0";
		fullMatchData["match_start"] = matchStartUnix;
		fullMatchData["match_end"] = matchEndUnix;
		fullMatchData["players"] = json::array(); // init players array
		fullMatchData["playlist"] = playlistType; // "1s"

		std::set<std::string> presentPlayers;
		for (int i = 0; i < pris.Count(); ++i) {
			PriWrapper pri = pris.Get(i);
			if (pri.IsNull()) continue;

			std::string playerName = pri.GetPlayerName().ToString();
			presentPlayers.insert(playerName);

			int team = pri.GetTeamNum();
			int goals = pri.GetMatchGoals();
			int assists = pri.GetMatchAssists();
			int saves = pri.GetMatchSaves();
			int shots = pri.GetMatchShots();
			int score = pri.GetMatchScore();

			json playerData = {
				{"player", playerName},
				{"team", team},
				{"goals", goals},
				{"assists", assists},
				{"saves", saves},
				{"shots", shots},
				{"score", score}
			};

			// only set mmr for me
			if (pri.IsLocalPlayerPRI()) {
				playerData["mmr_before"] = mmrBefore;
				playerData["mmr_after"] = mmrAfter;
			}

			// check cached stats for any missing players
			fullMatchData["players"].push_back(playerData);
		}

		for (const auto& pair : cachedPlayerStats) {
			std::string cachedName = pair.first;

			if (presentPlayers.find(cachedName) == presentPlayers.end()) {
				Log("StatPuller: Using cached stats for missing player: " + cachedName);
				fullMatchData["players"].push_back(pair.second);
			}
		}

		json wrapped;
		wrapped[std::to_string(matchStartUnix)] = fullMatchData;

		// write match stats .json to desktop folder
        std::string folder = "C:\\Users\\harri\\Desktop\\StatPuller-write-to-firebase";
		std::string path = folder + "\\" + "last-match-stats.json";
		std::ofstream file(path);
		file << wrapped.dump(4);
		file.close();


		/*Log("StatPuller: Saved match stats to: " + path);*/
		Log("StatPuller: Running firebase script.");

		// run python script in the background that saves match data to firebase
		std::thread([] {
			ShellExecute(
				nullptr,				
				L"open",                   
				L"pythonw.exe",            // use pythonw.exe to avoid console window
				L"\"C:\\Users\\harri\\Desktop\\StatPuller-write-to-firebase\\main.py\"",
				nullptr,                  
				SW_HIDE                   // hide window to avoid stealing focus from the game
			);
			}).detach();

		Log("StatPuller: Successfully ran firebase script.");

		cachedPlayerStats.clear();
		Log("Cleared cached player stats");

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

// this function caches player stats when a goal is scored
// this is done incase they leave before the match ends so we can still get their stats
void StatPullerPlugin::OnGoalScored(std::string name) {

	// goal explodes are logged ANYTIME a ball explodes including during replays
	// we only want to catch goals during the match, not during any kind of replay during the game or i nafter-match highlights
	if (isMatchInProgress == false) return; // return if the back has ended
	if (isInReplay) return; // ignore ball explodes during replays


	Log("Stat Puller: GOAL SCORED!");

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

void StatPullerPlugin::Log(std::string msg) {  
    cvarManager->log(msg);  
}