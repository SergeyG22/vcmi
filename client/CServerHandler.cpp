/*
 * CServerHandler.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "CServerHandler.h"
#include "Client.h"
#include "CGameInfo.h"
#include "CPlayerInterface.h"
#include "gui/CGuiHandler.h"

#include "lobby/CSelectionBase.h"
#include "lobby/CLobbyScreen.h"
#include "windows/InfoWindows.h"

#include "mainmenu/CMainMenu.h"

#ifdef VCMI_ANDROID
#include "../lib/CAndroidVMHelper.h"
#elif defined(VCMI_IOS)
#include "ios/utils.h"
#include <dispatch/dispatch.h>
#else
#include "../lib/Interprocess.h"
#endif

#ifdef SINGLE_PROCESS_APP
#include "../server/CVCMIServer.h"
#endif

#include "../lib/CConfigHandler.h"
#include "../lib/CGeneralTextHandler.h"
#include "../lib/CThreadHelper.h"
#include "../lib/NetPacks.h"
#include "../lib/StartInfo.h"
#include "../lib/VCMIDirs.h"
#include "../lib/mapping/CCampaignHandler.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapping/CMapInfo.h"
#include "../lib/mapObjects/MiscObjects.h"
#include "../lib/rmg/CMapGenOptions.h"
#include "../lib/registerTypes/RegisterTypes.h"
#include "../lib/serializer/Connection.h"
#include "../lib/serializer/CMemorySerializer.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include "../lib/serializer/Cast.h"

#include <vcmi/events/EventBus.h>

#ifdef VCMI_WINDOWS
#include <windows.h>
#endif

template<typename T> class CApplyOnLobby;

const std::string CServerHandler::localhostAddress{"127.0.0.1"};

#ifdef VCMI_ANDROID
extern std::atomic_bool androidTestServerReadyFlag;
#endif

class CBaseForLobbyApply
{
public:
	virtual bool applyOnLobbyHandler(CServerHandler * handler, void * pack) const = 0;
	virtual void applyOnLobbyScreen(CLobbyScreen * lobby, CServerHandler * handler, void * pack) const = 0;
	virtual ~CBaseForLobbyApply(){};
	template<typename U> static CBaseForLobbyApply * getApplier(const U * t = nullptr)
	{
		return new CApplyOnLobby<U>();
	}
};

template<typename T> class CApplyOnLobby : public CBaseForLobbyApply
{
public:
	bool applyOnLobbyHandler(CServerHandler * handler, void * pack) const override
	{
		T * ptr = static_cast<T *>(pack);
		logNetwork->trace("\tImmediately apply on lobby: %s", typeList.getTypeInfo(ptr)->name());
		return ptr->applyOnLobbyHandler(handler);
	}

	void applyOnLobbyScreen(CLobbyScreen * lobby, CServerHandler * handler, void * pack) const override
	{
		T * ptr = static_cast<T *>(pack);
		logNetwork->trace("\tApply on lobby from queue: %s", typeList.getTypeInfo(ptr)->name());
		ptr->applyOnLobbyScreen(lobby, handler);
	}
};

template<> class CApplyOnLobby<CPack>: public CBaseForLobbyApply
{
public:
	bool applyOnLobbyHandler(CServerHandler * handler, void * pack) const override
	{
		logGlobal->error("Cannot apply plain CPack!");
		assert(0);
		return false;
	}

	void applyOnLobbyScreen(CLobbyScreen * lobby, CServerHandler * handler, void * pack) const override
	{
		logGlobal->error("Cannot apply plain CPack!");
		assert(0);
	}
};

extern std::string NAME;

CServerHandler::CServerHandler()
	: state(EClientState::NONE), mx(std::make_shared<boost::recursive_mutex>()), client(nullptr), loadMode(0), campaignStateToSend(nullptr), campaignServerRestartLock(false)
{
	uuid = boost::uuids::to_string(boost::uuids::random_generator()());
	//read from file to restore last session
	if(!settings["server"]["uuid"].isNull() && !settings["server"]["uuid"].String().empty())
		uuid = settings["server"]["uuid"].String();
	applier = std::make_shared<CApplier<CBaseForLobbyApply>>();
	registerTypesLobbyPacks(*applier);
}

void CServerHandler::resetStateForLobby(const StartInfo::EMode mode, const std::vector<std::string> * names)
{
	hostClientId = -1;
	state = EClientState::NONE;
	th = std::make_unique<CStopWatch>();
	packsForLobbyScreen.clear();
	c.reset();
	si = std::make_shared<StartInfo>();
	playerNames.clear();
	si->difficulty = 1;
	si->mode = mode;
	myNames.clear();
	if(names && !names->empty()) //if have custom set of player names - use it
		myNames = *names;
	else
		myNames.push_back(settings["general"]["playerName"].String());

#if !defined(VCMI_ANDROID) && !defined(SINGLE_PROCESS_APP)
	shm.reset();

	if(!settings["session"]["disable-shm"].Bool())
	{
		std::string sharedMemoryName = "vcmi_memory";
		if(settings["session"]["enable-shm-uuid"].Bool())
		{
			//used or automated testing when multiple clients start simultaneously
			sharedMemoryName += "_" + uuid;
		}
		try
		{
			shm = std::make_shared<SharedMemory>(sharedMemoryName, true);
		}
		catch(...)
		{
			shm.reset();
			logNetwork->error("Cannot open interprocess memory. Continue without it...");
		}
	}
#endif
}

void CServerHandler::startLocalServerAndConnect()
{
	if(threadRunLocalServer)
		threadRunLocalServer->join();

	th->update();
	
	auto errorMsg = CGI->generaltexth->translate("vcmi.server.errors.existingProcess");
	try
	{
		CConnection testConnection(localhostAddress, getDefaultPort(), NAME, uuid);
		logNetwork->error("Port is busy, check if another instance of vcmiserver is working");
		CInfoWindow::showInfoDialog(errorMsg, {});
		return;
	}
	catch(...)
	{
		//no connection means that port is not busy and we can start local server
	}
	
#ifdef VCMI_ANDROID
	{
		CAndroidVMHelper envHelper;
		envHelper.callStaticVoidMethod(CAndroidVMHelper::NATIVE_METHODS_DEFAULT_CLASS, "startServer", true);
	}
#elif defined(SINGLE_PROCESS_APP)
	boost::condition_variable cond;
	std::vector<std::string> args{"--uuid=" + uuid, "--port=" + boost::lexical_cast<std::string>(getHostPort())};
	if(settings["session"]["lobby"].Bool() && settings["session"]["host"].Bool())
	{
		args.push_back("--lobby=" + settings["session"]["address"].String());
		args.push_back("--connections=" + settings["session"]["hostConnections"].String());
		args.push_back("--lobby-port=" + boost::lexical_cast<std::string>(settings["session"]["port"].Integer()));
		args.push_back("--lobby-uuid=" + settings["session"]["hostUuid"].String());
	}
	threadRunLocalServer = std::make_shared<boost::thread>([&cond, args, this] {
		setThreadName("CVCMIServer");
		CVCMIServer::create(&cond, args);
		onServerFinished();
	});
	threadRunLocalServer->detach();
#else
	threadRunLocalServer = std::make_shared<boost::thread>(&CServerHandler::threadRunServer, this); //runs server executable;
#endif
	logNetwork->trace("Setting up thread calling server: %d ms", th->getDiff());

	th->update();

#ifdef VCMI_ANDROID
	logNetwork->info("waiting for server");
	while(!androidTestServerReadyFlag.load())
	{
		logNetwork->info("still waiting...");
		boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
	}
	logNetwork->info("waiting for server finished...");
	androidTestServerReadyFlag = false;
#elif defined(SINGLE_PROCESS_APP)
	{
#ifdef VCMI_IOS
		dispatch_sync(dispatch_get_main_queue(), ^{
			iOS_utils::showLoadingIndicator();
		});
#endif

		boost::mutex m;
		boost::unique_lock<boost::mutex> lock{m};
		logNetwork->info("waiting for server");
		cond.wait(lock);
		logNetwork->info("server is ready");

#ifdef VCMI_IOS
		dispatch_sync(dispatch_get_main_queue(), ^{
			iOS_utils::hideLoadingIndicator();
		});
#endif
	}
#else
	if(shm)
		shm->sr->waitTillReady();
#endif
	logNetwork->trace("Waiting for server: %d ms", th->getDiff());

	th->update(); //put breakpoint here to attach to server before it does something stupid

#if !defined(VCMI_ANDROID) && !defined(VCMI_IOS)
	const ui16 port = shm ? shm->sr->port : 0;
#else
	const ui16 port = 0;
#endif
	justConnectToServer(localhostAddress, port);

	logNetwork->trace("\tConnecting to the server: %d ms", th->getDiff());
}

void CServerHandler::justConnectToServer(const std::string & addr, const ui16 port)
{
	state = EClientState::CONNECTING;
	while(!c && state != EClientState::CONNECTION_CANCELLED)
	{
		try
		{
			logNetwork->info("Establishing connection...");
			c = std::make_shared<CConnection>(
					addr.size() ? addr : getHostAddress(),
					port ? port : getHostPort(),
					NAME, uuid);
		}
		catch(...)
		{
			logNetwork->error("\nCannot establish connection! Retrying within 1 second");
			boost::this_thread::sleep(boost::posix_time::seconds(1));
		}
	}

	if(state == EClientState::CONNECTION_CANCELLED)
	{
		logNetwork->info("Connection aborted by player!");
		return;
	}

	c->handler = std::make_shared<boost::thread>(&CServerHandler::threadHandleConnection, this);

	if(!addr.empty() && addr != getHostAddress())
	{
		Settings serverAddress = settings.write["server"]["server"];
		serverAddress->String() = addr;
	}
	if(port && port != getHostPort())
	{
		Settings serverPort = settings.write["server"]["port"];
		serverPort->Integer() = port;
	}
}

void CServerHandler::applyPacksOnLobbyScreen()
{
	if(!c || !c->handler)
		return;

	boost::unique_lock<boost::recursive_mutex> lock(*mx);
	while(!packsForLobbyScreen.empty())
	{
		CPackForLobby * pack = packsForLobbyScreen.front();
		packsForLobbyScreen.pop_front();
		CBaseForLobbyApply * apply = applier->getApplier(typeList.getTypeID(pack)); //find the applier
		apply->applyOnLobbyScreen(static_cast<CLobbyScreen *>(SEL), this, pack);
		GH.totalRedraw();
		delete pack;
	}
}

void CServerHandler::stopServerConnection()
{
	if(c->handler)
	{
		while(!c->handler->timed_join(boost::posix_time::milliseconds(50)))
			applyPacksOnLobbyScreen();
		c->handler->join();
	}
}

std::set<PlayerColor> CServerHandler::getHumanColors()
{
	return clientHumanColors(c->connectionID);
}


PlayerColor CServerHandler::myFirstColor() const
{
	return clientFirstColor(c->connectionID);
}

bool CServerHandler::isMyColor(PlayerColor color) const
{
	return isClientColor(c->connectionID, color);
}

ui8 CServerHandler::myFirstId() const
{
	return clientFirstId(c->connectionID);
}

bool CServerHandler::isServerLocal() const
{
	if(threadRunLocalServer)
		return true;

	return false;
}

bool CServerHandler::isHost() const
{
	return c && hostClientId == c->connectionID;
}

bool CServerHandler::isGuest() const
{
	return !c || hostClientId != c->connectionID;
}

ui16 CServerHandler::getDefaultPort()
{
	return static_cast<ui16>(settings["server"]["port"].Integer());
}

std::string CServerHandler::getDefaultPortStr()
{
	return boost::lexical_cast<std::string>(getDefaultPort());
}

std::string CServerHandler::getHostAddress() const
{
	if(settings["session"]["lobby"].isNull() || !settings["session"]["lobby"].Bool())
		return settings["server"]["server"].String();
	
	if(settings["session"]["host"].Bool())
		return localhostAddress;
	
	return settings["session"]["address"].String();
}

ui16 CServerHandler::getHostPort() const
{
	if(settings["session"]["lobby"].isNull() || !settings["session"]["lobby"].Bool())
		return getDefaultPort();
	
	if(settings["session"]["host"].Bool())
		return getDefaultPort();
	
	return settings["session"]["port"].Integer();
}

void CServerHandler::sendClientConnecting() const
{
	LobbyClientConnected lcc;
	lcc.uuid = uuid;
	lcc.names = myNames;
	lcc.mode = si->mode;
	sendLobbyPack(lcc);
}

void CServerHandler::sendClientDisconnecting()
{
	// FIXME: This is workaround needed to make sure client not trying to sent anything to non existed server
	if(state == EClientState::DISCONNECTING)
		return;

	state = EClientState::DISCONNECTING;
	LobbyClientDisconnected lcd;
	lcd.clientId = c->connectionID;
	logNetwork->info("Connection has been requested to be closed.");
	if(isServerLocal())
	{
		lcd.shutdownServer = true;
		logNetwork->info("Sent closing signal to the server");
	}
	else
	{
		logNetwork->info("Sent leaving signal to the server");
	}
	sendLobbyPack(lcd);
}

void CServerHandler::setCampaignState(std::shared_ptr<CCampaignState> newCampaign)
{
	state = EClientState::LOBBY_CAMPAIGN;
	LobbySetCampaign lsc;
	lsc.ourCampaign = newCampaign;
	sendLobbyPack(lsc);
}

void CServerHandler::setCampaignMap(int mapId) const
{
	if(state == EClientState::GAMEPLAY) // FIXME: UI shouldn't sent commands in first place
		return;

	LobbySetCampaignMap lscm;
	lscm.mapId = mapId;
	sendLobbyPack(lscm);
}

void CServerHandler::setCampaignBonus(int bonusId) const
{
	if(state == EClientState::GAMEPLAY) // FIXME: UI shouldn't sent commands in first place
		return;

	LobbySetCampaignBonus lscb;
	lscb.bonusId = bonusId;
	sendLobbyPack(lscb);
}

void CServerHandler::setMapInfo(std::shared_ptr<CMapInfo> to, std::shared_ptr<CMapGenOptions> mapGenOpts) const
{
	LobbySetMap lsm;
	lsm.mapInfo = to;
	lsm.mapGenOpts = mapGenOpts;
	sendLobbyPack(lsm);
}

void CServerHandler::setPlayer(PlayerColor color) const
{
	LobbySetPlayer lsp;
	lsp.clickedColor = color;
	sendLobbyPack(lsp);
}

void CServerHandler::setPlayerOption(ui8 what, ui8 dir, PlayerColor player) const
{
	LobbyChangePlayerOption lcpo;
	lcpo.what = what;
	lcpo.direction = dir;
	lcpo.color = player;
	sendLobbyPack(lcpo);
}

void CServerHandler::setDifficulty(int to) const
{
	LobbySetDifficulty lsd;
	lsd.difficulty = to;
	sendLobbyPack(lsd);
}

void CServerHandler::setTurnLength(int npos) const
{
	vstd::amin(npos, GameConstants::POSSIBLE_TURNTIME.size() - 1);
	LobbySetTurnTime lstt;
	lstt.turnTime = GameConstants::POSSIBLE_TURNTIME[npos];
	sendLobbyPack(lstt);
}

void CServerHandler::sendMessage(const std::string & txt) const
{
	std::istringstream readed;
	readed.str(txt);
	std::string command;
	readed >> command;
	if(command == "!passhost")
	{
		std::string id;
		readed >> id;
		if(id.length())
		{
			LobbyChangeHost lch;
			lch.newHostConnectionId = boost::lexical_cast<int>(id);
			sendLobbyPack(lch);
		}
	}
	else if(command == "!forcep")
	{
		std::string connectedId, playerColorId;
		readed >> connectedId;
		readed >> playerColorId;
		if(connectedId.length() && playerColorId.length())
		{
			ui8 connected = boost::lexical_cast<int>(connectedId);
			auto color = PlayerColor(boost::lexical_cast<int>(playerColorId));
			if(color.isValidPlayer() && playerNames.find(connected) != playerNames.end())
			{
				LobbyForceSetPlayer lfsp;
				lfsp.targetConnectedPlayer = connected;
				lfsp.targetPlayerColor = color;
				sendLobbyPack(lfsp);
			}
		}
	}
	else
	{
		LobbyChatMessage lcm;
		lcm.message = txt;
		lcm.playerName = playerNames.find(myFirstId())->second.name;
		sendLobbyPack(lcm);
	}
}

void CServerHandler::sendGuiAction(ui8 action) const
{
	LobbyGuiAction lga;
	lga.action = static_cast<LobbyGuiAction::EAction>(action);
	sendLobbyPack(lga);
}

void CServerHandler::sendRestartGame() const
{
	LobbyEndGame endGame;
	endGame.closeConnection = false;
	endGame.restart = true;
	sendLobbyPack(endGame);
}

void CServerHandler::sendStartGame(bool allowOnlyAI) const
{
	verifyStateBeforeStart(allowOnlyAI ? true : settings["session"]["onlyai"].Bool());
	LobbyStartGame lsg;
	if(client)
	{
		lsg.initializedStartInfo = std::make_shared<StartInfo>(* const_cast<StartInfo *>(client->getStartInfo(true)));
		lsg.initializedStartInfo->mode = StartInfo::NEW_GAME;
		lsg.initializedStartInfo->seedToBeUsed = lsg.initializedStartInfo->seedPostInit = 0;
		* si = * lsg.initializedStartInfo;
	}
	sendLobbyPack(lsg);
	c->enterLobbyConnectionMode();
	c->disableStackSendingByID();
}

void CServerHandler::startGameplay(CGameState * gameState)
{
	if(CMM)
		CMM->disable();
	client = new CClient();

	switch(si->mode)
	{
	case StartInfo::NEW_GAME:
		client->newGame(gameState);
		break;
	case StartInfo::CAMPAIGN:
		client->newGame(gameState);
		break;
	case StartInfo::LOAD_GAME:
		client->loadGame(gameState);
		break;
	default:
		throw std::runtime_error("Invalid mode");
	}
	// After everything initialized we can accept CPackToClient netpacks
	c->enterGameplayConnectionMode(client->gameState());
	state = EClientState::GAMEPLAY;
	
	//store settings to continue game
	if(!isServerLocal() && isGuest())
	{
		Settings saveSession = settings.write["server"]["reconnect"];
		saveSession->Bool() = true;
		Settings saveUuid = settings.write["server"]["uuid"];
		saveUuid->String() = uuid;
		Settings saveNames = settings.write["server"]["names"];
		saveNames->Vector().clear();
		for(auto & name : myNames)
		{
			JsonNode jsonName;
			jsonName.String() = name;
			saveNames->Vector().push_back(jsonName);
		}
	}
}

void CServerHandler::endGameplay(bool closeConnection, bool restart)
{
	client->endGame();
	vstd::clear_pointer(client);

	if(closeConnection)
	{
		// Game is ending
		// Tell the network thread to reach a stable state
		CSH->sendClientDisconnecting();
		logNetwork->info("Closed connection.");
	}
	if(!restart)
	{
		if(CMM)
		{
			GH.terminate_cond->setn(false);
			GH.curInt = CMM.get();
			CMM->enable();
		}
		else
		{
			GH.curInt = CMainMenu::create().get();
		}
	}
	
	c->enterLobbyConnectionMode();
	c->disableStackSendingByID();
	
	//reset settings
	Settings saveSession = settings.write["server"]["reconnect"];
	saveSession->Bool() = false;
}

void CServerHandler::startCampaignScenario(std::shared_ptr<CCampaignState> cs)
{
	SDL_Event event;
	event.type = SDL_USEREVENT;
	event.user.code = EUserEvent::CAMPAIGN_START_SCENARIO;
	if(cs)
		event.user.data1 = CMemorySerializer::deepCopy(*cs.get()).release();
	else
		event.user.data1 = CMemorySerializer::deepCopy(*si->campState.get()).release();
	SDL_PushEvent(&event);
}

void CServerHandler::showServerError(std::string txt)
{
	CInfoWindow::showInfoDialog(txt, {});
}

int CServerHandler::howManyPlayerInterfaces()
{
	int playerInts = 0;
	for(auto pint : client->playerint)
	{
		if(dynamic_cast<CPlayerInterface *>(pint.second.get()))
			playerInts++;
	}

	return playerInts;
}

ui8 CServerHandler::getLoadMode()
{
	if(state == EClientState::GAMEPLAY)
	{
		if(si->campState)
			return ELoadMode::CAMPAIGN;
		for(auto pn : playerNames)
		{
			if(pn.second.connection != c->connectionID)
				return ELoadMode::MULTI;
		}
		if(howManyPlayerInterfaces() > 1)  //this condition will work for hotseat mode OR multiplayer with allowed more than 1 color per player to control
			return ELoadMode::MULTI;

		return ELoadMode::SINGLE;
	}
	return loadMode;
}

void CServerHandler::restoreLastSession()
{
	auto loadSession = [this]()
	{
		uuid = settings["server"]["uuid"].String();
		for(auto & name : settings["server"]["names"].Vector())
			myNames.push_back(name.String());
		resetStateForLobby(StartInfo::LOAD_GAME, &myNames);
		screenType = ESelectionScreen::loadGame;
		justConnectToServer(settings["server"]["server"].String(), settings["server"]["port"].Integer());
	};
	
	auto cleanUpSession = []()
	{
		//reset settings
		Settings saveSession = settings.write["server"]["reconnect"];
		saveSession->Bool() = false;
	};
	
	CInfoWindow::showYesNoDialog(VLC->generaltexth->translate("vcmi.server.confirmReconnect"), {}, loadSession, cleanUpSession);
}

void CServerHandler::debugStartTest(std::string filename, bool save)
{
	logGlobal->info("Starting debug test with file: %s", filename);
	auto mapInfo = std::make_shared<CMapInfo>();
	if(save)
	{
		resetStateForLobby(StartInfo::LOAD_GAME);
		mapInfo->saveInit(ResourceID(filename, EResType::CLIENT_SAVEGAME));
		screenType = ESelectionScreen::loadGame;
	}
	else
	{
		resetStateForLobby(StartInfo::NEW_GAME);
		mapInfo->mapInit(filename);
		screenType = ESelectionScreen::newGame;
	}
	if(settings["session"]["donotstartserver"].Bool())
		justConnectToServer(localhostAddress, 3030);
	else
		startLocalServerAndConnect();

	boost::this_thread::sleep(boost::posix_time::milliseconds(100));

	while(!settings["session"]["headless"].Bool() && !dynamic_cast<CLobbyScreen *>(GH.topInt().get()))
		boost::this_thread::sleep(boost::posix_time::milliseconds(50));
	while(!mi || mapInfo->fileURI != CSH->mi->fileURI)
	{
		setMapInfo(mapInfo);
		boost::this_thread::sleep(boost::posix_time::milliseconds(50));
	}
	// "Click" on color to remove us from it
	setPlayer(myFirstColor());
	while(myFirstColor() != PlayerColor::CANNOT_DETERMINE)
		boost::this_thread::sleep(boost::posix_time::milliseconds(50));

	while(true)
	{
		try
		{
			sendStartGame();
			break;
		}
		catch(...)
		{

		}
		boost::this_thread::sleep(boost::posix_time::milliseconds(50));
	}
}

void CServerHandler::threadHandleConnection()
{
	setThreadName("CServerHandler::threadHandleConnection");
	c->enterLobbyConnectionMode();

	try
	{
		sendClientConnecting();
		while(c->connected)
		{
			while(state == EClientState::STARTING)
				boost::this_thread::sleep(boost::posix_time::milliseconds(10));

			CPack * pack = c->retrievePack();
			if(state == EClientState::DISCONNECTING)
			{
				// FIXME: server shouldn't really send netpacks after it's tells client to disconnect
				// Though currently they'll be delivered and might cause crash.
				vstd::clear_pointer(pack);
			}
			else if(auto lobbyPack = dynamic_ptr_cast<CPackForLobby>(pack))
			{
				if(applier->getApplier(typeList.getTypeID(pack))->applyOnLobbyHandler(this, pack))
				{
					if(!settings["session"]["headless"].Bool())
					{
						boost::unique_lock<boost::recursive_mutex> lock(*mx);
						packsForLobbyScreen.push_back(lobbyPack);
					}
				}
			}
			else if(auto clientPack = dynamic_ptr_cast<CPackForClient>(pack))
			{
				client->handlePack(clientPack);
			}
		}
	}
	//catch only asio exceptions
	catch(const boost::system::system_error & e)
	{
		if(state == EClientState::DISCONNECTING)
		{
			logNetwork->info("Successfully closed connection to server, ending listening thread!");
		}
		else
		{
			logNetwork->error("Lost connection to server, ending listening thread!");
			logNetwork->error(e.what());
			if(client)
			{
				state = EClientState::DISCONNECTING;
				CGuiHandler::pushSDLEvent(SDL_USEREVENT, EUserEvent::RETURN_TO_MAIN_MENU);
			}
			else
			{
				auto lcd = new LobbyClientDisconnected();
				lcd->clientId = c->connectionID;
				boost::unique_lock<boost::recursive_mutex> lock(*mx);
				packsForLobbyScreen.push_back(lcd);
			}
		}
	}
	catch(...)
	{
		handleException();
		throw;
	}
}

void CServerHandler::threadRunServer()
{
#if !defined(VCMI_ANDROID) && !defined(VCMI_IOS)
	setThreadName("CServerHandler::threadRunServer");
	const std::string logName = (VCMIDirs::get().userLogsPath() / "server_log.txt").string();
	std::string comm = VCMIDirs::get().serverPath().string()
		+ " --port=" + boost::lexical_cast<std::string>(getHostPort())
		+ " --run-by-client"
		+ " --uuid=" + uuid;
	if(settings["session"]["lobby"].Bool() && settings["session"]["host"].Bool())
	{
		comm += " --lobby=" + settings["session"]["address"].String();
		comm += " --connections=" + settings["session"]["hostConnections"].String();
		comm += " --lobby-port=" + boost::lexical_cast<std::string>(settings["session"]["port"].Integer());
		comm += " --lobby-uuid=" + settings["session"]["hostUuid"].String();
	}
		
	if(shm)
	{
		comm += " --enable-shm";
		if(settings["session"]["enable-shm-uuid"].Bool())
			comm += " --enable-shm-uuid";
	}
	comm += " > \"" + logName + '\"';
    logGlobal->info("Server command line: %s", comm);

#ifdef VCMI_WINDOWS
	int result = -1;
	const auto bufSize = ::MultiByteToWideChar(CP_UTF8, 0, comm.c_str(), comm.size(), nullptr, 0);
	if(bufSize > 0)
	{
		std::wstring wComm(bufSize, {});
		const auto convertResult = ::MultiByteToWideChar(CP_UTF8, 0, comm.c_str(), comm.size(), &wComm[0], bufSize);
		if(convertResult > 0)
			result = ::_wsystem(wComm.c_str());
		else
			logNetwork->error("Error " + std::to_string(GetLastError()) + ": failed to convert server launch command to wide string: " + comm);
	}
	else
		logNetwork->error("Error " + std::to_string(GetLastError()) + ": failed to obtain buffer length to convert server launch command to wide string : " + comm);
#else
	int result = std::system(comm.c_str());
#endif
	if (result == 0)
	{
		logNetwork->info("Server closed correctly");
	}
	else
	{
		logNetwork->error("Error: server failed to close correctly or crashed!");
		logNetwork->error("Check %s for more info", logName);
	}
	onServerFinished();
#endif
}

void CServerHandler::onServerFinished()
{
	threadRunLocalServer.reset();
	CSH->campaignServerRestartLock.setn(false);
}

void CServerHandler::sendLobbyPack(const CPackForLobby & pack) const
{
	if(state != EClientState::STARTING)
		c->sendPack(&pack);
}
