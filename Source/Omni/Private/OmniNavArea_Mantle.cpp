#include "OmniNavArea_Mantle.h"

UOmniNavArea_Mantle::UOmniNavArea_Mantle(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DefaultCost = 2.25f;
    FixedAreaEnteringCost = 60.0f;
    DrawColor = FColor(90, 210, 255);
}
