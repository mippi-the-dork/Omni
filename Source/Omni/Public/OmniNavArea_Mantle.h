#pragma once

#include "CoreMinimal.h"
#include "NavAreas/NavArea.h"
#include "OmniNavArea_Mantle.generated.h"

/** Default navigation area used by Omni Mantle smart links. */
UCLASS(Config=Engine)
class OMNI_API UOmniNavArea_Mantle : public UNavArea
{
    GENERATED_BODY()

public:
    UOmniNavArea_Mantle(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
