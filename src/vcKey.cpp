#include "vcKey.h"

#include "vcStrings.h"

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
  static std::unordered_map<std::string, int> *g_pKeyMap;

  bool Pressed(int key)
  {
    ImGuiIO io = ImGui::GetIO();

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

  void PrintKeyName(int key)
  {
    if (key == 0)
    {
      ImGui::Text("UNSET");
      return;
    }

    if ((key & vcMOD_Shift) == vcMOD_Shift)
    {
      ImGui::Text("SHIFT+");
      ImGui::SameLine();
    }
    if ((key & vcMOD_Ctrl) == vcMOD_Ctrl)
    {
      ImGui::Text("CTRL+");
      ImGui::SameLine();
    }
    if ((key & vcMOD_Alt) == vcMOD_Alt)
    {
      ImGui::Text("ALT+");
      ImGui::SameLine();
    }

    if ((key & vcMOD_Super) == vcMOD_Super)
    {
#ifdef UDPLATFORM_WINDOWS
      ImGui::Text("WIN+");
#elif UDPLATFORM_OSX
      ImGui::Text("CMD+");
#else
      ImGui::Text("SUPER+");
#endif
      ImGui::SameLine();
    }

    ImGui::Text(SDL_GetScancodeName((SDL_Scancode)(key & 0x1FF)));
  }

  int GetMod(int key)
  {
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
    ImGui::BeginChild("bindingsInterfaceChild");

    PrintKeyName(pProgramState->currentKey);

    ImGui::Columns(4);
    ImGui::SetColumnWidth(0, 100);
    ImGui::SetColumnWidth(1, 100);
    ImGui::SetColumnWidth(2, 675);
    ImGui::SetColumnWidth(3, 75);

    for (std::pair<std::string, int> kvp : (*g_pKeyMap))
    {
      ImGui::Text(kvp.first.c_str());
      ImGui::NextColumn();
      PrintKeyName(kvp.second);
      ImGui::NextColumn();
      ImGui::Text(vcString::Get(udTempStr("bindings%s", kvp.first.c_str())));
      ImGui::NextColumn();

      if (ImGui::Button(udTempStr("%s###bindButton%d", vcString::Get("bindingsBind"), kvp.second)))
        Set(kvp.first.c_str(), pProgramState->currentKey);

      ImGui::NextColumn();
    }

    ImGui::EndColumns();
    ImGui::EndChild();
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
      int value = bindings.GetMember(i)->AsInt();

      Set(pKey, value);
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

    UD_ERROR_CHECK(udFile_Open(&pFile, pFilename, udFOF_Write));

    for (std::pair<std::string, int> kvp : (*g_pKeyMap))
      UD_ERROR_CHECK(output.Set("%s = %d", kvp.first.c_str(), kvp.second));

    UD_ERROR_CHECK(output.Export(&pOutput));
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
