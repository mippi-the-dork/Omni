#pragma once

#include "CoreMinimal.h"
#include "NavAreas/NavArea.h"
#include "OmniNavArea_Jump.generated.h"

/**
 * Default navigation area used by Omni Jump smart links.
 *
 * It intentionally costs more than ordinary walkable navigation so pathfinding
 * prefers a normal ground route when one is reasonably competitive, while still
 * allowing a Jump link when it provides the useful or only route.
 */
UCLASS(Config=Engine)
class OMNI_API UOmniNavArea_Jump : public UNavArea
{
    GENERATED_BODY()

public:
    UOmniNavArea_Jump(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
