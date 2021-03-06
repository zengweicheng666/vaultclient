#ifndef vcBPA_h__
#define vcBPA_h__

// This module is based on the Ball-Pivoting Algorithm (BPA) by
// Fausto Bernardini, Joshua Mittleman, Holly Rushmeier, Cláudio Silva, and Gabriel Taubin
// https://lidarwidgets.com/samples/bpa_tvcg.pdf

#include "vdkPointCloud.h"

void vcBPA_CompareExport(vdkContext *pContext, vdkPointCloud *pOldModel, vdkPointCloud *pNewModel, double ballRadius);

#endif //vcBPA_h__
