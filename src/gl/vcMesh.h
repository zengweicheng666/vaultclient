#ifndef vcMesh_h__
#define vcMesh_h__

#include "udPlatform/udMath.h"
#include "gl/vcShader.h"

struct vcMesh;

enum vcMeshFlags
{
  vcMF_None = 0,
  vcMF_Dynamic = 1 << 0,
  vcMF_NoIndexBuffer = 1 << 1,
  vcMF_IndexShort = 1 << 2,
};
inline vcMeshFlags operator|(vcMeshFlags a, vcMeshFlags b) { return (vcMeshFlags)(int(a) | int(b)); }

udResult vcMesh_Create(vcMesh **ppMesh, const vcVertexLayoutTypes *pMeshLayout, int totalTypes, const void* pVerts, int currentVerts, const void *pIndices, int currentIndices, vcMeshFlags flags = vcMF_None);
//bool vcMesh_CreateSimple(vcMesh **ppMesh, const vcSimpleVertex *pVerts, int totalVerts, const int *pIndices, int totalIndices);
void vcMesh_Destroy(vcMesh **ppMesh);

udResult vcMesh_UploadData(vcMesh *pMesh, const vcVertexLayoutTypes *pLayout, int totalTypes, const void* pVerts, int totalVerts, const void *pIndices, int totalIndices);

bool vcMesh_RenderTriangles(vcMesh *pMesh, uint32_t triangleCount, uint32_t startTriangle = 0);

#endif // vcMesh_h__