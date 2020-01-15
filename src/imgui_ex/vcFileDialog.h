#ifndef vcFileDialog_h__
#define vcFileDialog_h__

#include <stddef.h>
#include "udMath.h"
#include "udCallback.h"

struct vcProgramState;
using vcFileDialogCallback = udCallback<void(void)>;

static const char *SupportedFileTypes_Images[] = { ".jpg", ".png", ".tga", ".bmp", ".gif" };
static const char *SupportedFileTypes_Projects[] = { ".json", ".udp" };

struct vcFileDialog
{
  bool showDialog;
  bool folderOnly;
  bool allowCreate;
  char *pPath;
  size_t pathLen;

  const char **ppExtensions; // Should be static arrays only
  size_t numExtensions;

  vcFileDialogCallback onSelect;
};

template <size_t N, size_t M> inline void vcFileDialog_Show(vcFileDialog *pDialog, char(&path)[N], const char *(&extensions)[M], bool loadOnly, vcFileDialogCallback callback)
{
  memset(pDialog, 0, sizeof(vcFileDialog));

  pDialog->showDialog = true;
  pDialog->folderOnly = false;
  pDialog->allowCreate = !loadOnly;

  pDialog->pPath = path;
  pDialog->pathLen = N;

  pDialog->ppExtensions = extensions;
  pDialog->numExtensions = M;

  pDialog->onSelect = callback;
}

bool vcFileDialog_DrawImGui(char *pPath, size_t pathLength, bool loadOnly = true, const char **ppExtensions = nullptr, size_t extensionCount = 0);

#endif //vcMenuButtons_h__
