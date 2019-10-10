#include "vcDXF.h"
#include "vcDXF_Internal.h"

#include "udFile.h"
#include "udPlatform.h"
#include "udStringUtil.h"
#include "udMath.h"

inline vcDXF_Type vcDXF_GetType(int typeCode)
{
  for (int i = 0; i < TYPEMAPLEN; ++i)
  {
    if (typeCode <= typeMap[i][2])
    {
      if (typeCode < typeMap[i][1])
        return vcDXFT_Unknown;

      return (vcDXF_Type)typeMap[i][0];
    }
  }

  return vcDXFT_Unknown;
}

inline vcDXF_Sections vcDXF_GetSection(const char *pSection)
{
  int i = 0;

  for (; i < udLengthOf(pSections); ++i)
  {
    if (udStrBeginsWithi(pSection, pSections[i]))
      break;
  }

  if (i < udLengthOf(pSections))
    pSection += sizeof(pSections[i]); // TODO: This right?

  return (vcDXF_Sections)i;
}

udResult vcDXF_DestroyEntity(vcDXF_Entity *pEntity)
{
  if (pEntity == nullptr)
    return udR_Success;

  udResult result;

  while (pEntity->children.length > 0)
  {
    vcDXF_Entity *pCurr = nullptr;
    UD_ERROR_CHECK(vcDXF_DestroyEntity(pEntity->children.GetElement(0)));
    pEntity->children.PopFront(pCurr);
    udFree(pCurr);
  }

  UD_ERROR_CHECK(pEntity->children.Deinit());

  udFree(pEntity->pName);
  udFree(pEntity->pHandle);

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDXF_Create(vcDXF **ppDXF)
{
  if (ppDXF == nullptr)
    return udR_InvalidParameter_;

  udResult result;

  vcDXF *pDXF = udAllocType(vcDXF, 1, udAF_Zero);
  UD_ERROR_NULL(pDXF, udR_MemoryAllocationFailure);

  pDXF->header.creationTime = 0; // TODO: Make now
  pDXF->header.linetypeScale = 1.0;
  pDXF->header.pointDisplaySize = 1.0; // TODO
  pDXF->header.polylineWidth = 1.0;
  pDXF->header.textSize = 1.0; // TODO: Check this
  UD_ERROR_CHECK(pDXF->entities.Init(32));

  *ppDXF = pDXF;
  pDXF = nullptr;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDXF_Destroy(vcDXF **ppDXF)
{
  if (ppDXF == nullptr || *ppDXF == nullptr)
    return udR_Success;

  udResult result;

  while ((*ppDXF)->entities.length > 0)
  {
    vcDXF_Entity *pEntity = nullptr;
    UD_ERROR_CHECK(vcDXF_DestroyEntity((*ppDXF)->entities.GetElement(0)));
    (*ppDXF)->entities.PopFront(pEntity);
    udFree(pEntity);
  }

  udFree((*ppDXF)->header.pNextHandle);
  udFree((*ppDXF)->header.pUserCoordSysName);
  udFree((*ppDXF)->pFileName);

  UD_ERROR_CHECK((*ppDXF)->entities.Deinit());

  result = udR_Success;

epilogue:

  return result;
}

vcDXF_Entity *vcDXF_GetNewParentEntity(vcDXF_Entity *pParent)
{
  if (pParent == nullptr)
    return nullptr;

  switch (pParent->type)
  {
  case vcDXFET_Polyline:
    return pParent->children.PushBack();
  case vcDXFET_Vertex: // Falls through
  case vcDXFET_None: 
    return nullptr;
  }

  // Shouldn't be hit
  return nullptr;
}

void *vcDXF_GetEndpoint(vcDXF_Entity *pCurrEntity, int groupCode)
{
  int i = 0;
  switch (pCurrEntity->type)
  {
  case vcDXFET_Polyline:
    for (; i < udLengthOf(vcDXF_PolylineCodes); ++i)
    {
      if (groupCode == vcDXF_PolylineCodes[i][0])
        break;
    }

    if (i == udLengthOf(vcDXF_PolylineCodes))
      return nullptr;

    return (uint8_t *)pCurrEntity + vcDXF_PolylineCodes[i][1];

  case vcDXFET_Vertex:
    for (; i < udLengthOf(vcDXF_VertexCodes); ++i)
    {
      if (groupCode == vcDXF_VertexCodes[i][0])
        break;
    }

    if (i == udLengthOf(vcDXF_VertexCodes))
      return nullptr;

    return (uint8_t *)pCurrEntity + vcDXF_VertexCodes[i][1];
  }
  
  return nullptr;
}

inline bool vcDXF_ValidChild(vcDXF_EntityType parent, vcDXF_EntityType child)
{
  return DXFValidChildren[parent] & child ? true : false;
}

udResult vcDXF_InitialiseEntity(vcDXF_Entity *pEntity)
{
  if (pEntity == nullptr)
    return udR_InvalidParameter_;

  udResult result;

  switch (pEntity->type)
  {
  case vcDXFET_None: // Falls through
  case vcDXFET_Vertex:
    break;

  case vcDXFET_Polyline:
    UD_ERROR_CHECK(pEntity->children.Init(32));
    break;
  }

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDXF_Load(vcDXF **ppDXF, const char *pFilename)
{
  if (pFilename == nullptr || ppDXF == nullptr)
    return udR_InvalidParameter_;

  udResult result;

  vcDXF_Sections currSection = vcDXFS_Unset;
  vcDXF *pDXF = nullptr;
  const char *pFile = nullptr;
  const char *pPos = nullptr;
  int64_t fileLen = 0;
  vcDXF_Type nextType = vcDXFT_Unknown;
  int32_t groupCode = 0;
  int skipChars = 0;
  size_t skipCharsT = 0;
  bool setSection = false;
  bool end = false;
  bool actioned = true;
  void *pCurrEndpoint = nullptr;
  char commandBuffer[30] = {};
  vcDXF_Entity *pParentEntity = nullptr;
  vcDXF_Entity *pCurrEntity = nullptr;

  UD_ERROR_CHECK(udFile_Load(pFilename, &pFile, &fileLen));
  pPos = pFile;

  pDXF = udAllocType(vcDXF, 1, udAF_Zero);
  UD_ERROR_NULL(pDXF, udR_MemoryAllocationFailure);

  pDXF->pFileName = udStrdup(pFilename);
  UD_ERROR_CHECK(pDXF->entities.Init(32));

  pPos = udStrSkipWhiteSpace(pPos);

  while ((pFile + fileLen) > pPos)
  {
    // Check type tag
    groupCode = udStrAtoi(pPos, &skipChars);
    nextType = vcDXF_GetType(groupCode);
    UD_ERROR_IF(nextType == vcDXFT_Unknown, udR_ParseError);
    pPos += skipChars;

    // Skip comments
    if (nextType == vcDXFT_Comment)
    {
      pPos = udStrSkipToEOL(pPos);
      pPos = udStrSkipToEOL(pPos);
      pPos = udStrSkipWhiteSpace(pPos);
      continue;
    }
    
    pPos = udStrSkipWhiteSpace(pPos);

    actioned = true;

    if (setSection)
    {
      currSection = vcDXF_GetSection(pPos);
      UD_ERROR_IF(currSection == vcDXFS_Unset, udR_ParseError);
      setSection = false;
    }
    else if (udStrBeginsWithi(pPos, "ENDSEC"))
    {
      currSection = vcDXFS_Unset;
    }
    else
    {
      int value = 0;
      int i = 0;

      // Could be a command type, or is data related to a command we don't handle
      if (nextType == vcDXFT_BigString || nextType == vcDXFT_String)
      {
        while (pPos[i] != ' ' && pPos[i] != '\n' && pPos[i] != '\r' && pPos[i] != '\t')
        {
          commandBuffer[i] = pPos[i];
          ++i;
        }
        commandBuffer[i] = '\0';

        switch (currSection)
        {
        case vcDXFS_Unset:
          if (udStrEquali(commandBuffer, "SECTION"))
            setSection = true;
          else if (udStrEquali(commandBuffer, "EOF"))
            end = true;
          else
            actioned = false;
          break;

        case vcDXFS_Header:
          value = headerCommandEndpoints[commandBuffer];
          if (value != 0) // TODO: Assuming missing entry will output 0 for new allocation
            pCurrEndpoint = ((uint8_t *)pDXF) + value;
          else
            actioned = false;
          // Otherwise we don't handle this command, continue and skip it
          break;

        case vcDXFS_Entities:
          if (udStrEquali(commandBuffer, "SEQEND"))
          {
            pCurrEndpoint = nullptr;
            pCurrEntity = nullptr;
            pParentEntity = nullptr;
          }
          else
          {
            int j = 0;
            for (; j < udLengthOf(pDXFEntities); ++j)
            {
              if (udStrEquali(commandBuffer, pDXFEntities[j]))
                break;
            }

            if (j == udLengthOf(pDXFEntities))
            {
              actioned = false;
              break;
            }

            if (pCurrEntity != nullptr && vcDXF_ValidChild(pCurrEntity->type, (vcDXF_EntityType)j))
              pParentEntity = pCurrEntity;

            if (pParentEntity != nullptr)
              pCurrEntity = (vcDXF_Entity *)vcDXF_GetNewParentEntity(pParentEntity);
            else
              pCurrEntity = pDXF->entities.PushBack();

            pCurrEntity->type = (vcDXF_EntityType)j;
            UD_ERROR_CHECK(vcDXF_InitialiseEntity(pCurrEntity));
          }
          break;

        default:
          break;
        }
      }
      else
      {
        actioned = false;
      }

      if (!actioned)
      {
        if (pCurrEntity != nullptr)
          pCurrEndpoint = vcDXF_GetEndpoint(pCurrEntity, groupCode);

        if (pCurrEndpoint != nullptr)
        {
          double valueD = 0;
          int16_t value16 = 0;
          int32_t value32 = 0;
          int64_t value64 = 0;
          bool valueb = false;

          switch (nextType)
          {
          case vcDXFT_BigString: // Falls through
          case vcDXFT_String: // String type should be duped into endpoint
            udStrchr(pPos, " \n\r\t", &skipCharsT);
            *(char **)pCurrEndpoint = udStrndup(pPos, skipCharsT);
            skipChars = (int)skipCharsT;
            break;

          case vcDXFT_Double:
            valueD = udStrAtof(pPos, &skipChars);
            memcpy(pCurrEndpoint, &valueD, sizeof(double));
            break;

          case vcDXFT_Int16:
            value16 = (int16_t)udStrAtoi(pPos, &skipChars);
            memcpy(pCurrEndpoint, &value16, sizeof(int16_t));
            break;

          case vcDXFT_Int32:
            value32 = udStrAtoi(pPos, &skipChars);
            memcpy(pCurrEndpoint, &value32, sizeof(int32_t));
            break;

          case vcDXFT_Int64:
            value64 = udStrAtoi64(pPos, &skipChars);
            memcpy(pCurrEndpoint, &value64, sizeof(int64_t));
            break;

          case vcDXFT_Boolean:
            valueb = (bool)udStrAtoi(pPos, &skipChars);
            memcpy(pCurrEndpoint, &valueb, sizeof(bool));
            break;

          case vcDXFT_HexString: // TODO: Check this is how we would best store hex strings
            value32 = udStrAtoi(pPos, &skipChars, 16);
            memcpy(pCurrEndpoint, &value32, sizeof(int32_t));
            break;
          }

          pPos += skipChars;
          pCurrEndpoint = nullptr;
        }
      }

      if (end)
        break;
    }

    pPos = udStrSkipToEOL(pPos);
    pPos = udStrSkipWhiteSpace(pPos);
  }

  *ppDXF = pDXF;
  pDXF = nullptr;

  result = udR_Success;

epilogue:

  udFree(pFile);

  if (result != udR_Success)
    vcDXF_Destroy(&pDXF);

  return result;
}

udResult vcDXF_AddToProject(vcDXF *pDXF, vdkProject *pProject, udGeoZone *pSourceZone /* = nullptr */)
{
  if (pDXF == nullptr || pProject == nullptr)
    return udR_InvalidParameter_;

  udResult result;

  vdkProjectNode *pRootNode = nullptr;
  vdkProjectNode *pFolder = nullptr;
  vdkProjectNode *pNode = nullptr;

  UD_ERROR_IF(vdkProject_GetProjectRoot(pProject, &pRootNode) != vE_Success, udR_Failure_);
  UD_ERROR_IF(vdkProjectNode_Create(pProject, &pFolder, pRootNode, "Folder", pDXF->pFileName, nullptr, nullptr) != vE_Success, udR_Failure_);

  for (int i = 0; i < pDXF->entities.length; ++i)
  {
    vcDXF_Entity *pEntity = &pDXF->entities[i];
    UD_ERROR_IF(vdkProjectNode_Create(pProject, &pNode, pFolder, "POI", pEntity->pName, nullptr, nullptr) != vE_Success, udR_Failure_);

    udDouble3 *pVerts = nullptr;

    switch (pEntity->type)
    {
    case vcDXFET_Polyline:
      pVerts = udAllocType(udDouble3, pEntity->children.length, udAF_Zero);
      for (int j = 0; j < pEntity->children.length; ++j)
      {
        if (pSourceZone != nullptr)
          pVerts[j] = udGeoZone_ToLatLong(*pSourceZone, pEntity->children[j].point, true);
        else
          pVerts[j] = pEntity->children[j].point;
      }
        
      UD_ERROR_IF(vdkProjectNode_SetGeometry(pProject, pNode, vdkPGT_LineString, (int)pEntity->children.length, (double*)pVerts) != vE_Success, udR_Failure_);
      udFree(pVerts);
      break;

    case vcDXFET_Vertex: // Can't exist alone?
      //UD_ERROR_IF(vdkProjectNode_SetGeometry(pProject, pNode, vdkPGT_Point, 1, ) != vE_Success, udR_Failure_);
      break;

    }
  }

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDXF_Save(vcDXF *pDXF, const char *pFilename)
{
  udUnused(pDXF);
  udUnused(pFilename);

  // TODO
  return udR_Success;
}
