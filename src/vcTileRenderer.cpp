#include "vcTileRenderer.h"
#include "vcQuadTree.h"
#include "vcGIS.h"
#include "vcSettings.h"

#include "gl/vcGLState.h"
#include "gl/vcMeshUtils.h"
#include "gl/vcRenderShaders.h"
#include "gl/vcShader.h"
#include "gl/vcMesh.h"

#include "udPlatform/udThread.h"
#include "udPlatform/udFile.h"
#include "udPlatform/udPlatformUtil.h"
#include "udPlatform/udChunkedArray.h"

#include "stb_image.h"

// Debug tiles with colour information
#define VISUALIZE_DEBUG_TILES 0

vcTexture *pDEMTexture[2] = {};

enum
{
  TileVertexResolution = 5, // note: this should be odd
  TileIndexResolution = (TileVertexResolution - 1),

  MaxTileRequestAttempts = 3,
};

struct vcTileRenderer
{
  vcSettings *pSettings;
  vcQuadTree quadTree;

  vcMesh *pTileMeshes[16];
  vcTexture *pEmptyTileTexture;

  udDouble3 cameraPosition;

  // cache textures
  struct vcTileCache
  {
    volatile bool keepLoading;
    udThread *pThreads[4];
    udSemaphore *pSemaphore;
    udMutex *pMutex;
    udChunkedArray<vcQuadTreeNode*> tileLoadList;
  } cache;

  struct
  {
    vcShader *pProgram;
    vcShaderConstantBuffer *pConstantBuffer;
    vcShaderSampler *uniform_texture;
    vcShaderSampler *uniform_dem0;
    vcShaderSampler *uniform_dem1;

    struct
    {
      udFloat4x4 projectionMatrix;
      udFloat4x4 viewMatrix;
      udFloat4 eyePositions[9];//TileVertexResolution * TileVertexResolution];
      udFloat4 colour;
      udFloat4 demUVs[9 * 2];
    } everyObject;
  } presentShader;
};

struct vcTileVertex
{
  udFloat2 uv;
};
const vcVertexLayoutTypes vcTileVertexLayout[] = { vcVLT_TextureCoords2 };

void vcTileRenderer_LoadThread(void *pThreadData)
{
  vcTileRenderer *pRenderer = (vcTileRenderer*)pThreadData;
  vcTileRenderer::vcTileCache *pCache = &pRenderer->cache;

  while (pCache->keepLoading)
  {
    int loadStatus = udWaitSemaphore(pCache->pSemaphore, 1000);

    if (loadStatus != 0 && pCache->tileLoadList.length == 0)
      continue;

    while (pCache->tileLoadList.length > 0 && pCache->keepLoading)
    {
      udLockMutex(pCache->pMutex);

      // TODO: Store in priority order and recalculate on insert/delete
      int best = -1;
      vcQuadTreeNode *pNode = nullptr;
      udDouble3 tileCenter = udDouble3::zero();
      double bestDistancePrioritySqr = FLT_MAX;

      for (size_t i = 0; i < pCache->tileLoadList.length; ++i)
      {
        pNode = pCache->tileLoadList[i];
        if (!pNode->renderInfo.tryLoad || !pNode->touched || pNode->renderInfo.loadStatus != vcNodeRenderInfo::vcTLS_InQueue)
          continue;

        tileCenter = udDouble3::create(pNode->renderInfo.center, pRenderer->pSettings->maptiles.mapHeight);
        double distanceToCameraSqr = udMagSq3(tileCenter - pRenderer->cameraPosition);

        // root (special case)
        if (pNode == &pRenderer->quadTree.nodes.pPool[pRenderer->quadTree.rootIndex])
        {
          best = int(i);
          break;
        }

        bool betterNode = true;
        if (best != -1)
        {
          vcQuadTreeNode *pBestNode = pCache->tileLoadList[best];

          // priorities: visibility > failed to render visible area > distance
          betterNode = pNode->visible && !pBestNode->visible;
          if (pNode->visible == pBestNode->visible)
          {
            betterNode = !pNode->rendered && pBestNode->rendered;
            if (pNode->rendered == pBestNode->rendered)
            {
              betterNode = distanceToCameraSqr < bestDistancePrioritySqr;
            }
          }
        }

        if (betterNode)
        {
          bestDistancePrioritySqr = distanceToCameraSqr;
          best = int(i);
        }
      }

      if (best == -1)
      {
        udReleaseMutex(pCache->pMutex);
        continue;
      }

      vcQuadTreeNode *pBestNode = pCache->tileLoadList[best];
      pBestNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_Downloading;
      pBestNode->renderInfo.loadAttempts++;
      pCache->tileLoadList.RemoveSwapLast(best);

      char buff[256];
      udSprintf(buff, sizeof(buff), "%s/%d/%d/%d.%s", pRenderer->pSettings->maptiles.tileServerAddress, pBestNode->slippyPosition.z, pBestNode->slippyPosition.x, pBestNode->slippyPosition.y, pRenderer->pSettings->maptiles.tileServerExtension);
      udReleaseMutex(pCache->pMutex);

      bool failedLoad = false;

      void *pFileData = nullptr;
      int64_t fileLen = -1;
      int width = 0;
      int height = 0;
      int channelCount = 0;
      uint8_t *pData = nullptr;

      if (udFile_Load(buff, &pFileData, &fileLen) != udR_Success || fileLen == 0)
      {
        // Failed to load tile from server
        failedLoad = true;
      }
      else
      {
        pData = stbi_load_from_memory((stbi_uc*)pFileData, (int)fileLen, (int*)&width, (int*)&height, (int*)&channelCount, 4);
        if (pData == nullptr)
        {
          // Failed to load tile from memory
          failedLoad = true;
        }
      }

      // Node has been invalidated since download started
      if (!pBestNode->touched)
      {
        // TODO: Put into LRU texture cache (but for now just throw it out)
        pBestNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_None;
        goto epilogue;
      }

      if (failedLoad)
      {
        if (pBestNode->renderInfo.loadAttempts < MaxTileRequestAttempts)
        {
          pBestNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_InQueue;
          udLockMutex(pCache->pMutex);
          pCache->tileLoadList.PushBack(pBestNode); // put it back
          udReleaseMutex(pCache->pMutex);
        }
        else
        {
          pBestNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_Failed;
        }
        goto epilogue;
      }

      pBestNode->renderInfo.width = width;
      pBestNode->renderInfo.height = height;

      pBestNode->renderInfo.pData = udMemDup(pData, sizeof(uint32_t)*width*height, 0, udAF_None);

      pBestNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_Downloaded;

epilogue:
      udFree(pFileData);
      stbi_image_free(pData);
    }
  }
}

int up = 1 << 0;
int right = 1 << 1;
int down = 1 << 2;
int left = 1 << 3;

// build meshes
int meshConfigurations[] =
{
  0,

  up,                      //  ^
  right,                   //   >
  down,                    //  .
  left,                    // <

  up | right,              //  ^>
  up | down,               //  ^.
  up | left,               // <^

  right | down,            //  .>
  right | left,            // < >

  down | left,             // <.

  up | right | down,       //  ^.>
  up | left | right,       // <^>
  up | left | down,        // <^.

  down | left | right,     // <.>

  up | left | right | down // <^.>
};

void vcTileRenderer_BuildMeshVertices(vcTileVertex *pVerts, int *pIndicies, udFloat2 minUV, udFloat2 maxUV, int collapseEdgeMask)
{
  for (int y = 0; y < TileIndexResolution; ++y)
  {
    for (int x = 0; x < TileIndexResolution; ++x)
    {
      int index = y * TileIndexResolution + x;
      int vertIndex = y * TileVertexResolution + x;

      // TODO: once figured out, remove commented code from all the conditionals statements below
      pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
      pIndicies[index * 6 + 1] = vertIndex + 1;
      pIndicies[index * 6 + 2] = vertIndex;

      pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
      pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
      pIndicies[index * 6 + 5] = vertIndex + 1;

      // corner cases
      if ((collapseEdgeMask & down) && (collapseEdgeMask & right) && x >= (TileIndexResolution - 2) && y >= (TileIndexResolution - 2))
      {
        if (x == TileIndexResolution - 2 && y == TileIndexResolution - 2)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else if (x == TileIndexResolution - 1 && y == TileIndexResolution - 2)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          // collapse
          pIndicies[index * 6 + 3] = vertIndex;
          pIndicies[index * 6 + 4] = vertIndex;
          pIndicies[index * 6 + 5] = vertIndex;
        }
        else if (x == TileIndexResolution - 2 && y == TileIndexResolution - 1)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          // collapse
          pIndicies[index * 6 + 3] = vertIndex;
          pIndicies[index * 6 + 4] = vertIndex;
          pIndicies[index * 6 + 5] = vertIndex;
        }
        else // x == 1 && y == 1
        {
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution - 1;
          pIndicies[index * 6 + 1] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 2] = vertIndex;

          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 4] = vertIndex - TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
      }
      else if ((collapseEdgeMask & down) && (collapseEdgeMask & left) && x <= 1 && y >= (TileIndexResolution - 2))
      {
        if (x == 0 && y == TileIndexResolution - 2)
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex + 1;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex + 1;

          // re-orient
          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 4] = vertIndex + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
        else if (x == 1 && y == TileIndexResolution - 2)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;
          //
          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else if (x == 0 && y == TileIndexResolution - 1)
        {
          // re-orient
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex - TileVertexResolution;

          // collapse
          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 2;
          pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else // x == 1 && y == 1
        {
          // collapse
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 2] = vertIndex + TileVertexResolution;

          // re-orient
          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 4] = vertIndex + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
      }
      else if ((collapseEdgeMask & up) && (collapseEdgeMask & right) && x >= (TileIndexResolution - 2) && y <= 1)
      {
        if (x == TileIndexResolution - 2 && y == 0)
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex;
          pIndicies[index * 6 + 1] = vertIndex;
          pIndicies[index * 6 + 2] = vertIndex;

          // re-orient triangle
          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
        else if (x == TileIndexResolution - 1 && y == 0)
        {
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex - 1;

          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else if (x == TileIndexResolution - 2 && y == 1)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;
          //
          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else // x == TileIndexResolution -1 && y == 1
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex;
          pIndicies[index * 6 + 1] = vertIndex;
          //pIndicies[index * 6 + 2] = vertIndex;

          // re-orient triangle
          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
      }
      else if ((collapseEdgeMask & up) && (collapseEdgeMask & left) && x <= 1 && y <= 1)
      {
        if (x == 0 && y == 0)
        {
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 1] = vertIndex + 2;
          pIndicies[index * 6 + 2] = vertIndex;

          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution + TileVertexResolution;
          pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex;
        }
        else if (x == 1 && y == 0)
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex + 1;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex + 1;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else if (x == 0 && y == 1)
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex + 1;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex + 1;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else // x==1, y == 1
        {
          // do nothing
        }
      }
      else if (y == 0 && (collapseEdgeMask & up))
      {
        if ((x & 1) == 0)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex + 2;
          //pIndicies[index * 6 + 2] = vertIndex;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex + 2;
        }
        else
        {
          // collapse
          pIndicies[index * 6 + 0] = vertIndex + 1;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          pIndicies[index * 6 + 2] = vertIndex + 1;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
      }
      else if (y == TileIndexResolution - 1 && (collapseEdgeMask & down))
      {
        if ((x & 1) == 0)
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          // collapse
          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 5] = vertIndex + TileVertexResolution;
        }
        else
        {
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution - 1;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution - 1;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
      } else if (x == TileIndexResolution - 1 && (collapseEdgeMask & right))
      {
        if ((y & 1) == 0)
        {
         // pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          // collapse
          pIndicies[index * 6 + 3] = vertIndex + 1;
          pIndicies[index * 6 + 4] = vertIndex + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else
        {
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex - TileVertexResolution + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          pIndicies[index * 6 + 5] = vertIndex - TileVertexResolution + 1;
        }
      }
      else if (x == 0 && (collapseEdgeMask & left))
      {
        if ((y & 1) == 0)
        {
          pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution + TileVertexResolution;
          //pIndicies[index * 6 + 1] = vertIndex + 1;
          //pIndicies[index * 6 + 2] = vertIndex;

          pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
        else
        {
          // collapse
          //pIndicies[index * 6 + 0] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 1] = vertIndex + TileVertexResolution;
          pIndicies[index * 6 + 2] = vertIndex + TileVertexResolution;

          //pIndicies[index * 6 + 3] = vertIndex + TileVertexResolution;
          //pIndicies[index * 6 + 4] = vertIndex + TileVertexResolution + 1;
          //pIndicies[index * 6 + 5] = vertIndex + 1;
        }
      }
    }
  }

  float normalizeVertexPositionScale = float(TileVertexResolution) / (TileVertexResolution - 1); // ensure verts are [0, 1]
  for (int y = 0; y < TileVertexResolution; ++y)
  {
    for (int x = 0; x < TileVertexResolution; ++x)
    {
      uint32_t index = y * TileVertexResolution + x;
      //pVerts[index].index = (float)index;
      //pVerts[index].index = 0;

      //if (x == 0 && (y == 0 || y == ((TileVertexResolution - 1) / 2) || y == TileVertexResolution - 1))
      //  pVerts[index].index = (float)ii++;
      //else if (x == ((TileVertexResolution - 1) / 2) && (y == 0 || y == ((TileVertexResolution - 1) / 2) || y == TileVertexResolution - 1))
      //  pVerts[index].index = (float)ii++;
      //else if (x == TileVertexResolution - 1 && (y == 0 || y == ((TileVertexResolution - 1) / 2) || y == TileVertexResolution - 1))
      //  pVerts[index].index = (float)ii++;

      float normX = ((float)(x) / TileVertexResolution) * normalizeVertexPositionScale;
      float normY = ((float)(y) / TileVertexResolution) * normalizeVertexPositionScale;
      pVerts[index].uv.x = minUV.x + normX * (maxUV.x - minUV.x);
      pVerts[index].uv.y = minUV.y + normY * (maxUV.y - minUV.y);

    }
  }
}

udResult vcTileRenderer_Create(vcTileRenderer **ppTileRenderer, vcSettings *pSettings)
{
  udResult result;
  vcTileRenderer *pTileRenderer = nullptr;
  vcTileVertex verts[TileVertexResolution * TileVertexResolution];
  int indicies[TileIndexResolution * TileIndexResolution * 6];
  uint32_t greyPixel = 0xf3f3f3ff;
  UD_ERROR_NULL(ppTileRenderer, udR_InvalidParameter_);

  pTileRenderer = udAllocType(vcTileRenderer, 1, udAF_Zero);
  UD_ERROR_NULL(pTileRenderer, udR_MemoryAllocationFailure);

  vcQuadTree_Create(&pTileRenderer->quadTree, pSettings);

  pTileRenderer->pSettings = pSettings;

  pTileRenderer->cache.pSemaphore = udCreateSemaphore();
  pTileRenderer->cache.pMutex = udCreateMutex();
  pTileRenderer->cache.keepLoading = true;
  pTileRenderer->cache.tileLoadList.Init(128);

  for (size_t i = 0; i < udLengthOf(pTileRenderer->cache.pThreads); ++i)
    udThread_Create(&pTileRenderer->cache.pThreads[i], (udThreadStart*)vcTileRenderer_LoadThread, pTileRenderer);

 vcShader_CreateFromText(&pTileRenderer->presentShader.pProgram, g_tileVertexShader, g_tileFragmentShader, vcSimpleVertexLayout, (int)udLengthOf(vcSimpleVertexLayout));
 vcShader_GetConstantBuffer(&pTileRenderer->presentShader.pConstantBuffer, pTileRenderer->presentShader.pProgram, "u_EveryObject", sizeof(pTileRenderer->presentShader.everyObject));
 vcShader_GetSamplerIndex(&pTileRenderer->presentShader.uniform_texture, pTileRenderer->presentShader.pProgram, "u_texture");
 vcShader_GetSamplerIndex(&pTileRenderer->presentShader.uniform_dem0, pTileRenderer->presentShader.pProgram, "u_dem0");
 vcShader_GetSamplerIndex(&pTileRenderer->presentShader.uniform_dem1, pTileRenderer->presentShader.pProgram, "u_dem1");

 // build meshes
 for (int i = 0; i < 16; ++i)
 {
   vcTileRenderer_BuildMeshVertices(verts, indicies, udFloat2::create(0.0f, 0.0f), udFloat2::create(1.0f, 1.0f), meshConfigurations[i]);
   vcMesh_Create(&pTileRenderer->pTileMeshes[i], vcTileVertexLayout, (int)udLengthOf(vcTileVertexLayout), verts, TileVertexResolution * TileVertexResolution, indicies, TileIndexResolution * TileIndexResolution * 6);
 }
 vcTexture_Create(&pTileRenderer->pEmptyTileTexture, 1, 1, &greyPixel);

 const char *tiles[] = { "E:\\git\\vaultclient\\builds\\assets\\S28E152.hgt", "E:\\git\\vaultclient\\builds\\assets\\S28E153.hgt" };

 for (int i = 0; i < 2; ++i)
  {
    void *pFileData;
    int64_t fileLen;
    udFile_Load(tiles[i], &pFileData, &fileLen);
    int outputSize = 3601;
    int inputSize = 3601;
    uint16_t *pRealignedPixels = udAllocType(uint16_t, outputSize * outputSize, udAF_Zero);
    uint16_t lastValidHeight = 0;
    for (int y = 0; y < outputSize; ++y)
    {
      for (int x = 0; x < outputSize; ++x)
      {
        uint16_t p = ((uint16_t*)pFileData)[y * inputSize + x];
        p = ((p & 0xff00) >> 8) | ((p & 0x00ff) << 8);
        if (p > 40000)
          p = lastValidHeight;
        lastValidHeight = p;
        pRealignedPixels[y * outputSize + x] = p;
      }
    }
    vcTexture_Create(&pDEMTexture[i], outputSize, outputSize, pRealignedPixels, vcTextureFormat_R16, vcTFM_Linear, false, vcTWM_Clamp);//, vcTCF_None, 16);
  }

  *ppTileRenderer = pTileRenderer;
  pTileRenderer = nullptr;
  result = udR_Success;
epilogue:
  if (pTileRenderer)
    vcTileRenderer_Destroy(&pTileRenderer);

  return result;
}

udResult vcTileRenderer_Destroy(vcTileRenderer **ppTileRenderer)
{
  if (ppTileRenderer == nullptr || *ppTileRenderer == nullptr)
    return udR_InvalidParameter_;

  vcTileRenderer *pTileRenderer = *ppTileRenderer;

  pTileRenderer->cache.keepLoading = false;

  for (size_t i = 0; i < udLengthOf(pTileRenderer->cache.pThreads); ++i)
    udIncrementSemaphore(pTileRenderer->cache.pSemaphore);

  for (size_t i = 0; i < udLengthOf(pTileRenderer->cache.pThreads); ++i)
  {
    udThread_Join(pTileRenderer->cache.pThreads[i]);
    udThread_Destroy(&pTileRenderer->cache.pThreads[i]);
  }

  udDestroyMutex(&pTileRenderer->cache.pMutex);
  udDestroySemaphore(&pTileRenderer->cache.pSemaphore);

  pTileRenderer->cache.tileLoadList.Deinit();

  vcShader_ReleaseConstantBuffer(pTileRenderer->presentShader.pProgram, pTileRenderer->presentShader.pConstantBuffer);
  vcShader_DestroyShader(&(pTileRenderer->presentShader.pProgram));
  for (int i = 0; i < 16; ++i)
    vcMesh_Destroy(&pTileRenderer->pTileMeshes[i]);
  vcTexture_Destroy(&pTileRenderer->pEmptyTileTexture);

  vcQuadTree_Destroy(&(*ppTileRenderer)->quadTree);
  udFree(*ppTileRenderer);
  *ppTileRenderer = nullptr;

  vcTexture_Destroy(&pDEMTexture[0]);
  vcTexture_Destroy(&pDEMTexture[1]);
  return udR_Success;
}

void vcTileRenderer_UpdateTileTexture(vcTileRenderer *pTileRenderer, vcQuadTreeNode *pNode)
{
  (pTileRenderer);
  (pNode);

  vcTileRenderer::vcTileCache *pTileCache = &pTileRenderer->cache;
  if (pNode->renderInfo.loadStatus == vcNodeRenderInfo::vcTLS_None)
  {
    pNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_InQueue;

    pNode->renderInfo.pData = nullptr;
    pNode->renderInfo.pTexture = nullptr;
    pNode->renderInfo.loadAttempts = 0;

    udDouble2 min = udDouble2::create(pNode->worldBounds[0].x, pNode->worldBounds[2].y);
    udDouble2 max = udDouble2::create(pNode->worldBounds[3].x, pNode->worldBounds[1].y);
    pNode->renderInfo.center = (max + min) * 0.5;

    pTileCache->tileLoadList.PushBack(pNode);
    udIncrementSemaphore(pTileCache->pSemaphore);
  }

  pNode->renderInfo.tryLoad = true;

  if (pNode->renderInfo.loadStatus == vcNodeRenderInfo::vcTLS_Downloaded)
  {
    pNode->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_Loaded;
    pNode->renderInfo.tryLoad = false;

    vcTexture_Create(&pNode->renderInfo.pTexture, pNode->renderInfo.width, pNode->renderInfo.height, pNode->renderInfo.pData, vcTextureFormat_RGBA8, vcTFM_Linear, true, vcTWM_Clamp, vcTCF_None, 16);
    udFree(pNode->renderInfo.pData);
  }

}

void vcTileRenderer_UpdateTextureQueuesRecursive(vcTileRenderer *pTileRenderer, vcQuadTreeNode *pNode)
{
  if (!vcQuadTree_IsLeafNode(pNode))
  {
    for (int c = 0; c < 4; ++c)
      vcTileRenderer_UpdateTextureQueuesRecursive(pTileRenderer, &pTileRenderer->quadTree.nodes.pPool[pNode->childBlockIndex + c]);
  }
  else
  {
    bool updateTile = (pNode->renderInfo.loadStatus != vcNodeRenderInfo::vcTLS_Loaded);
    if ((pTileRenderer->pSettings->maptiles.mapOptions & vcTRF_OnlyRequestVisibleTiles) != 0)
      updateTile = updateTile && pNode->visible;

    if (updateTile)
      vcTileRenderer_UpdateTileTexture(pTileRenderer, pNode);
  }
}

void vcTileRenderer_UpdateTextureQueues(vcTileRenderer *pTileRenderer)
{
  // invalidate current queue
  for (size_t i = 0; i < pTileRenderer->cache.tileLoadList.length; ++i)
    pTileRenderer->cache.tileLoadList[i]->renderInfo.tryLoad = false;

  // update visible tiles textures
  //vcTileRenderer_UpdateTextureQueuesRecursive(pTileRenderer, &pTileRenderer->quadTree.nodes.pPool[pTileRenderer->quadTree.rootIndex]);

  // always request root
  //vcTileRenderer_UpdateTileTexture(pTileRenderer, &pTileRenderer->quadTree.nodes.pPool[pTileRenderer->quadTree.rootIndex]);

  // remove from the queue any tiles that now invalid
  for (int i = 0; i < (int)pTileRenderer->cache.tileLoadList.length; ++i)
  {
    if (!pTileRenderer->cache.tileLoadList[i]->renderInfo.tryLoad)
    {
      pTileRenderer->cache.tileLoadList[i]->renderInfo.loadStatus = vcNodeRenderInfo::vcTLS_None;
      pTileRenderer->cache.tileLoadList.RemoveSwapLast(i);
      --i;
    }
  }

  // TODO: For each tile in cache, LRU destroy
}

void vcTileRenderer_Update(vcTileRenderer *pTileRenderer, vcGISSpace *pSpace, const udDouble3 worldCorners[4], const udInt3 &slippyCoords, const udDouble3 &cameraWorldPos, const udDouble4x4 &viewProjectionMatrix)
{
  pTileRenderer->cameraPosition = cameraWorldPos;

  double slippyCornersViewSize = udMag3(worldCorners[1] - worldCorners[2]) * 0.5;
  vcQuadTreeViewInfo viewInfo =
  {
    pSpace,
    slippyCoords,
    cameraWorldPos,
    slippyCornersViewSize,
    (double)pTileRenderer->pSettings->camera.farPlane,
    pTileRenderer->pSettings->maptiles.mapHeight,
    viewProjectionMatrix
  };

  //uint64_t startTime = udPerfCounterStart();

  vcQuadTree_Update(&pTileRenderer->quadTree, viewInfo);
  vcQuadTree_Prune(&pTileRenderer->quadTree);

  udLockMutex(pTileRenderer->cache.pMutex);
  vcTileRenderer_UpdateTextureQueues(pTileRenderer);
  udReleaseMutex(pTileRenderer->cache.pMutex);

  //pTileRenderer->quadTree.metaData.generateTimeMs = udPerfCounterMilliseconds(startTime);
  printf("%f\n", pTileRenderer->quadTree.metaData.generateTimeMs);
}

bool vcTileRenderer_DrawNode(vcTileRenderer *pTileRenderer, vcQuadTreeNode *pNode, vcMesh *pMesh, const udDouble4x4 &view)
{
  vcTexture *pTexture = pNode->renderInfo.pTexture;
  pNode->rendered = (pTexture != nullptr);
  if (!pNode->rendered)
  {
#if !VISUALIZE_DEBUG_TILES
    //if (pNode != &pTileRenderer->quadTree.nodes.pPool[pTileRenderer->quadTree.rootIndex])
    //  return false;
#endif
    // only root node should touch this
    pTexture = pTileRenderer->pEmptyTileTexture;
  }

  int slippyLayerDescendAmount = 1;//udMin((MAX_SLIPPY_LEVEL - slippyTileCoord.z), gSlippyLayerDescendAmount[3]);

  udDouble3 tileBounds[9];
  for (int t = 0; t < 9; ++t)
  {
    udInt2 slippySampleCoord = udInt2::create((pNode->slippyPosition.x * (1 << slippyLayerDescendAmount)) + (t % 3),
      (pNode->slippyPosition.y * (1 << slippyLayerDescendAmount)) + (t / 3));
    vcGIS_SlippyToLocal(pTileRenderer->quadTree.pSpace, &tileBounds[t], slippySampleCoord, pNode->slippyPosition.z + slippyLayerDescendAmount);
    //tileBounds[t] = udDouble2::create(localCorners[t].x, localCorners[t].y);
  }

  for (int t = 0; t < 9; ++t)//TileVertexResolution * TileVertexResolution; ++t)
  {
    udFloat4 eyeSpaceVertexPosition = udFloat4::create(view * udDouble4::create(tileBounds[t].toVector2(), 0.0, 1.0));
    pTileRenderer->presentShader.everyObject.eyePositions[t] = eyeSpaceVertexPosition;
  }

#if VISUALIZE_DEBUG_TILES
  pTileRenderer->presentShader.everyObject.colour = udFloat4::one();
  if (!pNode->renderInfo.pTexture)
  {
    pTileRenderer->presentShader.everyObject.colour = udFloat4::create(pNode->level / 21.0f, 0, 0, 1);
    if (!pNode->visible)
      pTileRenderer->presentShader.everyObject.colour.z = pNode->level / 21.0f;
  }
#endif

  //S28E153
  udDouble3 r0 = udGeoZone_ToCartesian(pTileRenderer->quadTree.pSpace->zone, udDouble3::create(-28.0, 152, 0));
  udDouble3 r1 = udGeoZone_ToCartesian(pTileRenderer->quadTree.pSpace->zone, udDouble3::create(-27.0, 153, 0));

  //S28E152
  udDouble3 r2 = udGeoZone_ToCartesian(pTileRenderer->quadTree.pSpace->zone, udDouble3::create(-28.0, 153.0, 0));
  udDouble3 r3 = udGeoZone_ToCartesian(pTileRenderer->quadTree.pSpace->zone, udDouble3::create(-27.0, 154.0, 0));
  // left brisbane (S28E152.hgt), right brisbane (S28E153.hgt)
  //udDouble2 mins[] = { udDouble2::create(400781.82958118513, 6902797.6293904129), udDouble2::create(500000.00000000000, 6902394.7726541311) };
  //udDouble2 maxs[] = { udDouble2::create(500000.00000000000, 7013171.6474111192), udDouble2::create(598325.33504640602, 7013564.7575185951) };


  //{x = 400781.82958145085 y = 12986828.352577262 z = 0.00000000000000000 }

  // left brisbane (S28E152.hgt), right brisbane (S28E153.hgt)
  //udDouble2 mins[] = { udDouble2::create(400781.82958118513, 6902797.6293904129), udDouble2::create(500000.00000000000, 6902394.7726541311) };
  //udDouble2 maxs[] = { udDouble2::create(500000.00000000000, 7013171.6474111192), udDouble2::create(598325.33504640602, 7013564.7575185951) };
  //udDouble2 mins[] = { udDouble2::create(401674.66495384468, 6902394.7726660399), udDouble2::create(500000.00000000000, 6902797.6294023134) };
  //udDouble2 maxs[] = { udDouble2::create(500000.00000000000, 6902797.6294023134), udDouble2::create(598325.33504615526, 6902394.7726660399) };
  udDouble2 mins[] = { udDouble2::create(r0.x, r0.y), udDouble2::create(r2.x, r2.y) };
  udDouble2 maxs[] = { udDouble2::create(r1.x, r1.y), udDouble2::create(r3.x, r3.y) };

  //mins[0].x += 100.0; // manual correction (because its busted)
  //maxs[0].y += 365.0; // manual correction (because its busted)
  udDouble2 ranges[] = { maxs[0] - mins[0], maxs[1] - mins[1] };


  bool in0Bounds = !(tileBounds[5].x < mins[0].x || tileBounds[0].x > maxs[0].x || tileBounds[2].y < mins[0].y || tileBounds[6].y > maxs[0].y);
  bool in1Bounds = !(tileBounds[5].x < mins[1].x || tileBounds[0].x > maxs[1].x || tileBounds[2].y < mins[1].y || tileBounds[6].y > maxs[1].y);
  if (!in0Bounds && !in1Bounds)
    return true;

  for (int d = 0; d < 2; ++d)
  {
    for (int t = 0; t < 9; ++t)
    {
      double u2 = (tileBounds[t].x - mins[d].x) / ranges[d].x;
      double v2 = (tileBounds[t].y - mins[d].y) / ranges[d].y;

      pTileRenderer->presentShader.everyObject.demUVs[d * 9 + t].x = float(u2);
      pTileRenderer->presentShader.everyObject.demUVs[d * 9 + t].y = float(1.0 - v2);
    }
  }

  vcShader_BindTexture(pTileRenderer->presentShader.pProgram, pTexture, 0, pTileRenderer->presentShader.uniform_texture);
  vcShader_BindTexture(pTileRenderer->presentShader.pProgram, pDEMTexture[0], 1, pTileRenderer->presentShader.uniform_dem0);
  vcShader_BindTexture(pTileRenderer->presentShader.pProgram, pDEMTexture[1], 2, pTileRenderer->presentShader.uniform_dem1);

  vcShader_BindConstantBuffer(pTileRenderer->presentShader.pProgram, pTileRenderer->presentShader.pConstantBuffer, &pTileRenderer->presentShader.everyObject, sizeof(pTileRenderer->presentShader.everyObject));
  vcMesh_Render(pMesh, TileIndexResolution * TileIndexResolution * 2); // 2 tris per quad
  return true;
}

void vcTileRenderer_RecursiveSetRendered(vcTileRenderer *pTileRenderer, vcQuadTreeNode *pNode, bool rendered)
{
  pNode->rendered = pNode->rendered || rendered;
  if (!vcQuadTree_IsLeafNode(pNode))
  {
    for (int c = 0; c < 4; ++c)
      vcTileRenderer_RecursiveSetRendered(pTileRenderer, &pTileRenderer->quadTree.nodes.pPool[pNode->childBlockIndex + c], pNode->rendered);
  }
}

// 'true' indicates the node was able to render itself (or it didn't want to render itself).
// 'false' indicates that the nodes parent needs to be rendered.
bool vcTileRenderer_RecursiveRender(vcTileRenderer *pTileRenderer, vcQuadTreeNode *pNode, const udDouble4x4 &view)
{
  // Nodes can still have 'invalid' children that have not yet been pruned
  if (!pNode->touched)
    return false;

  bool childrenNeedThisTileRendered = vcQuadTree_IsLeafNode(pNode);
  if (!childrenNeedThisTileRendered)
  {
    for (int c = 0; c < 4; ++c)
      childrenNeedThisTileRendered = !vcTileRenderer_RecursiveRender(pTileRenderer, &pTileRenderer->quadTree.nodes.pPool[pNode->childBlockIndex + c], view) || childrenNeedThisTileRendered;
  }

  bool shouldRender = childrenNeedThisTileRendered && pNode->visible;
#if VISUALIZE_DEBUG_TILES
  shouldRender = childrenNeedThisTileRendered;
#endif

  shouldRender = vcQuadTree_IsLeafNode(pNode);
  if (shouldRender)
  {
    int meshIndex = 0;
    for (int i = 0; i < 16; ++i)
    {
      if (meshConfigurations[i] == pNode->neighbours)
      {
        meshIndex = i;
        break;
      }
    }
    return vcTileRenderer_DrawNode(pTileRenderer, pNode, pTileRenderer->pTileMeshes[meshIndex], view);
  }

  // This child doesn't need parent to draw itself
  return true;
}

#include "gl\opengl\vcOpenGL.h"

void vcTileRenderer_Render(vcTileRenderer *pTileRenderer, const udDouble4x4 &view, const udDouble4x4 &proj)
{
  udDouble4x4 viewWithMapTranslation = view * udDouble4x4::translation(0, 0, pTileRenderer->pSettings->maptiles.mapHeight);

  vcGLStencilSettings stencil = {};
  stencil.writeMask = 0xFF;
  stencil.compareFunc = vcGLSSF_Equal;
  stencil.compareValue = 0;
  stencil.compareMask = 0xFF;
  stencil.onStencilFail = vcGLSSOP_Keep;
  stencil.onDepthFail = vcGLSSOP_Keep;
  stencil.onStencilAndDepthPass = vcGLSSOP_Increment;

  vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_Back);
  vcGLState_SetBlendMode(vcGLSBM_AdditiveSrcInterpolativeDst);
  vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, true, nullptr);//&stencil);

  if (pTileRenderer->pSettings->maptiles.blendMode == vcMTBM_Overlay)
    vcGLState_SetDepthStencilMode(vcGLSDM_Always, false, &stencil);
  else if (pTileRenderer->pSettings->maptiles.blendMode == vcMTBM_Underlay)
    vcGLState_SetViewportDepthRange(1.0f, 1.0f);

  for (int i = 0; i < 2; ++i)
  {
    vcShader_Bind(pTileRenderer->presentShader.pProgram);
    pTileRenderer->presentShader.everyObject.projectionMatrix = udFloat4x4::create(proj);
    pTileRenderer->presentShader.everyObject.viewMatrix = udFloat4x4::create(view);
    pTileRenderer->presentShader.everyObject.colour = udFloat4::create(0.f, 0.f, 0.f, 0.0f);//pTileRenderer->pSettings->maptiles.transparency);

    if (i == 1)
    {
      glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      glPolygonOffset(1.0f, -0.1f);
      pTileRenderer->presentShader.everyObject.colour = udFloat4::create(1.f, 1.f, 1.f, 1.f);//pTileRenderer->pSettings->maptiles.transparency);
      vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, true, nullptr);//&stencil);
    }

    vcQuadTreeNode *pRoot = &pTileRenderer->quadTree.nodes.pPool[pTileRenderer->quadTree.rootIndex];
    vcTileRenderer_RecursiveRender(pTileRenderer, pRoot, viewWithMapTranslation);
    vcTileRenderer_RecursiveSetRendered(pTileRenderer, pRoot, pRoot->rendered);

    // Render the root tile again (if it hasn't already been rendered normally) to cover up gaps between tiles
    //if (!pRoot->rendered && pRoot->renderInfo.pTexture)
    //  vcTileRenderer_DrawNode(pTileRenderer, pRoot, pTileRenderer->pTileMeshes[0], viewWithMapTranslation);

    if (pTileRenderer->pSettings->maptiles.blendMode == vcMTBM_Underlay)
      vcGLState_SetViewportDepthRange(0.0f, 1.0f);
    vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, true, nullptr);
    vcGLState_SetBlendMode(vcGLSBM_None);
    vcShader_Bind(nullptr);
  }

  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glPolygonOffset(1.0f, 0.0f);
}

void vcTileRenderer_ClearTiles(vcTileRenderer *pTileRenderer)
{
  udLockMutex(pTileRenderer->cache.pMutex);

  vcQuadTree_Reset(&pTileRenderer->quadTree);
  pTileRenderer->cache.tileLoadList.Clear();

  udReleaseMutex(pTileRenderer->cache.pMutex);
}
