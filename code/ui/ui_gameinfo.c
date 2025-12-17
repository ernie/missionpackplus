// Copyright (C) 1999-2000 Id Software, Inc.
//
//
// gameinfo.c
//

#include "ui_local.h"


//
// arena and bot info
//


int				ui_numBots;
static char		*ui_botInfos[MAX_BOTS];

static int		ui_numArenas;
static char		*ui_arenaInfos[MAX_ARENAS];

#ifndef MISSIONPACK // bk001206
static int		ui_numSinglePlayerArenas;
static int		ui_numSpecialSinglePlayerArenas;
#endif

//
// spawn cache for dynamic GT_TEAM support
//
#define SPAWN_CACHE_FILE "spawncache.dat"
#define SPAWN_CACHE_MAX_ENTRIES MAX_MAPS
#define SPAWN_CACHE_MAX_ENTSTRING 65536

typedef struct {
	char mapname[MAX_QPATH];
	int filesize;
	unsigned int checksum;
	int ffaSpawns;
	int teamSpawns;
} spawnCacheEntry_t;

static spawnCacheEntry_t spawnCache[SPAWN_CACHE_MAX_ENTRIES];
static int numSpawnCacheEntries;
static qboolean spawnCacheDirty;
static vmCvar_t ui_teamDMSpawnThreshold;

/*
===============
UI_ParseHex
===============
*/
static unsigned int UI_ParseHex( const char *str ) {
	unsigned int val = 0;
	char c;

	while ( ( c = *str++ ) != '\0' ) {
		if ( c >= '0' && c <= '9' ) {
			val = ( val << 4 ) + ( c - '0' );
		} else if ( c >= 'a' && c <= 'f' ) {
			val = ( val << 4 ) + ( c - 'a' + 10 );
		} else if ( c >= 'A' && c <= 'F' ) {
			val = ( val << 4 ) + ( c - 'A' + 10 );
		} else {
			break;
		}
	}
	return val;
}

/*
===============
UI_ParseInfos
===============
*/
int UI_ParseInfos( char *buf, int max, char *infos[] ) {
	char	*token;
	int		count;
	char	key[MAX_TOKEN_CHARS];
	char	info[MAX_INFO_STRING];

	count = 0;

	while ( 1 ) {
		token = COM_Parse( &buf );
		if ( !token[0] ) {
			break;
		}
		if ( strcmp( token, "{" ) ) {
			Com_Printf( "Missing { in info file\n" );
			break;
		}

		if ( count == max ) {
			Com_Printf( "Max infos exceeded\n" );
			break;
		}

		info[0] = '\0';
		while ( 1 ) {
			token = COM_ParseExt( &buf, qtrue );
			if ( !token[0] ) {
				Com_Printf( "Unexpected end of info file\n" );
				break;
			}
			if ( !strcmp( token, "}" ) ) {
				break;
			}
			Q_strncpyz( key, token, sizeof( key ) );

			token = COM_ParseExt( &buf, qfalse );
			if ( !token[0] ) {
				strcpy( token, "<NULL>" );
			}
			Info_SetValueForKey( info, key, token );
		}
		//NOTE: extra space for arena number
		infos[count] = UI_Alloc(strlen(info) + strlen("\\num\\") + strlen(va("%d", MAX_ARENAS)) + 1);
		if (infos[count]) {
			strcpy(infos[count], info);
			count++;
		}
	}
	return count;
}

/*
===============
UI_LoadArenasFromFile
===============
*/
static void UI_LoadArenasFromFile( char *filename ) {
	int				len;
	fileHandle_t	f;
	char			buf[MAX_ARENAS_TEXT];

	len = trap_FS_FOpenFile( filename, &f, FS_READ );
	if ( !f ) {
		trap_Print( va( S_COLOR_RED "file not found: %s\n", filename ) );
		return;
	}
	if ( len >= MAX_ARENAS_TEXT ) {
		trap_Print( va( S_COLOR_RED "file too large: %s is %i, max allowed is %i", filename, len, MAX_ARENAS_TEXT ) );
		trap_FS_FCloseFile( f );
		return;
	}

	trap_FS_Read( buf, len, f );
	buf[len] = 0;
	trap_FS_FCloseFile( f );

	ui_numArenas += UI_ParseInfos( buf, MAX_ARENAS - ui_numArenas, &ui_arenaInfos[ui_numArenas] );
}

/*
===============
UI_FindSpawnCacheEntry
===============
*/
static spawnCacheEntry_t *UI_FindSpawnCacheEntry( const char *mapname ) {
	int i;
	for ( i = 0; i < numSpawnCacheEntries; i++ ) {
		if ( Q_stricmp( spawnCache[i].mapname, mapname ) == 0 ) {
			return &spawnCache[i];
		}
	}
	return NULL;
}

/*
===============
UI_ComputeBSPChecksum
===============
*/
static unsigned int UI_ComputeBSPChecksum( const char *mapname, int *outFilesize ) {
	fileHandle_t f;
	char path[MAX_QPATH];
	unsigned char buf[4096];
	unsigned int checksum = 0;
	int filesize, readlen, i;

	Com_sprintf( path, sizeof( path ), "maps/%s.bsp", mapname );
	filesize = trap_FS_FOpenFile( path, &f, FS_READ );

	if ( outFilesize ) {
		*outFilesize = filesize;
	}

	if ( filesize < 0 || !f ) {
		return 0;
	}

	// Read first 4KB (or less if file smaller)
	readlen = ( filesize > 4096 ) ? 4096 : filesize;
	trap_FS_Read( buf, readlen, f );
	trap_FS_FCloseFile( f );

	// Simple rolling checksum
	for ( i = 0; i < readlen; i++ ) {
		checksum = checksum * 31 + buf[i];
	}
	return checksum;
}

/*
===============
UI_ScanBSPSpawns
===============
*/
static qboolean UI_ScanBSPSpawns( const char *mapname, int *ffa, int *team ) {
	fileHandle_t f;
	char path[MAX_QPATH];
	int filesize;
	int entOffset, entLength;
	char *entString;
	char *p;
	int header[2 + 17*2];  // magic, version, 17 lumps (offset, length each)

	*ffa = 0;
	*team = 0;

	Com_sprintf( path, sizeof( path ), "maps/%s.bsp", mapname );
	filesize = trap_FS_FOpenFile( path, &f, FS_READ );

	if ( filesize < 0 || !f ) {
		return qfalse;
	}

	// Read BSP header (8 bytes + 17 lumps * 8 bytes = 144 bytes)
	if ( filesize < 144 ) {
		trap_FS_FCloseFile( f );
		return qfalse;
	}

	trap_FS_Read( header, sizeof( header ), f );

	// Verify BSP magic "IBSP" and version 46
	if ( header[0] != 0x50534249 || header[1] != 46 ) {  // "IBSP" little-endian, version 46
		trap_FS_FCloseFile( f );
		return qfalse;
	}

	// Entity lump is lump 0 - offset at header[2], length at header[3]
	entOffset = header[2];
	entLength = header[3];

	if ( entLength <= 0 || entLength > SPAWN_CACHE_MAX_ENTSTRING ) {
		trap_FS_FCloseFile( f );
		return qfalse;
	}

	// Allocate and read entity string
	entString = UI_Alloc( entLength + 1 );
	if ( !entString ) {
		trap_FS_FCloseFile( f );
		return qfalse;
	}

	trap_FS_Seek( f, entOffset, FS_SEEK_SET );
	trap_FS_Read( entString, entLength, f );
	entString[entLength] = '\0';
	trap_FS_FCloseFile( f );

	// Count spawn entities by searching for classname patterns
	// FFA spawns: info_player_deathmatch, info_player_start
	p = entString;
	while ( ( p = strstr( p, "\"info_player_deathmatch\"" ) ) != NULL ) {
		(*ffa)++;
		p++;
	}
	p = entString;
	while ( ( p = strstr( p, "\"info_player_start\"" ) ) != NULL ) {
		(*ffa)++;
		p++;
	}

	// Team spawns: team_CTF_redspawn, team_CTF_bluespawn, team_CTF_redplayer, team_CTF_blueplayer
	p = entString;
	while ( ( p = strstr( p, "\"team_CTF_redspawn\"" ) ) != NULL ) {
		(*team)++;
		p++;
	}
	p = entString;
	while ( ( p = strstr( p, "\"team_CTF_bluespawn\"" ) ) != NULL ) {
		(*team)++;
		p++;
	}
	p = entString;
	while ( ( p = strstr( p, "\"team_CTF_redplayer\"" ) ) != NULL ) {
		(*team)++;
		p++;
	}
	p = entString;
	while ( ( p = strstr( p, "\"team_CTF_blueplayer\"" ) ) != NULL ) {
		(*team)++;
		p++;
	}

	// Note: UI_Alloc memory is not freed - it's from a pool that persists
	return qtrue;
}

/*
===============
UI_LoadSpawnCache
===============
*/
static void UI_LoadSpawnCache( void ) {
	fileHandle_t f;
	int len;
	char buf[16384];
	char *p, *token;
	spawnCacheEntry_t *entry;

	numSpawnCacheEntries = 0;
	spawnCacheDirty = qfalse;

	len = trap_FS_FOpenFile( SPAWN_CACHE_FILE, &f, FS_READ );
	if ( len < 0 || !f ) {
		return;  // No cache file yet
	}

	if ( len >= sizeof( buf ) ) {
		trap_FS_FCloseFile( f );
		return;  // File too large
	}

	trap_FS_Read( buf, len, f );
	buf[len] = '\0';
	trap_FS_FCloseFile( f );

	// Parse cache file: mapname,filesize,checksum,ffa,team per line
	p = buf;
	while ( *p && numSpawnCacheEntries < SPAWN_CACHE_MAX_ENTRIES ) {
		// Skip comments and empty lines
		if ( *p == '/' || *p == '\n' || *p == '\r' ) {
			while ( *p && *p != '\n' ) p++;
			if ( *p ) p++;
			continue;
		}

		entry = &spawnCache[numSpawnCacheEntries];

		// Parse mapname
		token = p;
		while ( *p && *p != ',' ) p++;
		if ( !*p ) break;
		*p++ = '\0';
		Q_strncpyz( entry->mapname, token, sizeof( entry->mapname ) );

		// Parse filesize
		token = p;
		while ( *p && *p != ',' ) p++;
		if ( !*p ) break;
		*p++ = '\0';
		entry->filesize = atoi( token );

		// Parse checksum
		token = p;
		while ( *p && *p != ',' ) p++;
		if ( !*p ) break;
		*p++ = '\0';
		entry->checksum = UI_ParseHex( token );

		// Parse ffa spawns
		token = p;
		while ( *p && *p != ',' ) p++;
		if ( !*p ) break;
		*p++ = '\0';
		entry->ffaSpawns = atoi( token );

		// Parse team spawns
		token = p;
		while ( *p && *p != '\n' && *p != '\r' ) p++;
		if ( *p ) {
			char c = *p;
			*p++ = '\0';
			entry->teamSpawns = atoi( token );
			// Skip any remaining newline chars
			while ( *p == '\n' || *p == '\r' ) p++;
		} else {
			entry->teamSpawns = atoi( token );
		}

		numSpawnCacheEntries++;
	}

	trap_Print( va( "%i spawn cache entries loaded\n", numSpawnCacheEntries ) );
}

/*
===============
UI_SaveSpawnCache
===============
*/
static void UI_SaveSpawnCache( void ) {
	fileHandle_t f;
	int i;
	char line[256];

	if ( !spawnCacheDirty ) {
		return;
	}

	trap_FS_FOpenFile( SPAWN_CACHE_FILE, &f, FS_WRITE );
	if ( !f ) {
		trap_Print( S_COLOR_YELLOW "WARNING: Could not write spawn cache\n" );
		return;
	}

	for ( i = 0; i < numSpawnCacheEntries; i++ ) {
		Com_sprintf( line, sizeof( line ), "%s,%d,%x,%d,%d\n",
			spawnCache[i].mapname,
			spawnCache[i].filesize,
			spawnCache[i].checksum,
			spawnCache[i].ffaSpawns,
			spawnCache[i].teamSpawns );
		trap_FS_Write( line, strlen( line ), f );
	}

	trap_FS_FCloseFile( f );
	spawnCacheDirty = qfalse;
	trap_Print( va( "%i spawn cache entries saved\n", numSpawnCacheEntries ) );
}

/*
===============
UI_ScanUncachedMaps
===============
*/
static void UI_ScanUncachedMaps( void ) {
	int i;
	int scanned = 0;

	for ( i = 0; i < uiInfo.mapCount; i++ ) {
		const char *mapname = uiInfo.mapList[i].mapLoadName;
		spawnCacheEntry_t *entry;
		int filesize;
		unsigned int checksum;
		int ffa, team;

		if ( !mapname || !mapname[0] ) {
			continue;
		}

		entry = UI_FindSpawnCacheEntry( mapname );
		if ( entry ) {
			continue;  // Already in cache
		}

		// Scan this map's BSP
		checksum = UI_ComputeBSPChecksum( mapname, &filesize );
		if ( filesize <= 0 ) {
			continue;  // BSP not found or unreadable
		}

		if ( !UI_ScanBSPSpawns( mapname, &ffa, &team ) ) {
			continue;  // Could not parse BSP
		}

		// Add to cache
		if ( numSpawnCacheEntries < SPAWN_CACHE_MAX_ENTRIES ) {
			entry = &spawnCache[numSpawnCacheEntries++];
			Q_strncpyz( entry->mapname, mapname, sizeof( entry->mapname ) );
			entry->filesize = filesize;
			entry->checksum = checksum;
			entry->ffaSpawns = ffa;
			entry->teamSpawns = team;
			spawnCacheDirty = qtrue;
			scanned++;
		}
	}

	if ( scanned > 0 ) {
		trap_Print( va( "%i maps scanned for spawn points\n", scanned ) );
		UI_SaveSpawnCache();
	}
}

/*
===============
UI_ApplySpawnCacheToMaps
===============
*/
static void UI_ApplySpawnCacheToMaps( void ) {
	int i;
	int threshold;
	int added = 0;

	threshold = ui_teamDMSpawnThreshold.integer;
	if ( threshold <= 0 ) {
		threshold = 8;  // Default
	}

	for ( i = 0; i < uiInfo.mapCount; i++ ) {
		const char *mapname = uiInfo.mapList[i].mapLoadName;
		spawnCacheEntry_t *entry;
		int totalSpawns;

		if ( !mapname || !mapname[0] ) {
			continue;
		}

		// Skip if already has GT_TEAM bit set
		if ( uiInfo.mapList[i].typeBits & ( 1 << GT_TEAM ) ) {
			continue;
		}

		entry = UI_FindSpawnCacheEntry( mapname );
		if ( !entry ) {
			continue;
		}

		totalSpawns = entry->ffaSpawns + entry->teamSpawns;

		// Add GT_TEAM if: FFA spawns below threshold but total spawns meet threshold
		if ( entry->ffaSpawns < threshold && totalSpawns >= threshold ) {
			uiInfo.mapList[i].typeBits |= ( 1 << GT_TEAM );
			added++;
		}
	}

	if ( added > 0 ) {
		trap_Print( va( "%i maps dynamically added GT_TEAM support\n", added ) );
	}
}

/*
===============
UI_LoadArenas
===============
*/
void UI_LoadArenas( void ) {
	int			numdirs;
	vmCvar_t	arenasFile;
	char		filename[128];
	char		dirlist[1024];
	char*		dirptr;
	int			i, n;
	int			dirlen;
	char		*type;

	ui_numArenas = 0;
	uiInfo.mapCount = 0;

	trap_Cvar_Register( &arenasFile, "g_arenasFile", "", CVAR_INIT|CVAR_ROM );
	if( *arenasFile.string ) {
		UI_LoadArenasFromFile(arenasFile.string);
	}
	else {
		UI_LoadArenasFromFile("scripts/arenas.txt");
	}

	// get all arenas from .arena files
	numdirs = trap_FS_GetFileList("scripts", ".arena", dirlist, 1024 );
	dirptr  = dirlist;
	for (i = 0; i < numdirs; i++, dirptr += dirlen+1) {
		dirlen = strlen(dirptr);
		strcpy(filename, "scripts/");
		strcat(filename, dirptr);
		UI_LoadArenasFromFile(filename);
	}
	trap_Print( va( "%i arenas parsed\n", ui_numArenas ) );
	if (UI_OutOfMemory()) {
		trap_Print(S_COLOR_YELLOW"WARNING: not anough memory in pool to load all arenas\n");
	}

	for( n = 0; n < ui_numArenas; n++ ) {
		// determine type

		uiInfo.mapList[uiInfo.mapCount].cinematic = -1;
		uiInfo.mapList[uiInfo.mapCount].mapLoadName = String_Alloc(Info_ValueForKey(ui_arenaInfos[n], "map"));
		uiInfo.mapList[uiInfo.mapCount].mapName = String_Alloc(Info_ValueForKey(ui_arenaInfos[n], "longname"));
		uiInfo.mapList[uiInfo.mapCount].levelShot = -1;
		uiInfo.mapList[uiInfo.mapCount].imageName = String_Alloc(va("levelshots/%s", uiInfo.mapList[uiInfo.mapCount].mapLoadName));
		uiInfo.mapList[uiInfo.mapCount].typeBits = 0;

		type = Info_ValueForKey( ui_arenaInfos[n], "type" );
		// if no type specified, it will be treated as "ffa"
		if( *type ) {
			if( strstr( type, "ffa" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_FFA);
			}
			if( strstr( type, "tourney" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_TOURNAMENT);
			}
			if( strstr( type, "team" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_TEAM);
			}
			if( strstr( type, "ctf" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_CTF);
			}
			if( strstr( type, "oneflag" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_1FCTF);
			}
			if( strstr( type, "overload" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_OBELISK);
			}
			if( strstr( type, "harvester" ) ) {
				uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_HARVESTER);
			}
		} else {
			uiInfo.mapList[uiInfo.mapCount].typeBits |= (1 << GT_FFA);
		}

		uiInfo.mapCount++;
		if (uiInfo.mapCount >= MAX_MAPS) {
			break;
		}
	}

	// Load spawn cache and apply dynamic GT_TEAM support
	trap_Cvar_Register( &ui_teamDMSpawnThreshold, "g_teamDMSpawnThreshold", "8", CVAR_ARCHIVE );
	UI_LoadSpawnCache();
	UI_ScanUncachedMaps();
	UI_ApplySpawnCacheToMaps();
}


/*
===============
UI_LoadBotsFromFile
===============
*/
static void UI_LoadBotsFromFile( char *filename ) {
	int				len;
	fileHandle_t	f;
	char			buf[MAX_BOTS_TEXT];

	len = trap_FS_FOpenFile( filename, &f, FS_READ );
	if ( !f ) {
		trap_Print( va( S_COLOR_RED "file not found: %s\n", filename ) );
		return;
	}
	if ( len >= MAX_BOTS_TEXT ) {
		trap_Print( va( S_COLOR_RED "file too large: %s is %i, max allowed is %i", filename, len, MAX_BOTS_TEXT ) );
		trap_FS_FCloseFile( f );
		return;
	}

	trap_FS_Read( buf, len, f );
	buf[len] = 0;
	trap_FS_FCloseFile( f );

	COM_Compress(buf);

	ui_numBots += UI_ParseInfos( buf, MAX_BOTS - ui_numBots, &ui_botInfos[ui_numBots] );
}

/*
===============
UI_LoadBots
===============
*/
void UI_LoadBots( void ) {
	vmCvar_t	botsFile;
	int			numdirs;
	char		filename[128];
	char		dirlist[1024];
	char*		dirptr;
	int			i;
	int			dirlen;

	ui_numBots = 0;

	trap_Cvar_Register( &botsFile, "g_botsFile", "", CVAR_INIT|CVAR_ROM );
	if( *botsFile.string ) {
		UI_LoadBotsFromFile(botsFile.string);
	}
	else {
		UI_LoadBotsFromFile("scripts/bots.txt");
	}

	// get all bots from .bot files
	numdirs = trap_FS_GetFileList("scripts", ".bot", dirlist, 1024 );
	dirptr  = dirlist;
	for (i = 0; i < numdirs; i++, dirptr += dirlen+1) {
		dirlen = strlen(dirptr);
		strcpy(filename, "scripts/");
		strcat(filename, dirptr);
		UI_LoadBotsFromFile(filename);
	}
	trap_Print( va( "%i bots parsed\n", ui_numBots ) );
}


/*
===============
UI_GetBotInfoByNumber
===============
*/
char *UI_GetBotInfoByNumber( int num ) {
	if( num < 0 || num >= ui_numBots ) {
		trap_Print( va( S_COLOR_RED "Invalid bot number: %i\n", num ) );
		return NULL;
	}
	return ui_botInfos[num];
}


/*
===============
UI_GetBotInfoByName
===============
*/
char *UI_GetBotInfoByName( const char *name ) {
	int		n;
	char	*value;

	for ( n = 0; n < ui_numBots ; n++ ) {
		value = Info_ValueForKey( ui_botInfos[n], "name" );
		if ( !Q_stricmp( value, name ) ) {
			return ui_botInfos[n];
		}
	}

	return NULL;
}

int UI_GetNumBots() {
	return ui_numBots;
}


char *UI_GetBotNameByNumber( int num ) {
	char *info = UI_GetBotInfoByNumber(num);
	if (info) {
		return Info_ValueForKey( info, "name" );
	}
	return "Sarge";
}
