#include "GameTitleLoader.h"

std::optional<TitleInfo> getFirstTitleInfoByTitleId(TitleId titleId)
{
	TitleInfo titleInfo;
	if (CafeTitleList::GetFirstByTitleId(titleId, titleInfo))
		return titleInfo;
	return {};
}

GameTitleLoader::GameTitleLoader()
{
	m_loaderThread = std::thread(&GameTitleLoader::LoadGameTitles, this);
}

void GameTitleLoader::QueueTitle(TitleId titleId)
{
	{
		std::lock_guard lock(m_threadMutex);
		m_titlesToLoad.emplace_front(titleId);
	}
	m_condVar.notify_one();
}

void GameTitleLoader::SetOnTitleLoaded(std::shared_ptr<GameTitleLoadedCallback> gameTitleLoadedCallback)
{
	{
		std::lock_guard lock(m_threadMutex);
		m_gameTitleLoadedCallback = std::move(gameTitleLoadedCallback);
	}
	m_condVar.notify_one();
}

void GameTitleLoader::ReloadGameTitles()
{
	if (m_callbackIdTitleList.has_value())
	{
		CafeTitleList::UnregisterCallback(m_callbackIdTitleList.value());
	}
	{
		// m_gameInfos belongs to the loader thread, which may be refreshing a title right now.
		// Drop the pending queue and let that thread clear its own state before the next title.
		std::lock_guard lock(m_threadMutex);
		m_titlesToLoad.clear();
		m_clearGameInfos = true;
	}
	CafeTitleList::ClearScanPaths();
	for (auto&& gamePath : GetConfig().game_paths)
		CafeTitleList::AddScanPath(gamePath);
	CafeTitleList::Refresh();
	m_callbackIdTitleList = CafeTitleList::RegisterCallback([](CafeTitleListCallbackEvent* evt, void* ctx) { static_cast<GameTitleLoader*>(ctx)->HandleTitleListCallback(evt); }, this);
}

GameTitleLoader::~GameTitleLoader()
{
	m_continueLoading = false;
	m_condVar.notify_one();
	m_loaderThread.join();
	if (m_callbackIdTitleList.has_value())
		CafeTitleList::UnregisterCallback(m_callbackIdTitleList.value());
}

std::shared_ptr<GameTitleLoadedCallback> GameTitleLoader::GetCallback()
{
	// SetOnTitleLoaded can replace the callback from another thread at any time
	std::lock_guard lock(m_threadMutex);
	return m_gameTitleLoadedCallback;
}

void GameTitleLoader::TitleRefresh(TitleId titleId)
{
	using namespace std::chrono;
	TitleId baseTitleId;
	CafeTitleList::FindBaseTitleId(titleId, baseTitleId);
	GameInfo2 gameInfo = CafeTitleList::GetGameInfo(titleId);
	if (!gameInfo.IsValid())
	{
		// the base title is gone (deleted, or its folder removed from the game paths)
		if (m_gameInfos.erase(baseTitleId) > 0)
		{
			m_iconCache.erase(baseTitleId);
			m_name_cache.erase(baseTitleId);
			if (auto callback = GetCallback())
				callback->OnTitleRemoved(baseTitleId);
		}
		return;
	}
	baseTitleId = gameInfo.GetBaseTitleId();

	Game& game = m_gameInfos[baseTitleId];
	// Describe the game by its base title, whichever of base/update/DLC triggered the refresh,
	// so the path and name stay the same when an update or DLC is installed later.
	std::optional<TitleInfo> titleInfo = getFirstTitleInfoByTitleId(baseTitleId);
	if (!titleInfo.has_value())
		titleInfo = getFirstTitleInfoByTitleId(titleId);
	game.titleId = baseTitleId;
	if (titleInfo.has_value())
		game.path = titleInfo->GetPath();
	game.isFavorite = GetConfig().IsGameListFavorite(baseTitleId);
	game.name = GetNameByTitleId(baseTitleId, titleInfo);
	game.version = gameInfo.GetVersion();
	game.region = gameInfo.GetRegion();
	game.dlc = gameInfo.GetAOCVersion();
	std::shared_ptr<Image> icon = LoadIcon(baseTitleId, titleInfo);
	iosu::pdm::GameListStat playTimeStat{};
	if (iosu::pdm::GetStatForGamelist(baseTitleId, playTimeStat))
	{
		game.minutesPlayed = playTimeStat.numMinutesPlayed;
		if (playTimeStat.last_played.year != 0)
		{
			game.lastPlayed = year_month_day(year(playTimeStat.last_played.year), month(playTimeStat.last_played.month + 1), day(playTimeStat.last_played.day));
		}
	}
	// Report existing entries again as well: installing an update or DLC changes the
	// version shown in the list, which used to stay stale until the list was reloaded.
	if (auto callback = GetCallback())
		callback->OnTitleLoaded(game, icon);
}

void GameTitleLoader::LoadGameTitles()
{
	while (m_continueLoading)
	{
		TitleId titleId;
		bool clearGameInfos;
		{
			std::unique_lock lock(m_threadMutex);
			m_condVar.wait(lock, [this] { return (!m_titlesToLoad.empty()) || !m_continueLoading; });
			if (!m_continueLoading)
				return;
			titleId = m_titlesToLoad.front();
			m_titlesToLoad.pop_front();
			clearGameInfos = std::exchange(m_clearGameInfos, false);
		}
		if (clearGameInfos)
			m_gameInfos.clear();
		TitleRefresh(titleId);
	}
}
std::string GameTitleLoader::GetNameByTitleId(TitleId titleId, const std::optional<TitleInfo>& titleInfo)
{
	auto it = m_name_cache.find(titleId);
	if (it != m_name_cache.end())
		return it->second;
	if (!titleInfo.has_value())
		return "Unknown title";
	std::string name;
	if (!GetConfig().GetGameListCustomName(titleId, name))
		name = titleInfo.value().GetMetaTitleName();
	m_name_cache.emplace(titleId, name);
	return name;
}

std::shared_ptr<Image> GameTitleLoader::LoadIcon(TitleId titleId, const std::optional<TitleInfo>& titleInfo)
{
	if (auto iconIt = m_iconCache.find(titleId); iconIt != m_iconCache.end())
		return iconIt->second;
	std::string tempMountPath = TitleInfo::GetUniqueTempMountingPath();
	if (!titleInfo.has_value())
		return {};
	auto titleInfoValue = titleInfo.value();
	if (!titleInfoValue.Mount(tempMountPath, "", FSC_PRIORITY_BASE))
		return {};
	auto tgaData = fsc_extractFile((tempMountPath + "/meta/iconTex.tga").c_str());
	if (!tgaData || tgaData->size() <= 16)
	{
		cemuLog_log(LogType::Force, "Failed to load icon for title {:016x}", titleId);
		titleInfoValue.Unmount(tempMountPath);
		return {};
	}
	auto image = std::make_shared<Image>(tgaData.value());
	titleInfoValue.Unmount(tempMountPath);
	if (!image->IsOk())
		return {};
	m_iconCache.emplace(titleId, image);
	return image;
}

void GameTitleLoader::HandleTitleListCallback(CafeTitleListCallbackEvent* evt)
{
	if (evt->eventType == CafeTitleListCallbackEvent::TYPE::TITLE_DISCOVERED || evt->eventType == CafeTitleListCallbackEvent::TYPE::TITLE_REMOVED)
	{
		QueueTitle(evt->titleInfo->GetAppTitleId());
	}
}
