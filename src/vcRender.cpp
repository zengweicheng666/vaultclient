#include "vcRender.h"

#include "gl/vcRenderShaders.h"
#include "gl/vcFramebuffer.h"
#include "gl/vcShader.h"
#include "gl/vcGLState.h"
#include "vcFenceRenderer.h"
#include "vcWaterRenderer.h"
#include "vcTileRenderer.h"
#include "vcCompass.h"

#include "vcInternalModels.h"
#include "vcGIS.h"

#include "stb_image.h"
#include <vector>

enum
{
  vcRender_SceneSizeIncrement = 32 // directX framebuffer can only be certain increments
};

struct vcUDRenderContext
{
  vdkRenderContext *pRenderer;
  vdkRenderView *pRenderView;
  uint32_t *pColorBuffer;
  float *pDepthBuffer;
  vcTexture *pColourTex;
  vcTexture *pDepthTex;

  struct
  {
    vcShader *pProgram;
    vcShaderSampler *uniform_texture;
    vcShaderSampler *uniform_depth;
    vcShaderConstantBuffer *uniform_params;

    struct
    {
      udFloat4 screenParams;  // sampleStepX, sampleStepSizeY, near plane, far plane
      udFloat4x4 inverseViewProjection;

      // outlining
      udFloat4 outlineColour;
      udFloat4 outlineParams;   // outlineWidth, threshold, (unused), (unused)

      // colour by height
      udFloat4 colourizeHeightColourMin;
      udFloat4 colourizeHeightColourMax;
      udFloat4 colourizeHeightParams; // min world height, max world height, (unused), (unused)

      // colour by depth
      udFloat4 colourizeDepthColour;
      udFloat4 colourizeDepthParams; // min depth, max depth, (unused), (unused)

      // contours
      udFloat4 contourColour;
      udFloat4 contourParams; // contour distance, contour band height, (unused), (unused)
    } params;

  } presentShader;
};

struct vcRenderContext
{
  vdkContext *pVaultContext;
  vcSettings *pSettings;
  vcCamera *pCamera;

  udUInt2 sceneResolution;
  udUInt2 originalSceneResolution;

  vcFramebuffer *pFramebuffer;
  vcTexture *pTexture;
  vcTexture *pDepthTexture;

  vcUDRenderContext udRenderContext;
  vcFenceRenderer *pDiagnosticFences;

  vcTileRenderer *pTileRenderer;
  vcAnchor *pCompass;

  struct
  {
    vcShader *pProgram;
    vcShaderSampler *uniform_texture;
    vcShaderConstantBuffer *uniform_MatrixBlock;
  } skyboxShader;

  vcMesh *pScreenQuadMesh;
  vcTexture *pSkyboxTexture;
};

udResult vcRender_RecreateUDView(vcRenderContext *pRenderContext);
udResult vcRender_RenderAndUploadUDToTexture(vcRenderContext *pRenderContext, vcRenderData &renderData);

udResult vcRender_Init(vcRenderContext **ppRenderContext, vcSettings *pSettings, vcCamera *pCamera, const udUInt2 &sceneResolution)
{
  udResult result = udR_Success;
  vcRenderContext *pRenderContext = nullptr;

  UD_ERROR_NULL(ppRenderContext, udR_InvalidParameter_);

  pRenderContext = udAllocType(vcRenderContext, 1, udAF_Zero);
  UD_ERROR_NULL(pRenderContext, udR_MemoryAllocationFailure);

  UD_ERROR_IF(!vcShader_CreateFromText(&pRenderContext->udRenderContext.presentShader.pProgram, g_udVertexShader, g_udFragmentShader, vcSimpleVertexLayout), udR_InternalError);
  UD_ERROR_IF(!vcShader_CreateFromText(&pRenderContext->skyboxShader.pProgram, g_vcSkyboxVertexShader, g_vcSkyboxFragmentShader, vcSimpleVertexLayout), udR_InternalError);

  vcMesh_Create(&pRenderContext->pScreenQuadMesh, vcSimpleVertexLayout, int(udLengthOf(vcSimpleVertexLayout)), screenQuadVertices, 4, screenQuadIndices, 6, vcMF_Dynamic);

  vcTexture_CreateFromFilename(&pRenderContext->pSkyboxTexture, "asset://assets/skyboxes/WaterClouds.jpg", nullptr, nullptr, vcTFM_Linear);
  UD_ERROR_CHECK(vcCompass_Create(&pRenderContext->pCompass));

  vcShader_Bind(pRenderContext->skyboxShader.pProgram);
  vcShader_GetSamplerIndex(&pRenderContext->skyboxShader.uniform_texture, pRenderContext->skyboxShader.pProgram, "u_texture");
  vcShader_GetConstantBuffer(&pRenderContext->skyboxShader.uniform_MatrixBlock, pRenderContext->skyboxShader.pProgram, "u_EveryFrame", sizeof(udFloat4x4));

  vcShader_Bind(pRenderContext->udRenderContext.presentShader.pProgram);
  vcShader_GetSamplerIndex(&pRenderContext->udRenderContext.presentShader.uniform_texture, pRenderContext->udRenderContext.presentShader.pProgram, "u_texture");
  vcShader_GetSamplerIndex(&pRenderContext->udRenderContext.presentShader.uniform_depth, pRenderContext->udRenderContext.presentShader.pProgram, "u_depth");
  vcShader_GetConstantBuffer(&pRenderContext->udRenderContext.presentShader.uniform_params, pRenderContext->udRenderContext.presentShader.pProgram, "u_params", sizeof(pRenderContext->udRenderContext.presentShader.params));

  vcPolygonModel_CreateShaders();
  vcImageRenderer_Init();

  vcShader_Bind(nullptr);

  UD_ERROR_CHECK(vcTileRenderer_Create(&pRenderContext->pTileRenderer, pSettings));
  UD_ERROR_CHECK(vcFenceRenderer_Create(&pRenderContext->pDiagnosticFences));

  *ppRenderContext = pRenderContext;

  pRenderContext->pSettings = pSettings;
  pRenderContext->pCamera = pCamera;
  result = vcRender_ResizeScene(pRenderContext, sceneResolution.x, sceneResolution.y);
  if (result != udR_Success)
    goto epilogue;

epilogue:
  return result;
}

udResult vcRender_Destroy(vcRenderContext **ppRenderContext)
{
  if (ppRenderContext == nullptr || *ppRenderContext == nullptr)
    return udR_Success;

  udResult result = udR_Success;
  vcRenderContext *pRenderContext = nullptr;

  UD_ERROR_NULL(ppRenderContext, udR_InvalidParameter_);

  pRenderContext = *ppRenderContext;
  *ppRenderContext = nullptr;

  if (pRenderContext->pVaultContext != nullptr)
  {
    if (pRenderContext->udRenderContext.pRenderView != nullptr && vdkRenderView_Destroy(pRenderContext->pVaultContext, &pRenderContext->udRenderContext.pRenderView) != vE_Success)
      UD_ERROR_SET(udR_InternalError);

    if (vdkRenderContext_Destroy(pRenderContext->pVaultContext, &pRenderContext->udRenderContext.pRenderer) != vE_Success)
      UD_ERROR_SET(udR_InternalError);
  }

  vcShader_DestroyShader(&pRenderContext->udRenderContext.presentShader.pProgram);
  vcShader_DestroyShader(&pRenderContext->skyboxShader.pProgram);

  vcMesh_Destroy(&pRenderContext->pScreenQuadMesh);

  vcTexture_Destroy(&pRenderContext->pSkyboxTexture);
  UD_ERROR_CHECK(vcCompass_Destroy(&pRenderContext->pCompass));

  vcPolygonModel_DestroyShaders();
  vcImageRenderer_Destroy();

  udFree(pRenderContext->udRenderContext.pColorBuffer);
  udFree(pRenderContext->udRenderContext.pDepthBuffer);

  UD_ERROR_CHECK(vcTileRenderer_Destroy(&pRenderContext->pTileRenderer));
  UD_ERROR_CHECK(vcFenceRenderer_Destroy(&pRenderContext->pDiagnosticFences));

epilogue:
  vcTexture_Destroy(&pRenderContext->udRenderContext.pColourTex);
  vcTexture_Destroy(&pRenderContext->udRenderContext.pDepthTex);
  vcTexture_Destroy(&pRenderContext->pTexture);
  vcTexture_Destroy(&pRenderContext->pDepthTexture);
  vcFramebuffer_Destroy(&pRenderContext->pFramebuffer);

  udFree(pRenderContext);
  return result;
}

udResult vcRender_SetVaultContext(vcRenderContext *pRenderContext, vdkContext *pVaultContext)
{
  udResult result = udR_Success;

  UD_ERROR_NULL(pRenderContext, udR_InvalidParameter_);

  pRenderContext->pVaultContext = pVaultContext;

  if (vdkRenderContext_Create(pVaultContext, &pRenderContext->udRenderContext.pRenderer) != vE_Success)
    UD_ERROR_SET(udR_InternalError);

epilogue:
  return result;
}

udResult vcRender_ResizeScene(vcRenderContext *pRenderContext, const uint32_t width, const uint32_t height)
{
  udResult result = udR_Success;

  uint32_t widthIncr = width + (width % vcRender_SceneSizeIncrement != 0 ? vcRender_SceneSizeIncrement - width % vcRender_SceneSizeIncrement : 0);
  uint32_t heightIncr = height + (height % vcRender_SceneSizeIncrement != 0 ? vcRender_SceneSizeIncrement - height % vcRender_SceneSizeIncrement : 0);

  UD_ERROR_NULL(pRenderContext, udR_InvalidParameter_);
  UD_ERROR_IF(width == 0, udR_InvalidParameter_);
  UD_ERROR_IF(height == 0, udR_InvalidParameter_);

  pRenderContext->sceneResolution.x = widthIncr;
  pRenderContext->sceneResolution.y = heightIncr;
  pRenderContext->originalSceneResolution.x = width;
  pRenderContext->originalSceneResolution.y = height;

  //Resize CPU Targets
  udFree(pRenderContext->udRenderContext.pColorBuffer);
  udFree(pRenderContext->udRenderContext.pDepthBuffer);

  pRenderContext->udRenderContext.pColorBuffer = udAllocType(uint32_t, pRenderContext->sceneResolution.x*pRenderContext->sceneResolution.y, udAF_Zero);
  pRenderContext->udRenderContext.pDepthBuffer = udAllocType(float, pRenderContext->sceneResolution.x*pRenderContext->sceneResolution.y, udAF_Zero);

  //Resize GPU Targets
  vcTexture_Destroy(&pRenderContext->udRenderContext.pColourTex);
  vcTexture_Destroy(&pRenderContext->udRenderContext.pDepthTex);
  vcTexture_Create(&pRenderContext->udRenderContext.pColourTex, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y, pRenderContext->udRenderContext.pColorBuffer, vcTextureFormat_BGRA8, vcTFM_Nearest, false, vcTWM_Clamp, vcTCF_Dynamic);
  vcTexture_Create(&pRenderContext->udRenderContext.pDepthTex, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y, pRenderContext->udRenderContext.pDepthBuffer, vcTextureFormat_D32F, vcTFM_Nearest, false, vcTWM_Clamp, vcTCF_Dynamic);

  vcTexture_Destroy(&pRenderContext->pTexture);
  vcTexture_Destroy(&pRenderContext->pDepthTexture);
  vcFramebuffer_Destroy(&pRenderContext->pFramebuffer);
  vcTexture_Create(&pRenderContext->pTexture, widthIncr, heightIncr, nullptr, vcTextureFormat_RGBA8, vcTFM_Nearest, false, vcTWM_Repeat, vcTCF_RenderTarget);
  vcTexture_Create(&pRenderContext->pDepthTexture, widthIncr, heightIncr, nullptr, vcTextureFormat_D24S8, vcTFM_Nearest, false, vcTWM_Repeat, vcTCF_RenderTarget);
  vcFramebuffer_Create(&pRenderContext->pFramebuffer, pRenderContext->pTexture, pRenderContext->pDepthTexture);

  if (pRenderContext->pVaultContext)
    vcRender_RecreateUDView(pRenderContext);

epilogue:
  return result;
}

void vcRenderSkybox(vcRenderContext *pRenderContext)
{
  // Draw the skybox only at the far plane, where there is no geometry.
  // Drawing skybox here (after 'opaque' geometry) saves a bit on fill rate.

  udFloat4x4 viewMatrixF = udFloat4x4::create(pRenderContext->pCamera->matrices.view);
  udFloat4x4 projectionMatrixF = udFloat4x4::create(pRenderContext->pCamera->matrices.projectionNear);
  udFloat4x4 inverseViewProjMatrixF = projectionMatrixF * viewMatrixF;
  inverseViewProjMatrixF.axis.t = udFloat4::create(0, 0, 0, 1);
  inverseViewProjMatrixF.inverse();

  vcGLState_SetViewportDepthRange(1.0f, 1.0f);

  vcShader_Bind(pRenderContext->skyboxShader.pProgram);

  vcShader_BindTexture(pRenderContext->skyboxShader.pProgram, pRenderContext->pSkyboxTexture, 0, pRenderContext->skyboxShader.uniform_texture);
  vcShader_BindConstantBuffer(pRenderContext->skyboxShader.pProgram, pRenderContext->skyboxShader.uniform_MatrixBlock, &inverseViewProjMatrixF, sizeof(inverseViewProjMatrixF));

  vcMesh_Render(pRenderContext->pScreenQuadMesh, 2);

  vcGLState_SetViewportDepthRange(0.0f, 1.0f);

  vcShader_Bind(nullptr);
}

void vcPresentUD(vcRenderContext *pRenderContext)
{
  float nearPlane = pRenderContext->pSettings->camera.nearPlane;
  float farPlane = pRenderContext->pSettings->camera.farPlane;

  // edge outlines
  int outlineWidth = pRenderContext->pSettings->postVisualization.edgeOutlines.width;
  float outlineEdgeThreshold = pRenderContext->pSettings->postVisualization.edgeOutlines.threshold;
  udFloat4 outlineColour = pRenderContext->pSettings->postVisualization.edgeOutlines.colour;
  if (!pRenderContext->pSettings->postVisualization.edgeOutlines.enable)
    outlineColour.w = 0.0f;

  if (pRenderContext->pSettings->camera.cameraMode == vcCM_OrthoMap)
  {
    // adjust some visuals in map mode
    nearPlane = float(vcSL_CameraOrthoNearFarPlane.x);
    farPlane = float(vcSL_CameraOrthoNearFarPlane.y);
    outlineEdgeThreshold /= float(vcSL_CameraOrthoNearFarPlane.y * 0.15);
  }

  // colour by height
  udFloat4 colourByHeightMinColour = pRenderContext->pSettings->postVisualization.colourByHeight.minColour;
  if (!pRenderContext->pSettings->postVisualization.colourByHeight.enable)
    colourByHeightMinColour.w = 0.f;
  udFloat4 colourByHeightMaxColour = pRenderContext->pSettings->postVisualization.colourByHeight.maxColour;
  if (!pRenderContext->pSettings->postVisualization.colourByHeight.enable)
    colourByHeightMaxColour.w = 0.f;
  float colourByHeightStartHeight = pRenderContext->pSettings->postVisualization.colourByHeight.startHeight;
  float colourByHeightEndHeight = pRenderContext->pSettings->postVisualization.colourByHeight.endHeight;

  // colour by depth
  udFloat4 colourByDepthColour = pRenderContext->pSettings->postVisualization.colourByDepth.colour;
  if (!pRenderContext->pSettings->postVisualization.colourByDepth.enable)
    colourByDepthColour.w = 0.f;
  float colourByDepthStart = pRenderContext->pSettings->postVisualization.colourByDepth.startDepth;
  float colourByDepthEnd = pRenderContext->pSettings->postVisualization.colourByDepth.endDepth;

  // contours
  udFloat4 contourColour = pRenderContext->pSettings->postVisualization.contours.colour;
  if (!pRenderContext->pSettings->postVisualization.contours.enable)
    contourColour.w = 0.f;
  float contourDistances = pRenderContext->pSettings->postVisualization.contours.distances;
  float contourBandHeight = pRenderContext->pSettings->postVisualization.contours.bandHeight;

  pRenderContext->udRenderContext.presentShader.params.inverseViewProjection = udFloat4x4::create(pRenderContext->pCamera->matrices.inverseViewProjection);
  pRenderContext->udRenderContext.presentShader.params.screenParams.x = outlineWidth * (1.0f / pRenderContext->pSettings->window.width);
  pRenderContext->udRenderContext.presentShader.params.screenParams.y = outlineWidth * (1.0f / pRenderContext->pSettings->window.height);
  pRenderContext->udRenderContext.presentShader.params.screenParams.z = nearPlane;
  pRenderContext->udRenderContext.presentShader.params.screenParams.w = farPlane;
  pRenderContext->udRenderContext.presentShader.params.outlineColour = outlineColour;
  pRenderContext->udRenderContext.presentShader.params.outlineParams.x = (float)outlineWidth;
  pRenderContext->udRenderContext.presentShader.params.outlineParams.y = outlineEdgeThreshold;
  pRenderContext->udRenderContext.presentShader.params.colourizeHeightColourMin = colourByHeightMinColour;
  pRenderContext->udRenderContext.presentShader.params.colourizeHeightColourMax = colourByHeightMaxColour;
  pRenderContext->udRenderContext.presentShader.params.colourizeHeightParams.x = colourByHeightStartHeight;
  pRenderContext->udRenderContext.presentShader.params.colourizeHeightParams.y = colourByHeightEndHeight;
  pRenderContext->udRenderContext.presentShader.params.colourizeDepthColour = colourByDepthColour;
  pRenderContext->udRenderContext.presentShader.params.colourizeDepthParams.x = colourByDepthStart;
  pRenderContext->udRenderContext.presentShader.params.colourizeDepthParams.y = colourByDepthEnd;
  pRenderContext->udRenderContext.presentShader.params.contourColour = contourColour;
  pRenderContext->udRenderContext.presentShader.params.contourParams.x = contourDistances;
  pRenderContext->udRenderContext.presentShader.params.contourParams.y = contourBandHeight;

  vcShader_Bind(pRenderContext->udRenderContext.presentShader.pProgram);

  vcShader_BindTexture(pRenderContext->udRenderContext.presentShader.pProgram, pRenderContext->udRenderContext.pColourTex, 0, pRenderContext->udRenderContext.presentShader.uniform_texture);
  vcShader_BindTexture(pRenderContext->udRenderContext.presentShader.pProgram, pRenderContext->udRenderContext.pDepthTex, 1, pRenderContext->udRenderContext.presentShader.uniform_depth);
  vcShader_BindConstantBuffer(pRenderContext->udRenderContext.presentShader.pProgram, pRenderContext->udRenderContext.presentShader.uniform_params, &pRenderContext->udRenderContext.presentShader.params, sizeof(pRenderContext->udRenderContext.presentShader.params));

  vcMesh_Render(pRenderContext->pScreenQuadMesh, 2);

  vcShader_Bind(nullptr);
}

void vcRenderTerrain(vcRenderContext *pRenderContext, vcRenderData &renderData)
{
  if (renderData.pGISSpace->isProjected && pRenderContext->pSettings->maptiles.mapEnabled)
  {
    udDouble4x4 cameraMatrix = pRenderContext->pCamera->matrices.camera;
    udDouble4x4 viewProjection = pRenderContext->pCamera->matrices.viewProjection;

#ifndef GIT_BUILD
    static bool debugDetachCamera = false;
    static udDouble4x4 gRealCameraMatrix = udDouble4x4::identity();
    if (!debugDetachCamera)
      gRealCameraMatrix = pRenderContext->pCamera->matrices.camera;

    cameraMatrix = gRealCameraMatrix;
    viewProjection = pRenderContext->pCamera->matrices.projection * udInverse(cameraMatrix);
#endif
    udDouble3 localCamPos = cameraMatrix.axis.t.toVector3();

    // Corners [nw, ne, sw, se]
    udDouble3 localCorners[4];
    udInt2 slippyCorners[4];

    int currentZoom = 21;

    double farPlane = pRenderContext->pSettings->camera.farPlane;
    if (pRenderContext->pSettings->camera.cameraMode == vcCM_OrthoMap)
      farPlane = udMax(farPlane, pRenderContext->pSettings->camera.orthographicSize * 2.0);

    // Cardinal Limits
    localCorners[0] = localCamPos + udDouble3::create(-farPlane, +farPlane, 0);
    localCorners[1] = localCamPos + udDouble3::create(+farPlane, +farPlane, 0);
    localCorners[2] = localCamPos + udDouble3::create(-farPlane, -farPlane, 0);
    localCorners[3] = localCamPos + udDouble3::create(+farPlane, -farPlane, 0);

    for (int i = 0; i < 4; ++i)
      vcGIS_LocalToSlippy(renderData.pGISSpace, &slippyCorners[i], localCorners[i], currentZoom);

    while (currentZoom > 0 && (slippyCorners[0] != slippyCorners[1] || slippyCorners[1] != slippyCorners[2] || slippyCorners[2] != slippyCorners[3]))
    {
      --currentZoom;

      for (int i = 0; i < 4; ++i)
        slippyCorners[i] /= 2;
    }

    for (int i = 0; i < 4; ++i)
      vcGIS_SlippyToLocal(renderData.pGISSpace, &localCorners[i], slippyCorners[0] + udInt2::create(i & 1, i / 2), currentZoom);

    vcTileRenderer_Update(pRenderContext->pTileRenderer, renderData.deltaTime, renderData.pGISSpace, localCorners, udInt3::create(slippyCorners[0], currentZoom), localCamPos, viewProjection);
    vcTileRenderer_Render(pRenderContext->pTileRenderer, pRenderContext->pCamera->matrices.view, pRenderContext->pCamera->matrices.projection);

    if (pRenderContext->pSettings->maptiles.mouseInteracts)
    {
      udPlane<double> mapPlane = udPlane<double>::create({ 0, 0, pRenderContext->pSettings->maptiles.mapHeight }, { 0, 0, 1 });

      udDouble3 hitPoint;
      double hitDistance;
      if (udIntersect(mapPlane, pRenderContext->pCamera->worldMouseRay, &hitPoint, &hitDistance) == udR_Success)
      {
        if (hitDistance < pRenderContext->pSettings->camera.farPlane && (!renderData.pickingSuccess || hitDistance < udMag3(renderData.worldMousePos - localCamPos) - pRenderContext->pSettings->camera.nearPlane))
        {
          renderData.pickingSuccess = true;
          renderData.worldMousePos = hitPoint;
        }
      }
    }
  }
}

void vcRenderOpaquePolygons(vcRenderContext *pRenderContext, vcRenderData &renderData)
{
  vcGLState_ResetState();

  // Polygon Models
  {
    vcGLState_SetBlendMode(vcGLSBM_None);
    vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, true);
    vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_Back);

    for (size_t i = 0; i < renderData.polyModels.length; ++i)
      vcPolygonModel_Render(renderData.polyModels[i].pModel, renderData.polyModels[i].worldMat, pRenderContext->pCamera->matrices.viewProjection);

    for (size_t i = 0; i < renderData.waterVolumes.length; ++i)
      vcWaterRenderer_Render(renderData.waterVolumes[i], pRenderContext->pCamera->matrices.view, pRenderContext->pCamera->matrices.viewProjection, pRenderContext->pSkyboxTexture, renderData.deltaTime);
  }
}

void vcRenderTransparentPolygons(vcRenderContext *pRenderContext, vcRenderData &renderData)
{
  vcGLState_SetBlendMode(vcGLSBM_Interpolative);
  vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, false);

  // Images
  {
    vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_Front);

    for (size_t i = 0; i < renderData.images.length; ++i)
    {
      static const double distScalar = 600.0;

      double zScale = 1.0;
      if (pRenderContext->pSettings->camera.cameraMode == vcCM_FreeRoam)
        zScale -= udMag3(pRenderContext->pCamera->position - renderData.images[i]->position) / distScalar;
      else // map mode
        zScale -= pRenderContext->pSettings->camera.orthographicSize / distScalar;

      if (zScale < 0) // too far
        continue;

      vcImageRenderer_Render(renderData.images[i], pRenderContext->pCamera->matrices.viewProjection, pRenderContext->sceneResolution, zScale);
    }
  }

  // Fences
  {
    vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_None);
    if (pRenderContext->pSettings->presentation.showDiagnosticInfo)
      vcFenceRenderer_Render(pRenderContext->pDiagnosticFences, pRenderContext->pCamera->matrices.viewProjection, renderData.deltaTime);

    for (size_t i = 0; i < renderData.fences.length; ++i)
      vcFenceRenderer_Render(renderData.fences[i], pRenderContext->pCamera->matrices.viewProjection, renderData.deltaTime);
  }
}

vcTexture* vcRender_GetSceneTexture(vcRenderContext *pRenderContext)
{
  return pRenderContext->pTexture;
}

void vcRender_RenderScene(vcRenderContext *pRenderContext, vcRenderData &renderData, vcFramebuffer *pDefaultFramebuffer)
{
  float aspect = pRenderContext->sceneResolution.x / (float)pRenderContext->sceneResolution.y;

  vcGLState_SetDepthStencilMode(vcGLSDM_LessOrEqual, true);

  vcRender_RenderAndUploadUDToTexture(pRenderContext, renderData);

  vcGLState_SetViewport(0, 0, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y);

  vcFramebuffer_Bind(pRenderContext->pFramebuffer);
  vcFramebuffer_Clear(pRenderContext->pFramebuffer, 0xFFFF8080);

  vcPresentUD(pRenderContext);
  vcRenderSkybox(pRenderContext);
  vcRenderOpaquePolygons(pRenderContext, renderData);
  vcRenderTerrain(pRenderContext, renderData);
  vcRenderTransparentPolygons(pRenderContext, renderData);

  if (pRenderContext->pSettings->presentation.mouseAnchor != vcAS_None && (renderData.pickingSuccess || (renderData.pWorldAnchorPos != nullptr)))
  {
    udDouble4x4 mvp = pRenderContext->pCamera->matrices.viewProjection * udDouble4x4::translation(renderData.pWorldAnchorPos ? *renderData.pWorldAnchorPos : renderData.worldMousePos);
    vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_Back);

    // Render highlighting any occlusion
    vcGLState_SetBlendMode(vcGLSBM_Additive);
    vcGLState_SetDepthStencilMode(vcGLSDM_Greater, false);
    vcCompass_Render(pRenderContext->pCompass, pRenderContext->pSettings->presentation.mouseAnchor, mvp, udDouble4::create(0.0, 0.15, 1.0, 0.5));

    // Render non-occluded
    vcGLState_SetBlendMode(vcGLSBM_Interpolative);
    vcGLState_SetDepthStencilMode(vcGLSDM_Less, true);
    vcCompass_Render(pRenderContext->pCompass, pRenderContext->pSettings->presentation.mouseAnchor, mvp);

    vcGLState_ResetState();
  }

  if (pRenderContext->pSettings->presentation.showCompass)
  {
    udDouble4x4 cameraRotation = udDouble4x4::rotationYPR(pRenderContext->pCamera->matrices.camera.extractYPR());
    vcGLState_SetFaceMode(vcGLSFM_Solid, vcGLSCM_Back);
    vcGLState_SetDepthStencilMode(vcGLSDM_Always, false);

    if (!renderData.pGISSpace->isProjected)
    {
      vcCompass_Render(pRenderContext->pCompass, vcAS_Compass, udDouble4x4::perspectiveZO(vcLens30mm, aspect, 0.01, 2.0) * udDouble4x4::translation(vcLens30mm * 0.45 * aspect, 1.0, -vcLens30mm * 0.45) * udDouble4x4::scaleUniform(vcLens30mm / 20.0) * udInverse(cameraRotation));
    }
    else
    {
      udDouble3 currentLatLong = udGeoZone_ToLatLong(renderData.pGISSpace->zone, pRenderContext->pCamera->position);
      currentLatLong.x = udClamp(currentLatLong.x, -90.0, 89.0);
      udDouble3 norther = udGeoZone_ToCartesian(renderData.pGISSpace->zone, udDouble3::create(currentLatLong.x + 1.0, currentLatLong.y, currentLatLong.z));
      udDouble4x4 north = udDouble4x4::lookAt(pRenderContext->pCamera->position, norther);
      vcCompass_Render(pRenderContext->pCompass, vcAS_Compass, udDouble4x4::perspectiveZO(vcLens30mm, aspect, 0.01, 2.0) * udDouble4x4::translation(vcLens30mm * 0.45 * aspect, 1.0, -vcLens30mm * 0.45) * udDouble4x4::scaleUniform(vcLens30mm / 20.0) * udDouble4x4::rotationYPR(north.extractYPR()) * udInverse(cameraRotation));
    }

    vcGLState_ResetState();
  }

  vcShader_Bind(nullptr);

  vcFramebuffer_Bind(pDefaultFramebuffer);
}

void vcRender_vcRenderSceneImGui(vcRenderContext *pRenderContext, const vcRenderData &renderData)
{
  // Labels
  for (size_t i = 0; i < renderData.labels.length; ++i)
    vcLabelRenderer_Render(renderData.labels[i], pRenderContext->pCamera->matrices.viewProjection, pRenderContext->sceneResolution);
}

udResult vcRender_RecreateUDView(vcRenderContext *pRenderContext)
{
  udResult result = udR_Success;

  UD_ERROR_NULL(pRenderContext, udR_InvalidParameter_);

  if (pRenderContext->udRenderContext.pRenderView && vdkRenderView_Destroy(pRenderContext->pVaultContext, &pRenderContext->udRenderContext.pRenderView) != vE_Success)
    UD_ERROR_SET(udR_InternalError);

  if (vdkRenderView_Create(pRenderContext->pVaultContext, &pRenderContext->udRenderContext.pRenderView, pRenderContext->udRenderContext.pRenderer, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y) != vE_Success)
    UD_ERROR_SET(udR_InternalError);

  if (vdkRenderView_SetTargets(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderView, pRenderContext->udRenderContext.pColorBuffer, 0, pRenderContext->udRenderContext.pDepthBuffer) != vE_Success)
    UD_ERROR_SET(udR_InternalError);

  if (vdkRenderView_SetMatrix(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderView, vdkRVM_Projection, pRenderContext->pCamera->matrices.projectionUD.a) != vE_Success)
    UD_ERROR_SET(udR_InternalError);

epilogue:
  return result;
}

udResult vcRender_RenderAndUploadUDToTexture(vcRenderContext *pRenderContext, vcRenderData &renderData)
{
  if (pRenderContext == nullptr)
    return udR_InvalidParameter_;

  vdkRenderInstance *pModels = nullptr;
  int numVisibleModels = 0;

  vdkRenderView_SetMatrix(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderView, vdkRVM_Projection, pRenderContext->pCamera->matrices.projectionUD.a);
  vdkRenderView_SetMatrix(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderView, vdkRVM_View, pRenderContext->pCamera->matrices.view.a);

  switch (pRenderContext->pSettings->visualization.mode)
  {
  case vcVM_Intensity:
    vdkRenderContext_ShowIntensity(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderer, pRenderContext->pSettings->visualization.minIntensity, pRenderContext->pSettings->visualization.maxIntensity);
    break;
  case vcVM_Classification:
    vdkRenderContext_ShowClassification(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderer, (int*)pRenderContext->pSettings->visualization.customClassificationColors, (int)udLengthOf(pRenderContext->pSettings->visualization.customClassificationColors));
    break;
  default: //Includes vcVM_Colour
    vdkRenderContext_ShowColor(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderer);
    break;
  }

  vdkRenderContext_SetPointMode(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderer, (vdkRenderContextPointMode)pRenderContext->pSettings->presentation.pointMode);

  if (renderData.models.length > 0)
    pModels = udAllocStack(vdkRenderInstance, renderData.models.length, udAF_None);

  double maxDistSqr = pRenderContext->pSettings->camera.farPlane * pRenderContext->pSettings->camera.farPlane;
  renderData.pWatermarkTexture = nullptr;

  for (size_t i = 0; i < renderData.models.length; ++i)
  {
    if (renderData.models[i]->m_visible && renderData.models[i]->m_loadStatus == vcSLS_Loaded)
    {
      // Copy to the contiguous array
      pModels[numVisibleModels].pPointCloud = renderData.models[i]->m_pPointCloud;
      memcpy(&pModels[numVisibleModels].matrix, renderData.models[i]->m_sceneMatrix.a, sizeof(pModels[numVisibleModels].matrix));
      ++numVisibleModels;

      if (renderData.models[i]->m_hasWatermark)
      {
        udDouble3 distVector = pRenderContext->pCamera->position - renderData.models[i]->GetWorldSpacePivot();
        if (pRenderContext->pSettings->camera.cameraMode == vcCM_OrthoMap)
          distVector.z = 0.0;

        double cameraDistSqr = udMagSq(distVector);
        if (cameraDistSqr < maxDistSqr)
        {
          maxDistSqr = cameraDistSqr;

          if (renderData.models[i]->m_pWatermark == nullptr) // Load the watermark
          {
            const char *pWatermarkStr = renderData.models[i]->m_metadata.Get("Watermark").AsString();
            if (pWatermarkStr)
            {
              uint8_t *pImage = nullptr;
              size_t imageLen = 0;
              if (udBase64Decode(&pImage, &imageLen, pWatermarkStr) == udR_Success)
              {
                int imageWidth, imageHeight, imageChannels;
                unsigned char *pImageData = stbi_load_from_memory(pImage, (int)imageLen, &imageWidth, &imageHeight, &imageChannels, 4);
                vcTexture_Create(&renderData.models[i]->m_pWatermark, imageWidth, imageHeight, pImageData, vcTextureFormat_RGBA8, vcTFM_Nearest, false);
                free(pImageData);
              }

              udFree(pImage);
            }
          }

          renderData.pWatermarkTexture = renderData.models[i]->m_pWatermark;
        }
      }
    }
  }


  if (pRenderContext->pSettings->presentation.showDiagnosticInfo)
  {
    vcFenceRenderer_ClearPoints(pRenderContext->pDiagnosticFences);

    float z = 0;
    if (pRenderContext->pSettings->maptiles.mapEnabled)
      z = pRenderContext->pSettings->maptiles.mapHeight;

    udInt2 slippyCurrent;
    udDouble3 localCurrent;

    double xRange = renderData.pGISSpace->zone.latLongBoundMax.x - renderData.pGISSpace->zone.latLongBoundMin.x;
    double yRange = renderData.pGISSpace->zone.latLongBoundMax.y - renderData.pGISSpace->zone.latLongBoundMin.y;

    if (xRange > 0 || yRange > 0)
    {
      std::vector<udDouble3> corners;

      for (int i = 0; i < yRange; ++i)
      {
        vcGIS_LatLongToSlippy(&slippyCurrent, udDouble3::create(renderData.pGISSpace->zone.latLongBoundMin.x, renderData.pGISSpace->zone.latLongBoundMin.y + i, 0), 21);
        vcGIS_SlippyToLocal(renderData.pGISSpace, &localCurrent, slippyCurrent, 21);
        corners.push_back(udDouble3::create(localCurrent.x, localCurrent.y, z));
      }
      for (int i = 0; i < xRange; ++i)
      {
        vcGIS_LatLongToSlippy(&slippyCurrent, udDouble3::create(renderData.pGISSpace->zone.latLongBoundMin.x + i, renderData.pGISSpace->zone.latLongBoundMax.y, 0), 21);
        vcGIS_SlippyToLocal(renderData.pGISSpace, &localCurrent, slippyCurrent, 21);
        corners.push_back(udDouble3::create(localCurrent.x, localCurrent.y, z));
      }
      for (int i = 0; i < yRange; ++i)
      {
        vcGIS_LatLongToSlippy(&slippyCurrent, udDouble3::create(renderData.pGISSpace->zone.latLongBoundMax.x, renderData.pGISSpace->zone.latLongBoundMax.y - i, 0), 21);
        vcGIS_SlippyToLocal(renderData.pGISSpace, &localCurrent, slippyCurrent, 21);
        corners.push_back(udDouble3::create(localCurrent.x, localCurrent.y, z));
      }
      for (int i = 0; i < xRange; ++i)
      {
        vcGIS_LatLongToSlippy(&slippyCurrent, udDouble3::create(renderData.pGISSpace->zone.latLongBoundMax.x - i, renderData.pGISSpace->zone.latLongBoundMin.y, 0), 21);
        vcGIS_SlippyToLocal(renderData.pGISSpace, &localCurrent, slippyCurrent, 21);
        corners.push_back(udDouble3::create(localCurrent.x, localCurrent.y, z));
      }
      corners.push_back(udDouble3::create(corners[0]));

      vcFenceRenderer_AddPoints(pRenderContext->pDiagnosticFences, &corners[0], corners.size());
    }
  }

  vdkRenderPicking picking = {};
  picking.x = (uint32_t)((float)renderData.mouse.x / (float)pRenderContext->originalSceneResolution.x * (float)pRenderContext->sceneResolution.x);
  picking.y = (uint32_t)((float)renderData.mouse.y / (float)pRenderContext->originalSceneResolution.y * (float)pRenderContext->sceneResolution.y);

  vdkError result = vdkRenderContext_RenderAdv(pRenderContext->pVaultContext, pRenderContext->udRenderContext.pRenderer, pRenderContext->udRenderContext.pRenderView, pModels, numVisibleModels, &picking);

  if (result == vE_Success)
  {
    if (picking.hit != 0)
    {
      // More to be done here
      renderData.worldMousePos = udDouble3::create(picking.pointCenter[0], picking.pointCenter[1], picking.pointCenter[2]);
      renderData.pickingSuccess = true;
    }

    vcTexture_UploadPixels(pRenderContext->udRenderContext.pColourTex, pRenderContext->udRenderContext.pColorBuffer, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y);
    vcTexture_UploadPixels(pRenderContext->udRenderContext.pDepthTex, pRenderContext->udRenderContext.pDepthBuffer, pRenderContext->sceneResolution.x, pRenderContext->sceneResolution.y);
  }
  else
  {
    //TODO: Clear the buffers
  }

  udFreeStack(pModels);
  return udR_Success;
}

void vcRender_ClearTiles(vcRenderContext *pRenderContext)
{
  if (pRenderContext == nullptr)
    return;

  vcTileRenderer_ClearTiles(pRenderContext->pTileRenderer);
}

void vcRender_ClearPoints(vcRenderContext *pRenderContext)
{
  if (pRenderContext == nullptr)
    return;

  vcFenceRenderer_ClearPoints(pRenderContext->pDiagnosticFences);
}
