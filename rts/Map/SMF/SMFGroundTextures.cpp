/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "StdAfx.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "SMFGroundTextures.h"
#include "Rendering/GL/PBO.h"
#include "Rendering/GlobalRendering.h"
#include "Map/SMF/mapfile.h"
#include "Map/SMF/SMFReadMap.h"
#include "Map/MapInfo.h"
#include "Game/Camera.h"
#include "Game/Game.h"
#include "Game/GameSetup.h"
#include "Game/LoadScreen.h"
#include "System/Exceptions.h"
#include "System/FastMath.h"
#include "System/LogOutput.h"
#include "System/mmgr.h"
#include "System/TimeProfiler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/FileSystem/FileSystem.h"

using std::sprintf;
using std::string;
using std::max;
using std::min;

CBFGroundTextures::CBFGroundTextures(CSmfReadMap* rm) :
	bigSquareSize(128),
	numBigTexX(gs->mapx / bigSquareSize),
	numBigTexY(gs->mapy / bigSquareSize)
{
	// todo: refactor: put reading code in CSmfFile and keep errorhandling/progress reporting here..
	map = rm;
	CFileHandler* ifs = rm->GetFile().GetFileHandler();
	const SMFHeader* header = &map->GetFile().GetHeader();

	ifs->Seek(header->tilesPtr);
	tileSize = header->tilesize;

	MapTileHeader tileHeader;
	READPTR_MAPTILEHEADER(tileHeader, ifs);

	tileMap = new int[(header->mapx * header->mapy) / 16];
	tiles = new char[tileHeader.numTiles * SMALL_TILE_SIZE];
	int curTile = 0;


	const std::string smfDir = filesystem.GetDirectory(gameSetup->MapFile());
	const CMapInfo::smf_t& smf = mapInfo->smf;
	bool smtHeaderOverride = false;

	if (!smf.smtFileNames.empty()) {
		if (smf.smtFileNames.size() != tileHeader.numTileFiles) {
			logOutput.Print(
				"[CBFGroundTextures] mismatched number of .smt file "
				"references between map's .smd ("_STPF_") and header (%d);"
				" ignoring .smd overrides",
				smf.smtFileNames.size(), tileHeader.numTileFiles
			);
		} else {
			smtHeaderOverride = true;
		}
	}


	loadscreen->SetLoadMessage("Loading Tile Files");

	for (int a = 0; a < tileHeader.numTileFiles; ++a) {
		int numSmallTiles;
		ifs->Read(&numSmallTiles, 4);
		numSmallTiles = swabdword(numSmallTiles);


		std::string smtFilePath, smtFileName;

		//! eat the zero-terminated name
		while (true) {
			char ch = 0;
			ifs->Read(&ch, 1);

			if (ch == 0) {
				break;
			}

			smtFileName += ch;
		}

		if (!smtHeaderOverride) {
			smtFilePath = smfDir + smtFileName;
		} else {
			smtFilePath = smfDir + smf.smtFileNames[a];
		}

		//logOutput.Print("Loading .smt tile-file \"%s\"", smtFilePath.c_str());

		CFileHandler tileFile(smtFilePath);

		if (!tileFile.FileExists()) {
			//! try absolute path
			if (!smtHeaderOverride) {
				smtFilePath = smtFileName;
			} else {
				smtFilePath = smf.smtFileNames[a];
			}
			tileFile = CFileHandler(smtFilePath);
		}

		if (!tileFile.FileExists()) {
			logOutput.Print(
				"[CBFGroundTextures] could not find .smt tile-file "
				"\"%s\" (all %d missing tiles will be colored red)",
				smtFilePath.c_str(), numSmallTiles
			);
			memset(&tiles[curTile * SMALL_TILE_SIZE], 0xaa, numSmallTiles * SMALL_TILE_SIZE);
			curTile += numSmallTiles;
			continue;
		}

		TileFileHeader tfh;
		READ_TILEFILEHEADER(tfh, tileFile);

		if (strcmp(tfh.magic, "spring tilefile") != 0 || tfh.version != 1 || tfh.tileSize != 32 || tfh.compressionType != 1) {
			char t[500];
			sprintf(t, "[CBFGroundTextures] file \"%s\" does not match .smt format", smtFilePath.c_str());
			throw content_error(t);
		}

		for (int b = 0; b < numSmallTiles; ++b) {
			tileFile.Read(&tiles[curTile * SMALL_TILE_SIZE], SMALL_TILE_SIZE);
			curTile++;
		}
	}

	loadscreen->SetLoadMessage("Loading Tile Map");

	int count = (header->mapx * header->mapy) / 16;
	ifs->Read(tileMap, count * sizeof(int));

	for (int i = 0; i < count; i++) {
		tileMap[i] = swabdword(tileMap[i]);
	}

	tileMapXSize = header->mapx / 4;
	tileMapYSize = header->mapy / 4;

	squares = new GroundSquare[numBigTexX * numBigTexY];

	for (int y = 0; y < numBigTexY; ++y) {
		for (int x = 0; x < numBigTexX; ++x) {
			GroundSquare* square = &squares[y * numBigTexX + x];
			square->lastUsed = 1;
			LoadSquare(x, y, 3);
		}
	}


	ScopedOnceTimer timer("generating MipMaps");

	const float* hdata = map->GetMIPHeightMapSynced(1);
	const int mx = header->mapx / 2;
	const int nbx = numBigTexX;
	const int nb = numBigTexX * numBigTexY;

	// 64 is the heightmap square-size at MIP level 1
	static const int mipSquareSize = 64;

	heightMaxima.resize(nb, readmap->currMinHeight);
	heightMinima.resize(nb, readmap->currMaxHeight);
	stretchFactors.resize(nb, 0.0f);

	for (int y = 0; y < numBigTexY; ++y) {
		for (int x = 0; x < numBigTexX; ++x) {

			// NOTE: we leave out the borders on sampling because it is easier to do the Sobel kernel convolution
			for (int x2 = x * mipSquareSize + 1; x2 < (x + 1) * mipSquareSize - 1; x2++) {
				for (int y2 = y * mipSquareSize + 1; y2 < (y + 1) * mipSquareSize - 1; y2++) {

					heightMaxima[y * nbx + x] = std::max( hdata[y2 * mx + x2], heightMaxima[y * nbx + x]);
					heightMinima[y * nbx + x] = std::min( hdata[y2 * mx + x2], heightMinima[y * nbx + x]);

					const float gx =
						-1.0f * hdata[(y2-1) * mx + x2-1] + // Gx sobel kernel
						-2.0f * hdata[(y2  ) * mx + x2-1] +
						-1.0f * hdata[(y2+1) * mx + x2-1] +
						 1.0f * hdata[(y2-1) * mx + x2+1] +
						 2.0f * hdata[(y2  ) * mx + x2+1] +
						 1.0f * hdata[(y2+1) * mx + x2+1] ;
					const float gy =
						-1.0f * hdata[(y2+1) * mx + x2-1] + // Gy sobel kernel
						-2.0f * hdata[(y2+1) * mx + x2  ] +
						-1.0f * hdata[(y2+1) * mx + x2+1] +
						 1.0f * hdata[(y2-1) * mx + x2-1] +
						 2.0f * hdata[(y2-1) * mx + x2  ] +
						 1.0f * hdata[(y2-1) * mx + x2+1] ;

					// linear sum, no need for fancy sqrt
					const float g = (std::fabs(gx) + std::fabs(gy)) / mipSquareSize;

					/*
					square to amplify large stretches of height.
					in fact, this should probably be different,
					as g of 64 (8*(1+2+1+1+2+1) would mean a 45 degree angle (which is what I think is streched),
					we should divide by 64 before squarification to supress lower values*/ 
					stretchFactors[y * nbx + x] += (g * g);
				}
			}

			stretchFactors[y * nbx + x] += 1.0f;
		}
	}
}

CBFGroundTextures::~CBFGroundTextures(void)
{
	for (int i = 0; i < numBigTexX * numBigTexY; ++i) {
		glDeleteTextures(1, &squares[i].texture);
	}

	delete[] squares;
	delete[] tileMap;
	delete[] tiles;

	heightMaxima.clear();
	heightMinima.clear();
	stretchFactors.clear();
}


void CBFGroundTextures::SetTexture(int x, int y)
{
	GroundSquare* square = &squares[y * numBigTexX + x];
	glBindTexture(GL_TEXTURE_2D, square->texture);

	if (game->GetDrawMode() == CGame::gameNormalDraw) {
		square->lastUsed = globalRendering->drawFrame;
	}
}

inline bool CBFGroundTextures::TexSquareInView(int btx, int bty) {
	static const float* shm = map->GetCornerHeightMapSynced();

	static const int heightMapSizeX = gs->mapx + 1;
	static const int bigTexW = (gs->mapx << 3) / numBigTexX;
	static const int bigTexH = (gs->mapy << 3) / numBigTexY;
	static const float bigTexSquareRadius = fastmath::apxsqrt(float(bigTexW * bigTexW + bigTexH * bigTexH));

	const int x = btx * bigTexW + (bigTexW >> 1);
	const int y = bty * bigTexH + (bigTexH >> 1);
	const int idx = (y >> 3) * heightMapSizeX + (x >> 3);
	const float3 bigTexSquarePos(x, shm[idx], y);

	return (cam2->InView(bigTexSquarePos, bigTexSquareRadius));
}

void CBFGroundTextures::DrawUpdate(void)
{
	// screen-diagonal number of pixels
	const float diag = fastmath::apxsqrt(globalRendering->viewSizeX * globalRendering->viewSizeX + globalRendering->viewSizeY * globalRendering->viewSizeY);

	for (int y = 0; y < numBigTexY; ++y) {
		float dy =
			cam2->pos.z -
			y * bigSquareSize * SQUARE_SIZE -
			(SQUARE_SIZE << 6);
		dy = max(0.0f, float(fabs(dy) - (SQUARE_SIZE << 6)));

		for (int x = 0; x < numBigTexX; ++x) {
			GroundSquare* square = &squares[y * numBigTexX + x];

			if (!TexSquareInView(x, y)) {
				if ((square->texLevel < 3) && (globalRendering->drawFrame - square->lastUsed > 120)) {
					// `unload` texture (= load lowest mipmap)
					// if the square wasn't visible for 120 vframes
					glDeleteTextures(1, &square->texture);
					LoadSquare(x, y, 3);
				}
				continue;
			}

			float dx =
				cam2->pos.x -
				x * bigSquareSize * SQUARE_SIZE -
				(SQUARE_SIZE << 6);
			dx = max(0.0f, float(fabs(dx) - (SQUARE_SIZE << 6)));

			const float dz = max(cam2->pos.y - (heightMaxima[y * numBigTexX + x] + heightMinima[y * numBigTexX + x]) / 2, 0.0f);
			const float dist = fastmath::apxsqrt(dx * dx + dy * dy + dz * dz);

			// we work under the following assumptions:
			//    the minimum mip level is the closest ceiling mip level that we can use
			//    based on distance, FOV and tile size; we can increase this mip level IF
			//    the stretch factor requires us to do so.
			//
			//    we will approximate tile size with a sphere 512 elmos in radius, which
			//    translates to a diameter of =~ sqrt2 * 1024 =~ 1400 pixels
			//
			//    half (vertical) FOV is 45 degs, for default and most other camera modes
			int wantedLevel = 0;
			float heightDiff = heightMaxima[y * numBigTexX + x] - heightMinima[y * numBigTexX + x];
			int screenPixels = 1024;

			if (dist > 0.0f) {
				if (heightDiff > 1024.0f) {
					// this means the heightmap chunk is taller than it is wide,
					// so we use the tallness metric instead for calculating its
					// on-screen size in pixels
					screenPixels = int((heightDiff) * (diag * 0.5f) / dist);
				} else {
					screenPixels = int(1024 * (diag * 0.5f) / dist);
				}
			}

			if (screenPixels > 513)
				wantedLevel = 0;
			else if (screenPixels > 257)
				wantedLevel = 1;
			else if (screenPixels > 129)
				wantedLevel = 2;
			else
				wantedLevel = 3;

			// 16K is an approximation of the Sobel sum required to have a
			// heightmap that has double the texture area of a flat square
			if (stretchFactors[y * numBigTexX + x] > 16000 && wantedLevel > 0)
				wantedLevel--;

			if (square->texLevel != wantedLevel) {
				glDeleteTextures(1, &square->texture);
				LoadSquare(x, y, wantedLevel);
			}
		}
	}
}

int tileoffset[] = {0, 512, 640, 672};

void CBFGroundTextures::LoadSquare(int x, int y, int level)
{
	int size = 1024 >> level;

	pbo.Bind();
	pbo.Resize(size * size / 2);
	GLint* buf = (GLint*)pbo.MapBuffer();

	GroundSquare* square = &squares[y * numBigTexX + x];
	square->texLevel = level;
	int numblocks = 8 >> level;

	for (int y1 = 0; y1 < 32; y1++) {
		for (int x1 = 0; x1 < 32; x1++) {
			GLint* tile = (GLint*)&tiles[tileMap[(x1 + x * 32) + (y1 + y * 32) * tileMapXSize] * SMALL_TILE_SIZE + tileoffset[level]];

			const int doff = x1 * numblocks + y1 * numblocks * numblocks * 32;
			for (int yt = 0; yt < numblocks; yt++) {
				GLint* sbuf = &tile[yt * numblocks * 2];
				GLint* dbuf = &buf[(doff + yt * numblocks * 32) * 2];
				memcpy(dbuf, sbuf, numblocks * 2 * sizeof(GLint));
			}
		}
	}

	pbo.UnmapBuffer();

	glGenTextures(1, &square->texture);
	glBindTexture(GL_TEXTURE_2D, square->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	if (GLEW_EXT_texture_edge_clamp) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (map->anisotropy != 0.0f)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, map->anisotropy);
	if (level<2) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 1);
	} else {
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, 0.5f);
	}

	glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, size, size, 0, size * size / 2, pbo.GetPtr());
	pbo.Unbind();
}
