#include "vcKey.h"

#include "vcStrings.h"
#include "vcStringFormat.h"

#include "udFile.h"
#include "udJSON.h"
#include "udStringUtil.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>
#include <unordered_map>
#include <SDL2/SDL.h>

namespace vcKey
{
  static const char *pModText[] =
  {
    "Shift",
    "Ctrl",
    "Alt",
    "Super"
  };

  static const modifierFlags modList[] =
  {
    vcMOD_Shift,
    vcMOD_Ctrl,
    vcMOD_Alt,
    vcMOD_Super
  };

  static std::unordered_map<std::string, int> *g_pKeyMap;

  bool Pressed(const char *pKey)
  {
    ImGuiIO io = ImGui::GetIO();

    int key = (*g_pKeyMap)[pKey];

    if ((key & vcMOD_Shift) && !io.KeyShift)
      return false;
    if ((key & vcMOD_Ctrl) && !io.KeyCtrl)
      return false;
    if ((key & vcMOD_Alt) && !io.KeyAlt)
      return false;
    if ((key & vcMOD_Super) && !io.KeySuper)
      return false;

    return ImGui::IsKeyPressed((key & 0x1FF), false);
  }

  void GetKeyName(int key, char *pBuffer, uint32_t bufferLen)
  {
    if (key == 0)
    {
      udStrcpy(pBuffer, (size_t)bufferLen, vcString::Get("bindingsUnset"));
      return;
    }

    const char *pStrings[5] = {};

    if ((key & vcMOD_Shift) == vcMOD_Shift)
      pStrings[0] = "Shift + ";
    else
      pStrings[0] = "";

    if ((key & vcMOD_Ctrl) == vcMOD_Ctrl)
      pStrings[1] = "Ctrl + ";
    else
      pStrings[1] = "";

    if ((key & vcMOD_Alt) == vcMOD_Alt)
      pStrings[2] = "Alt + ";
    else
      pStrings[2] = "";

    if ((key & vcMOD_Super) == vcMOD_Super)
    {
#ifdef UDPLATFORM_WINDOWS
      pStrings[3] = "Win + ";
#elif UDPLATFORM_OSX
      pStrings[3] = "Cmd + ";
#else
      pStrings[3] = "Super + ";
#endif
    }
    else
    {
      pStrings[3] = "";
    }

    pStrings[4] = SDL_GetScancodeName((SDL_Scancode)(key & 0x1FF));
   
    vcStringFormat(pBuffer, bufferLen, "{0}{1}{2}{3}{4}", pStrings, 5);
  }

  int GetMod(int key)
  {
    // Only modifier keys in input
    if (key >= SDL_SCANCODE_LCTRL && key <= SDL_SCANCODE_RGUI)
      return 0;

    ImGuiIO io = ImGui::GetIO();
    int out = key;

    if (io.KeyShift)
      out |= vcMOD_Shift;
    if (io.KeyCtrl)
      out |= vcMOD_Ctrl;
    if (io.KeyAlt)
      out |= vcMOD_Alt;
    if (io.KeySuper)
      out |= vcMOD_Super;

    return out;
  }

  void Set(const char *pKey, int value)
  {
    (*g_pKeyMap)[pKey] = value;
  }

  int Get(const char *pKey)
  {
    if (g_pKeyMap == nullptr)
      g_pKeyMap = new std::unordered_map<std::string, int>();

    return (*g_pKeyMap)[pKey];
  }

  void DisplayBindings(vcState *pProgramState)
  {
    static char target[50] = {}; // TODO: Document/reconsider limitation of 50 chars
    static const char *pError = "";

    if (target[0] != '\0' && pProgramState->currentKey)
    {
      for (std::pair<std::string, int> kvp : (*g_pKeyMap))
      {
        if (kvp.second == pProgramState->currentKey)
        {
          pError = vcString::Get("bindingsErrorUnbound");
          Set(kvp.first.c_str(), 0);
          break;
        }
      }

      Set(target, pProgramState->currentKey);
      target[0] = '\0';
    }

    if (*pError != '\0')
      ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", pError);

    ImGui::Columns(3);
    ImGui::SetColumnWidth(0, 125);
    ImGui::SetColumnWidth(1, 125);
    ImGui::SetColumnWidth(2, 650);

    // Header Row
    ImGui::SetWindowFontScale(1.05f);
    ImGui::TextUnformatted(vcString::Get("bindingsColumnName"));
    ImGui::NextColumn();
    ImGui::TextUnformatted(vcString::Get("bindingsColumnKeyCombination"));
    ImGui::NextColumn();
    ImGui::TextUnformatted(vcString::Get("bindingsColumnDescription"));
    ImGui::NextColumn();
    ImGui::SetWindowFontScale(1.f);

    for (std::pair<std::string, int> kvp : (*g_pKeyMap))
    {
      if (ImGui::Button(kvp.first.c_str(), ImVec2(-1, 0)))
      {
        pError = "";
        if (udStrEquali(target, kvp.first.c_str()))
        {
          Set(kvp.first.c_str(), 0);
          target[0] = '\0';
        }
        else
        {
          pProgramState->currentKey = 0;
          udStrcpy(target, kvp.first.c_str());
        }
      }

      ImGui::NextColumn();

      char key[50];
      GetKeyName(kvp.second, key, (uint32_t)udLengthOf(key));
      ImGui::TextUnformatted(key);

      ImGui::NextColumn();
      ImGui::TextUnformatted(vcString::Get(udTempStr("bindings%s", kvp.first.c_str())));

      ImGui::NextColumn();
    }

    ImGui::EndColumns();
  }

  int DecodeKeyString(const char *pBind)
  {
    if (pBind == nullptr || *pBind == '\0')
      return 0;

    int len = (int)udStrlen(pBind);
    size_t index = 0;
    int value = 0;

    if (udStrrchr(pBind, "+", (size_t *)&index) != nullptr)
    {
      size_t modIndex = 0;

      for (int i = 0; i < (int)udLengthOf(modList); ++i)
      {
        udStrstr(pBind, len, pModText[i], &modIndex);
        if ((int)modIndex != len)
          value |= modList[i];
      }

      pBind += index + 2;
    }

    value += SDL_GetScancodeFromName(pBind);

    return value;
  }

  udResult LoadTableFromMemory(const char *pJSON)
  {
    udResult result;
    udJSON bindings;
    size_t count = 0;

    FreeTable(); // Empty the table
    g_pKeyMap = new std::unordered_map<std::string, int>();

    UD_ERROR_CHECK(bindings.Parse(pJSON));

    count = bindings.MemberCount();

    for (size_t i = 0; i < count; ++i)
    {
      const char *pKey = bindings.GetMemberName(i);
      if (bindings.GetMember(i)->IsNumeric())
        Set(pKey, bindings.GetMember(i)->AsInt());
      else
        Set(pKey, DecodeKeyString(bindings.GetMember(i)->AsString()));
    }

    result = udR_Success;
epilogue:
    return result;
  }

  udResult LoadTableFromFile(const char *pFilename)
  {
    udResult result;
    char *pPos = nullptr;

    UD_ERROR_NULL(pFilename, udR_InvalidParameter_);
    UD_ERROR_CHECK(udFile_Load(pFilename, (void **)& pPos));

    LoadTableFromMemory(pPos);

    result = udR_Success;
  epilogue:
    udFree(pPos);
    return result;
  }

  udResult SaveTableToFile(const char *pFilename)
  {
    udJSON output = {};
    const char *pOutput = nullptr;
    udFile *pFile = nullptr;
    udResult result;
    char buffer[50] = {};

    UD_ERROR_CHECK(udFile_Open(&pFile, pFilename, udFOF_Write));
    
    for (std::pair<std::string, int> kvp : (*g_pKeyMap))
    {
      GetKeyName(kvp.second, buffer, (uint32_t)udLengthOf(buffer));
      UD_ERROR_CHECK(output.Set("%s = '%s'", kvp.first.c_str(), buffer));
    }

    UD_ERROR_CHECK(output.Export(&pOutput, udJEO_JSON | udJEO_FormatWhiteSpace));
    UD_ERROR_CHECK(udFile_Write(pFile, pOutput, udStrlen(pOutput)));

    result = udR_Success;
epilogue:
    udFile_Close(&pFile);
    udFree(pOutput);
    return result;
  }

  void FreeTable()
  {
    if (g_pKeyMap == nullptr)
      return;

    g_pKeyMap->clear();
    delete g_pKeyMap;
    g_pKeyMap = nullptr;
  }
}
