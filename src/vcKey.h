#ifndef vcKey_h__
#define vcKey_h__

#include "udResult.h"
#include "vcState.h"

enum modifierShifts
{
  vcMOD_Shift = 1024,
  vcMOD_Ctrl = 2048,
  vcMOD_Alt = 4096,
  vcMOD_Super = 9192
};

namespace vcKey
{
  bool Pressed(int key);
  bool Pressed(const char *pKey);
  int GetMod(int key);
  void Set(const char *pKey, int value);
  int Get(const char *pKey);

  void DisplayBindings(vcState *pProgramState);

  udResult LoadTableFromMemory(const char *pJSON);
  udResult LoadTableFromFile(const char *pFilename);
  udResult SaveTableToFile(const char *pFilename);
  void FreeTable();
}

#endif //vcKey_h__
