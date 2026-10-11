#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "OmniTypes.generated.h"

/** How valid Source and Target samples are paired. */
UENUM(BlueprintType)
enum class EOmniPairingMode : uint8
{
    Nearest UMETA(DisplayName="Nearest"),
    MatchedGrid UMETA(DisplayName="Matched Grid")
};

/** Designer-facing sampling density shared by Omni authoring actors. */
UENUM(BlueprintType)
enum class EOmniCoverageDensity : uint8
{
    Sparse UMETA(DisplayName="Sparse"),
    Balanced UMETA(DisplayName="Balanced"),
    Dense UMETA(DisplayName="Dense"),
    Custom UMETA(DisplayName="Custom")
};

/** How a closed spline contributes navigation samples. */
UENUM(BlueprintType)
enum class EOmniClosedSplineCoverage : uint8
{
    Perimeter UMETA(DisplayName="Perimeter"),
    FilledArea UMETA(DisplayName="Filled Area")
};

/** How an AI physically traverses an Omni-generated crossing. */
UENUM(BlueprintType)
enum class EOmniTraversalAction : uint8
{
    NativeMovement UMETA(DisplayName="Native Movement"),
    Jump UMETA(DisplayName="Jump"),
    Automatic UMETA(DisplayName="Automatic"),
    Custom UMETA(DisplayName="Custom")
};

/** How much editor-only information Omni draws while an authoring actor is selected. */
UENUM(BlueprintType)
enum class EOmniPreviewDetail : uint8
{
    LinksOnly UMETA(DisplayName="Links Only"),
    SamplesAndLinks UMETA(DisplayName="Samples + Links"),
    Diagnostics UMETA(DisplayName="Diagnostics")
};

/** Runtime description of one generated smart jump link. World-space endpoints are converted by the host component. */
struct FOmniJumpLinkSpec
{
    FVector SourceWorld = FVector::ZeroVector;
    FVector TargetWorld = FVector::ZeroVector;
    ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
};

/** Runtime description of one generated custom traversal smart link. */
struct FOmniCustomLinkSpec
{
    FVector SourceWorld = FVector::ZeroVector;
    FVector TargetWorld = FVector::ZeroVector;
    ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
};
