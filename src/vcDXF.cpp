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
    if (typeCode < typeMap[i][2])
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

udResult vcDXF_Create(vcDXF **ppDXF)
{
  if (ppDXF == nullptr)
    return udR_InvalidParameter_;

  udResult result;

  vcDXF *pDXF = udAllocType(vcDXF, 1, udAF_Zero);
  UD_ERROR_NULL(pDXF, udR_MemoryAllocationFailure);

  *ppDXF = pDXF;
  pDXF = nullptr;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDXF_Destroy(vcDXF **ppDXF)
{
  udResult result;

  // TODO

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
  int skipChars = 0;
  size_t skipCharsT = 0;
  bool setSection = false;
  bool end = false;
  void *pCurrEndpoint = nullptr;

  UD_ERROR_CHECK(udFile_Load(pFilename, &pFile, &fileLen));
  pPos = pFile;

  pDXF = udAllocType(vcDXF, 1, udAF_Zero);
  UD_ERROR_NULL(pDXF, udR_MemoryAllocationFailure);

  pPos = udStrSkipWhiteSpace(pPos);

  do
  {
    // check type tag
    nextType = vcDXF_GetType(udStrAtoi(pPos, &skipChars));
    UD_ERROR_IF(nextType == vcDXFT_Unknown, udR_ParseError);
    pPos += skipChars;

    pPos = udStrSkipWhiteSpace(pPos);

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
      if (pCurrEndpoint == nullptr) 
      {
        // Could be a command type, or is data related to a command we don't handle
        switch (nextType)
        {
        case vcDXFT_BigString: // Falls through
        case vcDXFT_String:

          switch (currSection)
          {
          case vcDXFS_Unset:
            if (udStrBeginsWithi(pPos, "SECTION"))
              setSection = true;
            else if (udStrBeginsWithi(pPos, "EOF"))
              end = true;

            break;
          case vcDXFS_Header:


          }
        default:
          break;
        }
      }
      else // pCurrEndpoint is not nullptr
      {
        switch(nextType)
        {
        case vcDXFT_BigString: // Falls through
        case vcDXFT_String: // String type should be duped into endpoint
          udStrchr(pPos, " \n\r\t", &skipCharsT);
          *(char **)pCurrEndpoint = udStrndup(pPos, skipCharsT); // TODO: This work?
          skipChars = (int)skipCharsT;
          break;

        case vcDXFT_Double:
          double value = udStrAtof(pPos, &skipChars);
          memcpy(pCurrEndpoint, &value, sizeof(double));
          break;

        case vcDXFT_Int16:
          int16_t value = (int16_t)udStrAtoi(pPos, &skipChars);
          memcpy(pCurrEndpoint, &value, sizeof(int16_t));
          break;

        case vcDXFT_Int32:
          int32_t value = udStrAtoi(pPos, &skipChars);
          memcpy(pCurrEndpoint, &value, sizeof(int32_t));
          break;

        case vcDXFT_Int64:
          int64_t value = udStrAtoi64(pPos, &skipChars);
          memcpy(pCurrEndpoint, &value, sizeof(int64_t));
          break;

        case vcDXFT_Boolean:
          bool value = (bool)udStrAtoi(pPos, &skipChars);
          memcpy(pCurrEndpoint, &value, sizeof(bool));
          break;

        case vcDXFT_HexString: // TODO: Check this is how we would best store hex strings
          int32_t value = udStrAtoi(pPos, &skipChars, 16);
          memcpy(pCurrEndpoint, &value, sizeof(int32_t));
          break;
        }

        pPos += skipChars;
        pCurrEndpoint = nullptr;
      }

      if (end)
        break;
    }

    pPos = udStrSkipToEOL(pPos);
    pPos = udStrSkipWhiteSpace(pPos);
  } while ((pFile + fileLen) < pPos);

  *ppDXF = pDXF;
  pDXF = nullptr;

  result = udR_Success;

epilogue:

  if (result != udR_Success)
    vcDXF_Destroy(&pDXF);

  return result;
}

udResult vcDXF_Save(vcDXF *pDXF, const char *pFilename)
{
  // TODO
}
