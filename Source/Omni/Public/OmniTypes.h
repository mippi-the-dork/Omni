#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "OmniTypes.generated.h"

class AActor;
class APawn;

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
    // Preserve the serialized values used by Omni 0.9 through 0.17.
    NativeMovement = 0 UMETA(DisplayName="Native Movement"),
    Jump = 1 UMETA(DisplayName="Jump"),
    Automatic = 2 UMETA(DisplayName="Automatic"),
    Custom = 3 UMETA(DisplayName="Custom"),
    Mantle = 4 UMETA(DisplayName="Mantle")
};

/** How much editor-only information Omni draws while an authoring actor is selected. */
UENUM(BlueprintType)
enum class EOmniPreviewDetail : uint8
{
    LinksOnly UMETA(DisplayName="Links Only"),
    SamplesAndLinks UMETA(DisplayName="Samples + Links"),
    Diagnostics UMETA(DisplayName="Diagnostics")
};

/** Source/Target direction for one runtime Custom traversal request. */
UENUM(BlueprintType)
enum class EOmniTraversalDirection : uint8
{
    SourceToTarget UMETA(DisplayName="Source to Target"),
    TargetToSource UMETA(DisplayName="Target to Source")
};

/** Why an active Custom traversal request stopped waiting for project movement logic. */
UENUM(BlueprintType)
enum class EOmniCustomTraversalFinishReason : uint8
{
    Completed UMETA(DisplayName="Completed"),
    TimedOut UMETA(DisplayName="Timed Out"),
    Cancelled UMETA(DisplayName="Cancelled"),
    NoListener UMETA(DisplayName="No Listener")
};

/**
 * Blueprint-friendly runtime context for one Custom traversal request.
 *
 * Source and Target are the authored/projected Omni endpoints. Start is the Pawn's
 * actual location when path following reached the smart link, and Destination is
 * the endpoint Unreal asked the Pawn to reach for this traversal direction.
 */
USTRUCT(BlueprintType)
struct OMNI_API FOmniCustomTraversalRequest
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    int32 RequestId = INDEX_NONE;

    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    TObjectPtr<APawn> Pawn = nullptr;

    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    TObjectPtr<AActor> OmniActor = nullptr;

    /** Designer-authored identifier used to route this request to mantle, door, teleport, or other project logic. */
    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    FName TraversalId = NAME_None;

    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    EOmniTraversalDirection Direction = EOmniTraversalDirection::SourceToTarget;

    /** Pawn location when the Custom smart link was reached. */
    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    FVector Start = FVector::ZeroVector;

    /** Runtime destination supplied by Unreal path following for this request. */
    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    FVector Destination = FVector::ZeroVector;

    /** Authored/projected Source endpoint of the generated Omni link. */
    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    FVector Source = FVector::ZeroVector;

    /** Authored/projected Target endpoint of the generated Omni link. */
    UPROPERTY(BlueprintReadOnly, Category="Omni|Custom Traversal")
    FVector Target = FVector::ZeroVector;
};

/** Runtime description of one generated smart jump link. World-space endpoints are converted by the host component. */
struct FOmniJumpLinkSpec
{
    FVector SourceWorld = FVector::ZeroVector;
    FVector TargetWorld = FVector::ZeroVector;
    ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
};


/** Runtime description of one generated built-in mantle smart link. */
struct FOmniMantleLinkSpec
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

    /** ID reported when traversing from Source toward Target. */
    FName SourceToTargetTraversalId = NAME_None;

    /** ID reported when traversing from Target toward Source. */
    FName TargetToSourceTraversalId = NAME_None;
};
