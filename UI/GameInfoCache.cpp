// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/Common.h"

#include <string>
#include <map>
#include <memory>
#include <algorithm>

#include "base/logging.h"
#include "base/timeutil.h"
#include "base/stringutil.h"
#include "file/file_util.h"
#include "file/zip_read.h"
#include "thin3d/thin3d.h"
#include "thread/prioritizedworkqueue.h"
#include "Common/FileUtil.h"
#include "Common/StringUtils.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/FileSystems/DirectoryFileSystem.h"
#include "Core/FileSystems/VirtualDiscFileSystem.h"
#include "Core/ELF/PBPReader.h"
#include "Core/SaveState.h"
#include "Core/System.h"
#include "Core/Loaders.h"
#include "Core/Util/GameManager.h"
#include "Core/Config.h"
#include "UI/GameInfoCache.h"
#include "UI/TextureUtil.h"

GameInfoCache *g_gameInfoCache;

GameInfo::GameInfo() : fileType(IdentifiedFileType::UNKNOWN) {
}

GameInfo::~GameInfo() {
	delete icon.texture;
	delete pic0.texture;
	delete pic1.texture;
	delete fileLoader;
}

bool GameInfo::Delete() {
	switch (fileType) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
		{
			// Just delete the one file (TODO: handle two-disk games as well somehow).
			const char *fileToRemove = filePath_.c_str();
			File::Delete(fileToRemove);
			auto i = std::find(g_Config.recentIsos.begin(), g_Config.recentIsos.end(), fileToRemove);
			if (i != g_Config.recentIsos.end()) {
				g_Config.recentIsos.erase(i);
			}
			return true;
		}
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
	case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		{
			// TODO: This could be handled by Core/Util/GameManager too somehow.
			std::string directoryToRemove = ResolvePBPDirectory(filePath_);
			INFO_LOG(SYSTEM, "Deleting %s", directoryToRemove.c_str());
			if (!File::DeleteDirRecursively(directoryToRemove)) {
				ERROR_LOG(SYSTEM, "Failed to delete file");
				return false;
			}
			g_Config.CleanRecent();
			return true;
		}
	case IdentifiedFileType::PSP_ELF:
	case IdentifiedFileType::UNKNOWN_BIN:
	case IdentifiedFileType::UNKNOWN_ELF:
	case IdentifiedFileType::ARCHIVE_RAR:
	case IdentifiedFileType::ARCHIVE_ZIP:
	case IdentifiedFileType::ARCHIVE_7Z:
		{
			const std::string &fileToRemove = filePath_;
			File::Delete(fileToRemove);
			return true;
		}

	case IdentifiedFileType::PPSSPP_SAVESTATE:
		{
			const std::string &ppstPath = filePath_;
			File::Delete(ppstPath);
			const std::string screenshotPath = ReplaceAll(filePath_, ".ppst", ".jpg");
			if (File::Exists(screenshotPath)) {
				File::Delete(screenshotPath);
			}
			return true;
		}

	default:
		return false;
	}
}

static int64_t GetDirectoryRecursiveSize(const std::string &path) {
	std::vector<FileInfo> fileInfo;
	getFilesInDir(path.c_str(), &fileInfo);
	int64_t sizeSum = 0;
	// Note: getFileInDir does not fill in fileSize properly.
	for (size_t i = 0; i < fileInfo.size(); i++) {
		FileInfo finfo;
		getFileInfo(fileInfo[i].fullName.c_str(), &finfo);
		if (!finfo.isDirectory)
			sizeSum += finfo.size;
		else
			sizeSum += GetDirectoryRecursiveSize(finfo.fullName);
	}
	return sizeSum;
}

u64 GameInfo::GetGameSizeInBytes() {
	switch (fileType) {
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
	case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		return GetDirectoryRecursiveSize(ResolvePBPDirectory(filePath_));

	default:
		return GetFileLoader()->FileSize();
	}
}

// Not too meaningful if the object itself is a savedata directory...
std::vector<std::string> GameInfo::GetSaveDataDirectories() {
	std::string memc = GetSysDirectory(DIRECTORY_SAVEDATA);

	std::vector<FileInfo> dirs;
	getFilesInDir(memc.c_str(), &dirs);

	std::vector<std::string> directories;
	if (id.size() < 5) {
		return directories;
	}
	for (size_t i = 0; i < dirs.size(); i++) {
		if (startsWith(dirs[i].name, id)) {
			directories.push_back(dirs[i].fullName);
		}
	}

	return directories;
}

u64 GameInfo::GetSaveDataSizeInBytes() {
	if (fileType == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY || fileType == IdentifiedFileType::PPSSPP_SAVESTATE) {
		return 0;
	}
	std::vector<std::string> saveDataDir = GetSaveDataDirectories();

	u64 totalSize = 0;
	u64 filesSizeInDir = 0;
	for (size_t j = 0; j < saveDataDir.size(); j++) {
		std::vector<FileInfo> fileInfo;
		getFilesInDir(saveDataDir[j].c_str(), &fileInfo);
		// Note: getFileInDir does not fill in fileSize properly.
		for (size_t i = 0; i < fileInfo.size(); i++) {
			FileInfo finfo;
			getFileInfo(fileInfo[i].fullName.c_str(), &finfo);
			if (!finfo.isDirectory)
				filesSizeInDir += finfo.size;
		}
		if (filesSizeInDir < 0xA00000) {
			// HACK: Generally the savedata size in a dir shouldn't be more than 10MB.
			totalSize += filesSizeInDir;
		}
		filesSizeInDir = 0;
	}
	return totalSize;
}

u64 GameInfo::GetInstallDataSizeInBytes() {
	if (fileType == IdentifiedFileType::PSP_SAVEDATA_DIRECTORY || fileType == IdentifiedFileType::PPSSPP_SAVESTATE) {
		return 0;
	}
	std::vector<std::string> saveDataDir = GetSaveDataDirectories();

	u64 totalSize = 0;
	u64 filesSizeInDir = 0;
	for (size_t j = 0; j < saveDataDir.size(); j++) {
		std::vector<FileInfo> fileInfo;
		getFilesInDir(saveDataDir[j].c_str(), &fileInfo);
		// Note: getFileInDir does not fill in fileSize properly.
		for (size_t i = 0; i < fileInfo.size(); i++) {
			FileInfo finfo;
			getFileInfo(fileInfo[i].fullName.c_str(), &finfo);
			if (!finfo.isDirectory)
				filesSizeInDir += finfo.size;
		}
		if (filesSizeInDir >= 0xA00000) { 
			// HACK: Generally the savedata size in a dir shouldn't be more than 10MB.
			// This is probably GameInstall data.
			totalSize += filesSizeInDir;
		}
		filesSizeInDir = 0;
	}
	return totalSize;
}

bool GameInfo::LoadFromPath(const std::string &gamePath) {
	std::lock_guard<std::mutex> guard(lock);
	// No need to rebuild if we already have it loaded.
	if (filePath_ != gamePath) {
		delete fileLoader;
		fileLoader = ConstructFileLoader(gamePath);
		if (!fileLoader)
			return false;
		filePath_ = gamePath;

		// This is a fallback title, while we're loading / if unable to load.
		title = File::GetFilename(filePath_);
	}

	return fileLoader ? fileLoader->Exists() : true;
}

FileLoader *GameInfo::GetFileLoader() {
	if (!fileLoader) {
		fileLoader = ConstructFileLoader(filePath_);
	}
	return fileLoader;
}

void GameInfo::DisposeFileLoader() {
	delete fileLoader;
	fileLoader = nullptr;
}

bool GameInfo::DeleteAllSaveData() {
	std::vector<std::string> saveDataDir = GetSaveDataDirectories();
	for (size_t j = 0; j < saveDataDir.size(); j++) {
		std::vector<FileInfo> fileInfo;
		getFilesInDir(saveDataDir[j].c_str(), &fileInfo);

		u64 totalSize = 0;
		for (size_t i = 0; i < fileInfo.size(); i++) {
			File::Delete(fileInfo[i].fullName.c_str());
		}

		File::DeleteDir(saveDataDir[j].c_str());
	}
	return true;
}

void GameInfo::ParseParamSFO() {
	title = paramSFO.GetValueString("TITLE");
	id = paramSFO.GetValueString("DISC_ID");
	id_version = paramSFO.GetValueString("DISC_ID") + "_" + paramSFO.GetValueString("DISC_VERSION");
	disc_total = paramSFO.GetValueInt("DISC_TOTAL");
	disc_number = paramSFO.GetValueInt("DISC_NUMBER");
	// region = paramSFO.GetValueInt("REGION");  // Always seems to be 32768?

	region = GAMEREGION_OTHER;
	if (id_version.size() >= 4) {
		std::string regStr = id_version.substr(0, 4);

		// Guesswork
		switch (regStr[2]) {
		case 'E': region = GAMEREGION_EUROPE; break;
		case 'U': region = GAMEREGION_USA; break;
		case 'J': region = GAMEREGION_JAPAN; break;
		case 'H': region = GAMEREGION_HONGKONG; break;
		case 'A': region = GAMEREGION_ASIA; break;
		}
		/*
		if (regStr == "NPEZ" || regStr == "NPEG" || regStr == "ULES" || regStr == "UCES" ||
			  regStr == "NPEX") {
			region = GAMEREGION_EUROPE;
		} else if (regStr == "NPUG" || regStr == "NPUZ" || regStr == "ULUS" || regStr == "UCUS") {
			region = GAMEREGION_USA;
		} else if (regStr == "NPJH" || regStr == "NPJG" || regStr == "ULJM"|| regStr == "ULJS") {
			region = GAMEREGION_JAPAN;
		} else if (regStr == "NPHG") {
			region = GAMEREGION_HONGKONG;
		} else if (regStr == "UCAS") {
			region = GAMEREGION_CHINA;
		}*/
	}

	paramSFOLoaded = true;
}

std::string GameInfo::GetTitle() {
	std::lock_guard<std::mutex> guard(lock);
	return title;
}

bool GameInfo::IsPending() {
	std::lock_guard<std::mutex> guard(lock);
	return pending;
}

bool GameInfo::IsWorking() {
	std::lock_guard<std::mutex> guard(lock);
	return working;
}

void GameInfo::SetTitle(const std::string &newTitle) {
	std::lock_guard<std::mutex> guard(lock);
	title = newTitle;
}

static bool ReadFileToString(IFileSystem *fs, const char *filename, std::string *contents, std::mutex *mtx) {
	PSPFileInfo info = fs->GetFileInfo(filename);
	if (!info.exists) {
		return false;
	}

	int handle = fs->OpenFile(filename, FILEACCESS_READ);
	if (!handle) {
		return false;
	}

	if (mtx) {
		std::lock_guard<std::mutex> lock(*mtx);
		contents->resize(info.size);
		fs->ReadFile(handle, (u8 *)contents->data(), info.size);
	} else {
		contents->resize(info.size);
		fs->ReadFile(handle, (u8 *)contents->data(), info.size);
	}
	fs->CloseFile(handle);
	return true;
}

static bool ReadVFSToString(const char *filename, std::string *contents, std::mutex *mtx) {
	size_t sz;
	uint8_t *data = VFSReadFile(filename, &sz);
	if (data) {
		if (mtx) {
			std::lock_guard<std::mutex> lock(*mtx);
			*contents = std::string((const char *)data, sz);
		} else {
			*contents = std::string((const char *)data, sz);
		}
	}
	delete [] data;
	return data != nullptr;
}


class GameInfoWorkItem : public PrioritizedWorkQueueItem {
public:
	GameInfoWorkItem(const std::string &gamePath, GameInfo *info)
		: gamePath_(gamePath), info_(info) {
	}

	~GameInfoWorkItem() override {
		info_->DisposeFileLoader();
	}

	void run() override {
		if (!info_->LoadFromPath(gamePath_))
			return;

		{
			std::lock_guard<std::mutex> lock(info_->lock);
			info_->working = true;
			info_->fileType = Identify_File(info_->GetFileLoader());
		}

		switch (info_->fileType) {
		case IdentifiedFileType::PSP_PBP:
		case IdentifiedFileType::PSP_PBP_DIRECTORY:
			{
				FileLoader *pbpLoader = info_->GetFileLoader();
				std::unique_ptr<FileLoader> altLoader;
				if (info_->fileType == IdentifiedFileType::PSP_PBP_DIRECTORY) {
					std::string ebootPath = ResolvePBPFile(gamePath_);
					if (ebootPath != gamePath_) {
						pbpLoader = ConstructFileLoader(ebootPath);
						altLoader.reset(pbpLoader);
					}
				}

				PBPReader pbp(pbpLoader);
				if (!pbp.IsValid()) {
					if (pbp.IsELF()) {
						goto handleELF;
					}
					ERROR_LOG(LOADER, "invalid pbp %s\n", pbpLoader->Path().c_str());
					return;
				}

				// First, PARAM.SFO.
				std::vector<u8> sfoData;
				if (pbp.GetSubFile(PBP_PARAM_SFO, &sfoData)) {
					std::lock_guard<std::mutex> lock(info_->lock);
					info_->paramSFO.ReadSFO(sfoData);
					info_->ParseParamSFO();
				}

				// Then, ICON0.PNG.
				if (pbp.GetSubFileSize(PBP_ICON0_PNG) > 0) {
					std::lock_guard<std::mutex> lock(info_->lock);
					pbp.GetSubFileAsString(PBP_ICON0_PNG, &info_->icon.data);
				} else {
					// Read standard icon
					ReadVFSToString("unknown.png", &info_->icon.data, &info_->lock);
				}
				info_->icon.dataLoaded = true;

				if (info_->wantFlags & GAMEINFO_WANTBG) {
					if (pbp.GetSubFileSize(PBP_PIC0_PNG) > 0) {
						std::lock_guard<std::mutex> lock(info_->lock);
						pbp.GetSubFileAsString(PBP_PIC0_PNG, &info_->pic0.data);
						info_->pic0.dataLoaded = true;
					}
					if (pbp.GetSubFileSize(PBP_PIC1_PNG) > 0) {
						std::lock_guard<std::mutex> lock(info_->lock);
						pbp.GetSubFileAsString(PBP_PIC1_PNG, &info_->pic1.data);
						info_->pic1.dataLoaded = true;
					}
				}
				if (info_->wantFlags & GAMEINFO_WANTSND) {
					if (pbp.GetSubFileSize(PBP_SND0_AT3) > 0) {
						std::lock_guard<std::mutex> lock(info_->lock);
						pbp.GetSubFileAsString(PBP_SND0_AT3, &info_->sndFileData);
						info_->sndDataLoaded = true;
					}
				}
			}
			break;

		case IdentifiedFileType::PSP_ELF:
handleELF:
			// An elf on its own has no usable information, no icons, no nothing.
			{
				std::lock_guard<std::mutex> lock(info_->lock);
				info_->id = "ELF000000";
				info_->id_version = "ELF000000_1.00";
				info_->paramSFOLoaded = true;
			}

			// Read standard icon
			DEBUG_LOG(LOADER, "Loading unknown.png because there was an ELF");
			ReadVFSToString("unknown.png", &info_->icon.data, &info_->lock);
			info_->icon.dataLoaded = true;
			break;

		case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		{
			SequentialHandleAllocator handles;
			VirtualDiscFileSystem umd(&handles, gamePath_.c_str());

			// Alright, let's fetch the PARAM.SFO.
			std::string paramSFOcontents;
			if (ReadFileToString(&umd, "/PARAM.SFO", &paramSFOcontents, 0)) {
				std::lock_guard<std::mutex> lock(info_->lock);
				info_->paramSFO.ReadSFO((const u8 *)paramSFOcontents.data(), paramSFOcontents.size());
				info_->ParseParamSFO();
			}

			ReadFileToString(&umd, "/ICON0.PNG", &info_->icon.data, &info_->lock);
			info_->icon.dataLoaded = true;
			if (info_->wantFlags & GAMEINFO_WANTBG) {
				ReadFileToString(&umd, "/PIC1.PNG", &info_->pic1.data, &info_->lock);
				info_->pic1.dataLoaded = true;
			}
			break;
		}

		case IdentifiedFileType::PPSSPP_SAVESTATE:
		{
			info_->SetTitle(SaveState::GetTitle(gamePath_));

			std::lock_guard<std::mutex> guard(info_->lock);

			// Let's use the screenshot as an icon, too.
			std::string screenshotPath = ReplaceAll(gamePath_, ".ppst", ".jpg");
			if (File::Exists(screenshotPath)) {
				if (readFileToString(false, screenshotPath.c_str(), info_->icon.data)) {
					info_->icon.dataLoaded = true;
				}
			}
			break;
		}

		case IdentifiedFileType::PSP_DISC_DIRECTORY:
			{
				info_->fileType = IdentifiedFileType::PSP_ISO;
				SequentialHandleAllocator handles;
				VirtualDiscFileSystem umd(&handles, gamePath_.c_str());

				// Alright, let's fetch the PARAM.SFO.
				std::string paramSFOcontents;
				if (ReadFileToString(&umd, "/PSP_GAME/PARAM.SFO", &paramSFOcontents, 0)) {
					std::lock_guard<std::mutex> lock(info_->lock);
					info_->paramSFO.ReadSFO((const u8 *)paramSFOcontents.data(), paramSFOcontents.size());
					info_->ParseParamSFO();
				}

				ReadFileToString(&umd, "/PSP_GAME/ICON0.PNG", &info_->icon.data, &info_->lock);
				info_->icon.dataLoaded = true;
				if (info_->wantFlags & GAMEINFO_WANTBG) {
					ReadFileToString(&umd, "/PSP_GAME/PIC0.PNG", &info_->pic0.data, &info_->lock);
					info_->pic0.dataLoaded = true;
					ReadFileToString(&umd, "/PSP_GAME/PIC1.PNG", &info_->pic1.data, &info_->lock);
					info_->pic1.dataLoaded = true;
				}
				if (info_->wantFlags & GAMEINFO_WANTSND) {
					ReadFileToString(&umd, "/PSP_GAME/SND0.AT3", &info_->sndFileData, &info_->lock);
					info_->pic1.dataLoaded = true;
				}
				break;
			}

		case IdentifiedFileType::PSP_ISO:
		case IdentifiedFileType::PSP_ISO_NP:
			{
				info_->fileType = IdentifiedFileType::PSP_ISO;
				SequentialHandleAllocator handles;
				// Let's assume it's an ISO.
				// TODO: This will currently read in the whole directory tree. Not really necessary for just a
				// few files.
				FileLoader *fl = info_->GetFileLoader();
				if (!fl)
					return;  // Happens with UWP currently, TODO...
				BlockDevice *bd = constructBlockDevice(info_->GetFileLoader());
				if (!bd)
					return;  // nothing to do here..
				ISOFileSystem umd(&handles, bd);

				// Alright, let's fetch the PARAM.SFO.
				std::string paramSFOcontents;
				if (ReadFileToString(&umd, "/PSP_GAME/PARAM.SFO", &paramSFOcontents, nullptr)) {
					std::lock_guard<std::mutex> lock(info_->lock);
					info_->paramSFO.ReadSFO((const u8 *)paramSFOcontents.data(), paramSFOcontents.size());
					info_->ParseParamSFO();

					if (info_->wantFlags & GAMEINFO_WANTBG) {
						ReadFileToString(&umd, "/PSP_GAME/PIC0.PNG", &info_->pic0.data, nullptr);
						info_->pic0.dataLoaded = true;
						ReadFileToString(&umd, "/PSP_GAME/PIC1.PNG", &info_->pic1.data, nullptr);
						info_->pic1.dataLoaded = true;
					}
					if (info_->wantFlags & GAMEINFO_WANTSND) {
						ReadFileToString(&umd, "/PSP_GAME/SND0.AT3", &info_->sndFileData, nullptr);
						info_->pic1.dataLoaded = true;
					}
				}

				// Fall back to unknown icon if ISO is broken/is a homebrew ISO, override is allowed though
				if (!ReadFileToString(&umd, "/PSP_GAME/ICON0.PNG", &info_->icon.data, &info_->lock)) {
					DEBUG_LOG(LOADER, "Loading unknown.png because no icon was found");
					ReadVFSToString("unknown.png", &info_->icon.data, &info_->lock);
				}
				info_->icon.dataLoaded = true;
				break;
			}

			case IdentifiedFileType::ARCHIVE_ZIP:
				info_->paramSFOLoaded = true;
				{
					ReadVFSToString("zip.png", &info_->icon.data, &info_->lock);
					info_->icon.dataLoaded = true;
				}
				break;

			case IdentifiedFileType::ARCHIVE_RAR:
				info_->paramSFOLoaded = true;
				{
					ReadVFSToString("rargray.png", &info_->icon.data, &info_->lock);
					info_->icon.dataLoaded = true;
				}
				break;

			case IdentifiedFileType::ARCHIVE_7Z:
				info_->paramSFOLoaded = true;
				{
					ReadVFSToString("7z.png", &info_->icon.data, &info_->lock);
					info_->icon.dataLoaded = true;
				}
				break;

			case IdentifiedFileType::NORMAL_DIRECTORY:
			default:
				info_->paramSFOLoaded = true;
				break;
		}

		info_->hasConfig = g_Config.hasGameConfig(info_->id);

		if (info_->wantFlags & GAMEINFO_WANTSIZE) {
			std::lock_guard<std::mutex> lock(info_->lock);
			info_->gameSize = info_->GetGameSizeInBytes();
			info_->saveDataSize = info_->GetSaveDataSizeInBytes();
			info_->installDataSize = info_->GetInstallDataSizeInBytes();
		}

		std::lock_guard<std::mutex> lock(info_->lock);
		info_->pending = false;
		info_->working = false;
		// ILOG("Completed writing info for %s", info_->GetTitle().c_str());
	}

	float priority() override {
		return info_->lastAccessedTime;
	}

private:
	std::string gamePath_;
	GameInfo *info_;
	DISALLOW_COPY_AND_ASSIGN(GameInfoWorkItem);
};

GameInfoCache::GameInfoCache() : gameInfoWQ_(nullptr) {
	Init();
}

GameInfoCache::~GameInfoCache() {
	Clear();
	Shutdown();
}

void GameInfoCache::Init() {
	gameInfoWQ_ = new PrioritizedWorkQueue();
	ProcessWorkQueueOnThreadWhile(gameInfoWQ_);
}

void GameInfoCache::Shutdown() {
	if (gameInfoWQ_) {
		StopProcessingWorkQueue(gameInfoWQ_);
		delete gameInfoWQ_;
		gameInfoWQ_ = nullptr;
	}
}

void GameInfoCache::Clear() {
	if (gameInfoWQ_) {
		gameInfoWQ_->Flush();
		gameInfoWQ_->WaitUntilDone();
	}
	for (auto iter = info_.begin(); iter != info_.end(); iter++) {
		{
			std::lock_guard<std::mutex> lock(iter->second->lock);
			iter->second->pic0.Clear();
			iter->second->pic1.Clear();
			iter->second->icon.Clear();

			if (!iter->second->sndFileData.empty()) {
				iter->second->sndFileData.clear();
				iter->second->sndDataLoaded = false;
			}
		}
		delete iter->second;
	}
	info_.clear();
}

void GameInfoCache::FlushBGs() {
	for (auto iter = info_.begin(); iter != info_.end(); iter++) {
		std::lock_guard<std::mutex> lock(iter->second->lock);
		iter->second->pic0.Clear();
		iter->second->pic1.Clear();

		if (!iter->second->sndFileData.empty()) {
			iter->second->sndFileData.clear();
			iter->second->sndDataLoaded = false;
		}
		iter->second->wantFlags &= ~(GAMEINFO_WANTBG | GAMEINFO_WANTSND | GAMEINFO_WANTBGDATA);
	}
}

void GameInfoCache::PurgeType(IdentifiedFileType fileType) {
	if (gameInfoWQ_)
		gameInfoWQ_->Flush();
	restart:
	for (auto iter = info_.begin(); iter != info_.end(); iter++) {
		if (iter->second->fileType == fileType) {
			info_.erase(iter);
			goto restart;
		}
	}
}

void GameInfoCache::WaitUntilDone(GameInfo *info) {
	while (info->IsPending()) {
		if (gameInfoWQ_->WaitUntilDone(false)) {
			// A true return means everything finished, so bail out.
			// This way even if something gets stuck, we won't hang.
			break;
		}

		// Otherwise, wait again if it's not done...
	}
}


// Runs on the main thread.
GameInfo *GameInfoCache::GetInfo(Draw::DrawContext *draw, const std::string &gamePath, int wantFlags) {
	GameInfo *info = nullptr;

	auto iter = info_.find(gamePath);
	if (iter != info_.end()) {
		info = iter->second;
	}

	// If wantFlags don't match, we need to start over.  We'll just queue the work item again.
	if (info && (info->wantFlags & wantFlags) == wantFlags) {
		if (draw && info->icon.dataLoaded && !info->icon.texture) {
			SetupTexture(info, draw, info->icon);
		}
		if (draw && info->pic0.dataLoaded && !info->pic0.texture) {
			SetupTexture(info, draw, info->pic0);
		}
		if (draw && info->pic1.dataLoaded && !info->pic1.texture) {
			SetupTexture(info, draw, info->pic1);
		}
		info->lastAccessedTime = time_now_d();
		return info;
	}

	if (!info) {
		info = new GameInfo();
	}

	if (info->IsWorking()) {
		// Uh oh, it's currently in process.  It could mark pending = false with the wrong wantFlags.
		// Let's wait it out, then queue.
		WaitUntilDone(info);
	}

	{
		std::lock_guard<std::mutex> lock(info->lock);
		info->wantFlags |= wantFlags;
		info->pending = true;
	}

	GameInfoWorkItem *item = new GameInfoWorkItem(gamePath, info);
	gameInfoWQ_->Add(item);

	info_[gamePath] = info;
	return info;
}

void GameInfoCache::SetupTexture(GameInfo *info, Draw::DrawContext *thin3d, GameInfoTex &icon) {
	using namespace Draw;
	if (icon.data.size()) {
		if (!icon.texture) {
			icon.texture = CreateTextureFromFileData(thin3d, (const uint8_t *)icon.data.data(), (int)icon.data.size(), ImageFileType::DETECT);
			if (icon.texture) {
				icon.timeLoaded = time_now_d();
			}
		}
		if ((info->wantFlags & GAMEINFO_WANTBGDATA) == 0) {
			icon.data.clear();
			icon.dataLoaded = false;
		}
	}
}
