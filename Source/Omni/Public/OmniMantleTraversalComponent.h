#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "Components/ActorComponent.h"
#include "OmniTypes.h"
#include "OmniMantleTraversalComponent.generated.h"

class ACharacter;
class UNavAreaBase;
class UNavLinkCustomComponent;
class UPathFollowingComponent;

/**
 * Runtime helper used by Omni authoring actors for built-in Mantle traversal.
 *
 * Each generated mantle crossing is represented by a custom navigation link.
 * When path following reaches the link, Omni temporarily moves an ACharacter
 * along a controlled eased arc to the destination, then resumes path following.
 * This is intentionally animation-agnostic so projects can use it immediately;
 * projects that need animation-driven mantles can still choose Custom traversal.
 */
UCLASS(Transient)
class OMNI_API UOmniMantleTraversalComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOmniMantleTraversalComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    void RebuildLinks(
        const TArray<FOmniMantleLinkSpec>& LinkSpecs,
        const FNavAgentSelector& SupportedAgents,
        TSubclassOf<UNavAreaBase> AreaClass,
        float InDuration,
        float InArcHeight,
        float InLandingTolerance,
        float InTraversalTimeout);

    void ClearLinks();

    int32 GetGeneratedLinkCount() const { return MantleLinks.Num(); }

private:
    struct FActiveMantleTraversal
    {
        TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
        TWeakObjectPtr<ACharacter> Character;
        TWeakObjectPtr<UNavLinkCustomComponent> Link;
        FVector Start = FVector::ZeroVector;
        FVector Destination = FVector::ZeroVector;
        float ElapsedTime = 0.0f;
        uint8 PreviousMovementMode = 0;
        uint8 PreviousCustomMovementMode = 0;
    };

    void HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint);
    void FinishTraversalAtIndex(int32 Index, bool bSnapToDestination);
    void FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp);
    bool IsConfigurationEquivalent(
        const TArray<FOmniMantleLinkSpec>& LinkSpecs,
        const FNavAgentSelector& SupportedAgents,
        TSubclassOf<UNavAreaBase> AreaClass,
        float InDuration,
        float InArcHeight,
        float InLandingTolerance,
        float InTraversalTimeout) const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNavLinkCustomComponent>> MantleLinks;

    TArray<FActiveMantleTraversal> ActiveTraversals;

    float Duration = 0.45f;
    float ArcHeight = 35.0f;
    float LandingTolerance = 20.0f;
    float TraversalTimeout = 2.0f;

    TArray<FOmniMantleLinkSpec> CachedLinkSpecs;
    FNavAgentSelector CachedSupportedAgents;
    TWeakObjectPtr<UClass> CachedAreaClass;
    bool bHasCachedConfiguration = false;
};
