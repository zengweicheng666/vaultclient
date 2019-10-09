#ifndef vcDXF_h__
#define vcDXF_h__

#include "udResult.h"
#include "vdkProject.h"
#include "udGeoZone.h"

struct vcDXF;

udResult vcDXF_Create(vcDXF **ppDXF);
udResult vcDXF_Destroy(vcDXF **ppDXF);

udResult vcDXF_Load(vcDXF **ppDXF, const char *pFilename);
udResult vcDXF_Save(vcDXF *pDXF, const char *pFilename);

udResult vcDXF_AddToProject(vcDXF *pDXF, vdkProject *pProject, udGeoZone *pSourceZone = nullptr);

#endif //vcDXF_h__
