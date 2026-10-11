#include "OmniNavArea_Jump.h"

UOmniNavArea_Jump::UOmniNavArea_Jump(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    DefaultCost = 2.0f;
    FixedAreaEnteringCost = 50.0f;
    DrawColor = FColor(255, 170, 70);
}
