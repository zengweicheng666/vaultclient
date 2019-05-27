#ifndef vcProject_h__
#define vcProject_h__

#include "vdkProject.h"

struct vcState;
class vcSceneItem;

void vcProject_RemoveItem(vcState *pProgramState, vdkProjectNode *pParent, vdkProjectNode *pNode);
void vcProject_RemoveAll(vcState *pProgramState);
void vcProject_RemoveSelected(vcState *pProgramState);

void vcProject_SelectItem(vcState *pProgramState, vdkProjectNode *pParent, vdkProjectNode *pNode);
void vcProject_UnselectItem(vcState *pProgramState, vdkProjectNode *pParent, vdkProjectNode *pNode);
void vcProject_ClearSelection(vcState *pProgramState);

bool vcProject_ContainsItem(vdkProjectNode *pParentNode, vdkProjectNode *pItem);
bool vcProject_UseProjectionFromItem(vcState *pProgramState, vcSceneItem *pModel);

#endif // vcProject_h__