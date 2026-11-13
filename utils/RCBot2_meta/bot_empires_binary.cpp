#include "bot_empires_binary.h"
#include "bot.h"
#include "bot_cvars.h"
#include "bot_globals.h"
#include "miniz.h"
#include <cstdio>
#include <cstring>

namespace
{
struct KnownBinary
{
	unsigned int size;
	unsigned int crc;
};

// Builds the offsets in the bot_empires*.cpp files were checked against.
constexpr KnownBinary kKnownBinaries[] = {
	{14863947u, 0x6eaa6cc8u},
};

bool ReadFileCrc(const char* path, unsigned int& size, unsigned int& crc)
{
	FILE* f = std::fopen(path, "rb");
	if (f == nullptr)
		return false;
	unsigned char buffer[65536];
	mz_ulong c = MZ_CRC32_INIT;
	size_t total = 0;
	size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
	{
		c = mz_crc32(c, buffer, n);
		total += n;
	}
	std::fclose(f);
	size = static_cast<unsigned int>(total);
	crc = static_cast<unsigned int>(c);
	return total > 0;
}
} // namespace

bool EmpiresBinaryOffsetsTrusted()
{
	static int s_state = -1; // -1 unchecked, 0 untrusted, 1 trusted
	static bool s_warned = false;
	if (s_state < 0)
	{
		unsigned int size = 0, crc = 0;
		bool known = false;
		if (engine != nullptr)
		{
			char gameDir[512];
			engine->GetGameDir(gameDir, sizeof(gameDir));
			char path[600];
			std::snprintf(path, sizeof(path), "%s/bin/server_srv.so", gameDir);
			if (ReadFileCrc(path, size, crc))
			{
				for (const KnownBinary& k : kKnownBinaries)
					known = known || (k.size == size && k.crc == crc);
			}
			Msg("[RCBOT2] empires binary size=%u crc=%08x trusted=%d\n", size, crc, known ? 1 : 0);
			s_state = known ? 1 : 0;
		}
		else
			return false; // engine not up yet; check again later
	}
	if (s_state == 1)
		return true;
	if (rcbot_empires_force_offsets.GetInt() != 0)
	{
		if (!s_warned)
		{
			s_warned = true;
			Msg("[RCBOT2] WARNING: unknown game binary, rcbot_empires_force_offsets 1 uses the fixed offsets anyway\n");
		}
		return true;
	}
	if (!s_warned)
	{
		s_warned = true;
		Msg("[RCBOT2] WARNING: unknown game binary, fixed-offset features are OFF: revive, vehicle health fallback/armor, "
			"commander economy (resources, refinery points), engineer skill/rank readout, spawn-enabled check. "
			"Set rcbot_empires_force_offsets 1 to use them anyway.\n");
	}
	return false;
}
