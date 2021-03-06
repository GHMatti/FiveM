/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "ResourceCacheDevice.h"

#include <ResourceManager.h>

#include <concurrent_unordered_set.h>

#include <ICoreGameInit.h>

#include <Brofiler.h>

namespace fx
{
	fwEvent<const std::string&, size_t, size_t> OnCacheDownloadStatus;
}

#pragma comment(lib, "winmm.lib")
#include <mmsystem.h>

#include <Error.h>

static concurrency::concurrent_unordered_set<std::string> g_downloadedSet;

ResourceCacheDevice::ResourceCacheDevice(std::shared_ptr<ResourceCache> cache, bool blocking)
	: ResourceCacheDevice(cache, blocking, cache->GetCachePath())
{

}

ResourceCacheDevice::ResourceCacheDevice(std::shared_ptr<ResourceCache> cache, bool blocking, const std::string& cachePath)
	: m_cache(cache), m_blocking(blocking), m_cachePath(cachePath)
{
	m_httpClient = Instance<HttpClient>::Get();
}

boost::optional<ResourceCacheEntryList::Entry> ResourceCacheDevice::GetEntryForFileName(const std::string& fileName)
{
	// strip the path prefix
	std::string relativeName = fileName.substr(m_pathPrefix.length());

	// relative paths are {resource}/{filepath}
	int slashOffset = relativeName.find_first_of('/');
	std::string resourceName = relativeName.substr(0, slashOffset);
	std::string itemName = relativeName.substr(slashOffset + 1);

	// get the relative resource
	fx::ResourceManager* resourceManager = Instance<fx::ResourceManager>::Get();
	fwRefContainer<fx::Resource> resource = resourceManager->GetResource(resourceName);

	// TODO: handle this some better way
	if (!resource.GetRef())
	{
		return boost::optional<ResourceCacheEntryList::Entry>();
	}

	// get the entry list component
	fwRefContainer<ResourceCacheEntryList> entryList = resource->GetComponent<ResourceCacheEntryList>();

	// get the entry from the component
	auto entry = entryList->GetEntry(itemName);

	return entry;
}

ResourceCacheDevice::THandle ResourceCacheDevice::OpenInternal(const std::string& fileName, uint64_t* bulkPtr)
{
	// find the entry for this file
	auto entry = GetEntryForFileName(fileName);

	if (!entry.is_initialized())
	{
		return InvalidHandle;
	}

	// allocate a file handle
	THandle handle;
	auto handleData = AllocateHandle(&handle);

	// is this a bulk handle?
	handleData->bulkHandle = (bulkPtr != nullptr);
	handleData->entry = entry.get();
	handleData->status = HandleData::StatusNotFetched;

	// open the file beforehand if it's in the cache
	auto cacheEntry = m_cache->GetEntryFor(entry->referenceHash);
	
	if (cacheEntry.is_initialized())
	{
		const std::string& localPath = cacheEntry->GetLocalPath();

		handleData->parentDevice = vfs::GetDevice(localPath);
		
		if (handleData->parentDevice.GetRef())
		{
			handleData->parentHandle = (bulkPtr) ? handleData->parentDevice->OpenBulk(localPath, &handleData->bulkPtr) : handleData->parentDevice->Open(localPath, true);

			if (handleData->parentHandle != InvalidHandle)
			{
				handleData->status = HandleData::StatusFetched;
				handleData->metaData = cacheEntry->GetMetaData();

				MarkFetched(handleData);
			}
		}
	}

	if (handleData->status != HandleData::StatusFetched)
	{
		if (g_downloadedSet.find(handleData->entry.referenceHash) != g_downloadedSet.end())
		{
			trace("Huh - we fetched %s already, and it isn't in the cache now. That's strange.\n", handleData->entry.basename);
		}
	}

	return handle;
}

ResourceCacheDevice::THandle ResourceCacheDevice::Open(const std::string& fileName, bool readOnly)
{
	// this is a read-only device
	if (!readOnly)
	{
		return InvalidHandle;
	}

	// open the file
	return OpenInternal(fileName, nullptr);
}

ResourceCacheDevice::THandle ResourceCacheDevice::OpenBulk(const std::string& fileName, uint64_t* ptr)
{
	*ptr = 0;

	return OpenInternal(fileName, ptr);
}

auto ResourceCacheDevice::AllocateHandle(THandle* idx) -> HandleData*
{
	std::lock_guard<std::mutex> lock(m_handleLock);

	for (int i = 0; i < _countof(m_handles); i++)
	{
		if (m_handles[i].status == HandleData::StatusEmpty)
		{
			*idx = i;
			m_handles[i].status = HandleData::StatusError;

			return &m_handles[i];
		}
	}

	FatalError(__FUNCTION__ " - failed to allocate file handle");

	return nullptr;
}

static int GetWeightForFileName(const std::string& fileName)
{
	auto ext = fileName.substr(fileName.find_last_of('.'));

	if (ext == ".ybn" || ext == ".ymap" || ext == ".ytyp")
	{
		return 255;
	}
	else if (ext == ".ydd" || ext == ".ydr")
	{
		return 128;
	}
	else if (ext == ".ytd" || ext == ".rpf" || ext == ".gfx")
	{
		return 64;
	}
	else if (fileName.find("+hi") != std::string::npos || fileName.find("_hi") != std::string::npos)
	{
		return 16;
	}

	return 32;
}

bool ResourceCacheDevice::EnsureFetched(HandleData* handleData)
{
	PROFILE;

	// is it fetched already?
	if (handleData->status == HandleData::StatusFetched)
	{
		return true;
	}

	if (handleData->status == HandleData::StatusFetching)
	{
		if (m_blocking)
		{
			BROFILER_EVENT("block on Fetching");

			std::unique_lock<std::mutex> lock(handleData->lockMutex);
			handleData->lockVar.wait(lock);
		}

		return false;
	}

	BROFILER_EVENT("set StatusFetching");

	handleData->status = HandleData::StatusFetching;

	// log the request starting
	uint32_t initTime = timeGetTime();

	trace(__FUNCTION__ " downloading %s (hash %s) from %s\n", handleData->entry.basename.c_str(), handleData->entry.referenceHash.c_str(), handleData->entry.remoteUrl.c_str());

	// file extension for cache stuff
	std::string extension = handleData->entry.basename.substr(handleData->entry.basename.find_last_of('.') + 1);
	std::string outFileName = m_cachePath + extension + "_" + handleData->entry.referenceHash;

	HttpRequestOptions options;
	options.progressCallback = [this, handleData](const ProgressInfo& info)
	{
		handleData->downloadProgress = info.downloadNow;
		handleData->downloadSize = info.downloadTotal;

		if (info.downloadTotal != 0)
		{
			fx::OnCacheDownloadStatus(fmt::sprintf("%s%s/%s", m_pathPrefix, handleData->entry.resourceName, handleData->entry.basename), info.downloadNow, info.downloadTotal);
		}
	};

	std::string connectionToken;
	if (Instance<ICoreGameInit>::Get()->GetData("connectionToken", &connectionToken))
	{
		options.headers["X-CitizenFX-Token"] = connectionToken;
	}

	options.weight = GetWeightForFileName(handleData->entry.basename);

	// http request
	handleData->getRequest = m_httpClient->DoFileGetRequest(handleData->entry.remoteUrl, vfs::GetDevice(m_cachePath), outFileName, options, [=] (bool result, const char* errorData, size_t outSize)
	{
		if (result)
		{
			auto device = vfs::GetDevice(outFileName);
			outSize = device->GetLength(outFileName);
		}

		if (!result || outSize == 0)
		{
			handleData->status = HandleData::StatusError;
			handleData->getRequest = {};

			ICoreGameInit* init = Instance<ICoreGameInit>::Get();
			std::string reason;

			std::string caller;
			std::string initTime;

			if (init->GetData("gta-core-five:loadCaller", &caller))
			{
				if (!caller.empty())
				{
					init->GetData("gta-core-five:loadTime", &initTime);

					uint64_t time = GetTickCount64() - _atoi64(initTime.c_str());

					reason = fmt::sprintf("\nThis happened during a LoadObjectsNow call from %s, which by now took %d msec. Please report this.", caller, time);
				}
			}

			if (outSize == 0)
			{
				reason += "\nThe file was empty.";
			}

			trace("ResourceCacheDevice reporting failure: %s%s", errorData, reason);
			init->SetData("rcd:error", fmt::sprintf("Failed in ResourceCacheDevice: error result %s%s", errorData, reason));
		}
		else
		{
			// log success
			trace("ResourceCacheDevice: downloaded %s in %d msec (size %d)\n", handleData->entry.basename.c_str(), (timeGetTime() - initTime), outSize);

			if (g_downloadedSet.find(handleData->entry.referenceHash) != g_downloadedSet.end())
			{
				trace("Downloaded the same asset (%s) twice in the same run - that's bad.\n", handleData->entry.basename);
			}

			g_downloadedSet.insert(handleData->entry.referenceHash);

			// add the file to the resource cache
			std::map<std::string, std::string> metaData;
			metaData["filename"] = handleData->entry.basename;
			metaData["resource"] = handleData->entry.resourceName;
			metaData["from"] = handleData->entry.remoteUrl;

			AddEntryToCache(outFileName, metaData, handleData);

			// open the file as desired
			handleData->parentDevice = vfs::GetDevice(outFileName);

			if (handleData->parentDevice.GetRef())
			{
				handleData->parentHandle = (handleData->bulkHandle) ?
					handleData->parentDevice->OpenBulk(outFileName, &handleData->bulkPtr) : 
					handleData->parentDevice->Open(outFileName, true);
			}

			handleData->metaData = metaData;
			MarkFetched(handleData);

			handleData->status = HandleData::StatusFetched;
			handleData->getRequest = {};
		}

		// unblock the mutex
		handleData->lockVar.notify_all();
	});

	if (m_blocking)
	{
		BROFILER_EVENT("block on NotFetched");

		std::unique_lock<std::mutex> lock(handleData->lockMutex);
		handleData->lockVar.wait(lock);
	}

	return (handleData->status == HandleData::StatusFetched);
}

void ResourceCacheDevice::AddEntryToCache(const std::string& outFileName, std::map<std::string, std::string>& metaData, HandleData* handleData)
{
	m_cache->AddEntry(outFileName, metaData);
}

void ResourceCacheDevice::MarkFetched(HandleData* handleData)
{

}

size_t ResourceCacheDevice::Read(THandle handle, void* outBuffer, size_t size)
{
	// get the handle
	auto handleData = &m_handles[handle];

	// if the file isn't fetched, fetch it first
	bool fetched = EnsureFetched(handleData);

	// not fetched and non-blocking - return 0
	if (handleData->status == HandleData::StatusNotFetched || handleData->status == HandleData::StatusFetching)
	{
		return 0;
	}
	else if (handleData->status == HandleData::StatusError)
	{
		return -1;
	}

	return handleData->parentDevice->Read(handleData->parentHandle, outBuffer, size);
}

size_t ResourceCacheDevice::ReadBulk(THandle handle, uint64_t ptr, void* outBuffer, size_t size)
{
	// get the handle
	auto handleData = &m_handles[handle];

	// if the file isn't fetched, fetch it first
	bool fetched = EnsureFetched(handleData);

	// special sentinel for PatchStreamingPreparation to determine if file is fetched
	if (size == 0xFFFFFFFE || size == 0xFFFFFFFD)
	{
		auto getRequest = handleData->getRequest;

		if (getRequest)
		{
			// if FFFFFFFE, this is an active request; if FFFFFFFD, this isn't
			// no ExtensionCtl support exists for RageVFSDeviceAdapter yet, so we do it this way
			int newWeight = (size == 0xFFFFFFFE) ? -1 : 1;

			getRequest->SetRequestWeight(newWeight);
		}

		return (handleData->status == HandleData::StatusFetched) ? 2048 : 0;
	}

	// not fetched and non-blocking - return 0
	if (handleData->status == HandleData::StatusNotFetched || handleData->status == HandleData::StatusFetching)
	{
		return 0;
	}
	else if (handleData->status == HandleData::StatusError)
	{
		return -1;
	}

	return handleData->parentDevice->ReadBulk(handleData->parentHandle, ptr + handleData->bulkPtr, outBuffer, size);
}

size_t ResourceCacheDevice::Seek(THandle handle, intptr_t offset, int seekType)
{
	// get the handle
	auto handleData = &m_handles[handle];

	// make sure the file is fetched
	if (handleData->status != HandleData::StatusFetched)
	{
		return -1;
	}

	return handleData->parentDevice->Seek(handleData->parentHandle, offset, seekType);
}

bool ResourceCacheDevice::Close(THandle handle)
{
	// get the handle
	auto handleData = &m_handles[handle];

	bool retval = true;

	// close any parent device handle
	if (handleData->status == HandleData::StatusFetched)
	{
		retval = handleData->parentDevice->Close(handleData->parentHandle);
	}

	// clear the handle and return
	handleData->status = HandleData::StatusEmpty;

	return retval;
}

bool ResourceCacheDevice::CloseBulk(THandle handle)
{
	// get the handle
	auto handleData = &m_handles[handle];

	bool retval = true;

	// close any parent device handle
	if (handleData->status == HandleData::StatusFetched)
	{
		retval = handleData->parentDevice->CloseBulk(handleData->parentHandle);
	}

	// clear the handle and return
	handleData->status = HandleData::StatusEmpty;

	return retval;
}

ResourceCacheDevice::THandle ResourceCacheDevice::FindFirst(const std::string& folder, vfs::FindData* findData)
{
	return -1;
}

bool ResourceCacheDevice::FindNext(THandle handle, vfs::FindData* findData)
{
	return false;
}

void ResourceCacheDevice::FindClose(THandle handle)
{
	
}

size_t ResourceCacheDevice::GetLength(THandle handle)
{
	auto handleData = &m_handles[handle];

	// close any parent device handle
	if (handleData->status == HandleData::StatusFetched)
	{
		return handleData->parentDevice->GetLength(handleData->parentHandle);
	}

	return handleData->entry.size;
}

size_t ResourceCacheDevice::GetLength(const std::string& fileName)
{
	auto entry = GetEntryForFileName(fileName);

	if (entry)
	{
		return entry->size;
	}

	return -1;
}

uint32_t ResourceCacheDevice::GetAttributes(const std::string& fileName)
{
	auto entry = GetEntryForFileName(fileName);

	if (entry)
	{
		return 0;
	}

	return -1;
}

#define VFS_GET_RAGE_PAGE_FLAGS 0x20001

struct ResourceFlags
{
	uint32_t flag1;
	uint32_t flag2;
};

struct GetRagePageFlagsExtension
{
	const char* fileName; // in
	int version;
	ResourceFlags flags; // out
};

bool ResourceCacheDevice::ExtensionCtl(int controlIdx, void* controlData, size_t controlSize)
{
	if (controlIdx == VFS_GET_RAGE_PAGE_FLAGS)
	{
		GetRagePageFlagsExtension* data = (GetRagePageFlagsExtension*)controlData;

		auto entry = GetEntryForFileName(data->fileName);

		if (entry)
		{
			data->version = atoi(entry->extData["rscVersion"].c_str());
			data->flags.flag1 = strtoul(entry->extData["rscPagesVirtual"].c_str(), nullptr, 10);
			data->flags.flag2 = strtoul(entry->extData["rscPagesPhysical"].c_str(), nullptr, 10);
			return true;
		}
	}

	return false;
}

void ResourceCacheDevice::SetPathPrefix(const std::string& pathPrefix)
{
	m_pathPrefix = pathPrefix;
}

void ResourceCacheEntryList::AttachToObject(fx::Resource* resource)
{
	m_parentResource = resource;
}

void MountResourceCacheDevice(std::shared_ptr<ResourceCache> cache)
{
	vfs::Mount(new ResourceCacheDevice(cache, true), "cache:/");
	vfs::Mount(new ResourceCacheDevice(cache, false), "cache_nb:/");
}
