/*
 * CMapService.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "CMapService.h"

#include "../filesystem/Filesystem.h"
#include "../filesystem/CBinaryReader.h"
#include "../filesystem/CCompressedStream.h"
#include "../filesystem/CMemoryStream.h"
#include "../filesystem/CMemoryBuffer.h"

#include "CMap.h"

#include "MapFormatH3M.h"
#include "MapFormatJson.h"

VCMI_LIB_NAMESPACE_BEGIN


std::unique_ptr<CMap> CMapService::loadMap(const ResourceID & name) const
{
	auto stream = getStreamFromFS(name);
	return getMapLoader(stream)->loadMap();
}

std::unique_ptr<CMapHeader> CMapService::loadMapHeader(const ResourceID & name) const
{
	auto stream = getStreamFromFS(name);
	return getMapLoader(stream)->loadMapHeader();
}

std::unique_ptr<CMap> CMapService::loadMap(const ui8 * buffer, int size, const std::string & name) const
{
	auto stream = getStreamFromMem(buffer, size);
	std::unique_ptr<CMap> map(getMapLoader(stream)->loadMap());
	std::unique_ptr<CMapHeader> header(map.get());

	//might be original campaign and require patch
	getMapPatcher(name)->patchMapHeader(header);
	header.release();

	return map;
}

std::unique_ptr<CMapHeader> CMapService::loadMapHeader(const ui8 * buffer, int size, const std::string & name) const
{
	auto stream = getStreamFromMem(buffer, size);
	std::unique_ptr<CMapHeader> header = getMapLoader(stream)->loadMapHeader();

	//might be original campaign and require patch
	getMapPatcher(name)->patchMapHeader(header);
	return header;
}

void CMapService::saveMap(const std::unique_ptr<CMap> & map, boost::filesystem::path fullPath) const
{
	CMemoryBuffer serializeBuffer;
	{
		CMapSaverJson saver(&serializeBuffer);
		saver.saveMap(map);
	}
	{
		boost::filesystem::remove(fullPath);
		boost::filesystem::ofstream tmp(fullPath, boost::filesystem::ofstream::binary);

		tmp.write((const char *)serializeBuffer.getBuffer().data(),serializeBuffer.getSize());
		tmp.flush();
		tmp.close();
	}
}

std::unique_ptr<CInputStream> CMapService::getStreamFromFS(const ResourceID & name)
{
	return CResourceHandler::get()->load(name);
}

std::unique_ptr<CInputStream> CMapService::getStreamFromMem(const ui8 * buffer, int size)
{
	return std::unique_ptr<CInputStream>(new CMemoryStream(buffer, size));
}

std::unique_ptr<IMapLoader> CMapService::getMapLoader(std::unique_ptr<CInputStream> & stream)
{
	// Read map header
	CBinaryReader reader(stream.get());
	ui32 header = reader.readUInt32();
	reader.getStream()->seek(0);

	//check for ZIP magic. Zip files are VCMI maps
	switch(header)
	{
	case 0x06054b50:
	case 0x04034b50:
	case 0x02014b50:
		return std::unique_ptr<IMapLoader>(new CMapLoaderJson(stream.get()));
		break;
	default:
		// Check which map format is used
		// gzip header is 3 bytes only in size
		switch(header & 0xffffff)
		{
			// gzip header magic number, reversed for LE
			case 0x00088B1F:
				stream = std::unique_ptr<CInputStream>(new CCompressedStream(std::move(stream), true));
				return std::unique_ptr<IMapLoader>(new CMapLoaderH3M(stream.get()));
			case EMapFormat::WOG :
			case EMapFormat::AB  :
			case EMapFormat::ROE :
			case EMapFormat::SOD :
				return std::unique_ptr<IMapLoader>(new CMapLoaderH3M(stream.get()));
			default :
				throw std::runtime_error("Unknown map format");
		}
	}
}

static JsonNode loadPatches(std::string path)
{
	JsonNode node = JsonUtils::assembleFromFiles(path);
	for (auto & entry : node.Struct())
		JsonUtils::validate(entry.second, "vcmi:mapHeader", "patch for " + entry.first);
	return node;
}

std::unique_ptr<IMapPatcher> CMapService::getMapPatcher(std::string scenarioName)
{
	static JsonNode node;

	if (node.isNull())
		node = loadPatches("config/mapOverrides.json");

	boost::to_lower(scenarioName);
	logGlobal->debug("Request to patch map %s", scenarioName);
	return std::unique_ptr<IMapPatcher>(new CMapPatcher(node[scenarioName]));
}

VCMI_LIB_NAMESPACE_END
