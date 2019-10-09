#ifndef vcDXF_Internal_h__
#define vcDXF_Internal_h__

#include "udChunkedArray.h"

#include <unordered_map>
#include <string>

#define TYPEMAPLEN 39 // udLengthOf not working for multidimensional arrays?

struct vcDXF
{
  struct
  {
    udDouble3 minBounds;
    udDouble3 maxBounds;

    double linetypeScale;
    double textSize;

    // int16_t attributeMode; // 0 None, 1 Normal, 2 All
    // double traceWidth;

    /*
    double dimScale;
    double dimRounding;
    // double dimPlusTolerance;
    // double dimMinusTolerance;
    // double dimTolerances; // ? Dimension tolerances generated if nonzero
    // double dimLimits; // ? Dimension limits generated if nonzero
    // int16_t dimZeroSuppression;
                            0 = Suppresses zero feet and precisely zero inches
                            1 = Includes zero feet and precisely zero inches
                            2 = Includes zero feet and suppresses zero inches
                            3 = Includes zero inches and suppresses zero feet

    // bool dimAssociative; // 0 = Create associative dimensioning, 1 = Draw individual entities

    bool alternateUnitDimensioning;
    int16_t alternateDecimalPlaces;
    double alternateUnitScaleFactor;
    */

    // bool limitsChecking;

    double creationTime;
    double updateTime;

    // double angleBase;
    // bool clockwiseAngles;

    int16_t pointDisplayMode;
    double pointDisplaySize;

    double polylineWidth;

    // int16_t splineCurveType
    char *pNextHandle;

    char *pUserCoordSysName;
    udDouble3 userCoordOrigin;
    udDouble3 userCoordXDir;
    udDouble3 userCoordYDir;

    // int16_t worldView 0 = Don't change UCS, 1 = Set UCS to WCS during DVIEW/VPOINT
    // int16_t unitMode; // Low bit set = Display fractions, feet-and-inches, and surveyor's angles in input format
  } header;

  udChunkedArray<udDouble3>
};

enum vcDXF_Type
{
  vcDXFT_BigString,
  vcDXFT_HexString,
  vcDXFT_String,
  vcDXFT_Double,
  vcDXFT_Int16,
  vcDXFT_Int32,
  vcDXFT_Int64,
  vcDXFT_Boolean,
  vcDXFT_Unknown
};

static const int typeMap[][3] =
{ // Ordered list of type mappings corresponding to the group code value table layout
  {(int)vcDXFT_BigString, 0, 9},
  {(int)vcDXFT_Double, 10, 39},
  {(int)vcDXFT_Double, 40, 59},
  {(int)vcDXFT_Int16, 60, 79},
  {(int)vcDXFT_Int32, 90, 99},
  {(int)vcDXFT_String, 100, 100},
  {(int)vcDXFT_String, 102, 102},
  {(int)vcDXFT_HexString, 105, 105},
  {(int)vcDXFT_Double, 110, 119},
  {(int)vcDXFT_Double, 120, 129},
  {(int)vcDXFT_Double, 130, 139},
  {(int)vcDXFT_Double, 140, 149},
  {(int)vcDXFT_Int64, 160, 169},
  {(int)vcDXFT_Int16, 170, 179},
  {(int)vcDXFT_Double, 210, 239},
  {(int)vcDXFT_Int16, 270, 279},
  {(int)vcDXFT_Int16, 280, 289},
  {(int)vcDXFT_Boolean, 290, 299},
  {(int)vcDXFT_String, 300, 309},
  {(int)vcDXFT_HexString, 310, 319},
  {(int)vcDXFT_HexString, 320, 329},
  {(int)vcDXFT_HexString, 330, 369},
  {(int)vcDXFT_Int16, 370, 379},
  {(int)vcDXFT_Int16, 380, 389},
  {(int)vcDXFT_HexString, 390, 399},
  {(int)vcDXFT_Int16, 400, 409},
  {(int)vcDXFT_String, 410, 419},
  {(int)vcDXFT_Int32, 420, 429},
  {(int)vcDXFT_String, 430, 439},
  {(int)vcDXFT_Int32, 440, 449},
  {(int)vcDXFT_Int64, 450, 459},
  {(int)vcDXFT_Double, 460, 469},
  {(int)vcDXFT_String, 470, 479},
  {(int)vcDXFT_HexString, 480, 481},
  {(int)vcDXFT_String, 999, 999},
  {(int)vcDXFT_BigString, 1000, 1009},
  {(int)vcDXFT_Double, 1010, 1059},
  {(int)vcDXFT_Int16, 1060, 1070},
  {(int)vcDXFT_Int32, 1071, 1071}
};

static std::unordered_map<std::string, int> headerCommandEndpoints =
{
  { "$EXTMIN", offsetof(struct vcDXF, header.minBounds) },
  { "$EXTMAX", offsetof(struct vcDXF, header.maxBounds) },
  { "$LTSCALE", offsetof(struct vcDXF, header.linetypeScale) },
  { "$TEXTSIZE", offsetof(struct vcDXF, header.textSize) },
  { "$TDCREATE", offsetof(struct vcDXF, header.creationTime) },
  { "$TDUPDATE", offsetof(struct vcDXF, header.updateTime) },
  { "$PDMODE", offsetof(struct vcDXF, header.pointDisplayMode) },
  { "$PDSIZE", offsetof(struct vcDXF, header.pointDisplaySize) },
  { "$PLINEWID", offsetof(struct vcDXF, header.polylineWidth) },
  { "$HANDSEED", offsetof(struct vcDXF, header.pNextHandle) },
  { "$UCSNAME", offsetof(struct vcDXF, header.pUserCoordSysName) },
  { "$UCSORG", offsetof(struct vcDXF, header.userCoordOrigin) },
  { "$UCSXDIR", offsetof(struct vcDXF, header.userCoordXDir) },
  { "$UCSYDIR", offsetof(struct vcDXF, header.userCoordYDir) }
};

static const char *pSections[] =
{
  "HEADER",
  "CLASSES",
  "TABLES",
  "BLOCKS",
  "ENTITIES",
  "OBJECTS"
};

enum vcDXF_Sections
{
  vcDXFS_Header,
  vcDXFS_Classes,
  vcDXFS_Tables,
  vcDXFS_Blocks,
  vcDXFS_Entities,
  vcDXFS_Objects,
  vcDXFS_Unset
};

#endif //vcDXF_Internal_h__
