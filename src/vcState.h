#ifndef vcState_h__
#define vcState_h__

#include "udPlatform/udPlatformUtil.h"
#include "udPlatform/udMath.h"
#include "udPlatform/udChunkedArray.h"
#include "udPlatform/udJSON.h"

#include "vCore/vWorkerThread.h"

#include "vcSettings.h"
#include "vcScene.h"
#include "vcGIS.h"
#include "vcFolder.h"

#include "imgui_ex/ImGuizmo.h"

#include <vector>

struct SDL_Window;

struct vdkContext;

struct vcFramebuffer;
struct vcRenderContext;
struct vcCamera;
struct vcTexture;
struct vcConvertContext;

struct vcSceneItemRef
{
  vcFolder *pParent;
  size_t index;
};

struct vcState
{
  bool programComplete;
  SDL_Window *pWindow;
  vcFramebuffer *pDefaultFramebuffer;

  int openModals; // This is controlled inside vcModals.cpp

  vcCamera *pCamera;

  std::vector<const char*> loadList;
  vWorkerThreadPool *pWorkerPool;

  double deltaTime;
  udUInt2 sceneResolution;

  vcGISSpace gis;
  char username[64];

  vcTexture *pCompanyLogo;
  vcTexture *pSceneWatermark;
  vcTexture *pUITexture;

  udDouble3 worldMousePos;
  bool pickingSuccess;
  udDouble3 previousWorldMousePos;
  bool previousPickingSuccess;

  vcCameraInput cameraInput;

  bool hasContext;
  bool forceLogout;
  int64_t lastServerAttempt;
  int64_t lastServerResponse;
  vdkContext *pVDKContext;
  vcRenderContext *pRenderContext;
  vcConvertContext *pConvertContext;

  char password[vcMaxPathLength];
  const char *pLoginErrorMessage;
  const char *pReleaseNotes; //Only loaded when requested
  bool passFocus;

  char modelPath[vcMaxPathLength];

  int renaming;
  char renameText[30];

  vcSettings settings;
  udJSON projects;
  udJSON packageInfo;

  struct
  {
    vcGizmoOperation operation;
    vcGizmoCoordinateSystem coordinateSystem;
  } gizmo;

  struct
  {
    vcTexture *pServerIcon;
    volatile void *pImageData;
    volatile int64_t loadStatus; // >0 is the size of pImageData
  } tileModal;

  struct
  {
    vcFolder *pItems;

    vcSceneItemRef insertItem;
    vcSceneItemRef clickedItem;
    std::vector<vcSceneItemRef> selectedItems;
  } sceneExplorer;

  bool firstRun;
  int64_t lastEventTime;
  bool showUI;
};

#endif // !vcState_h__
