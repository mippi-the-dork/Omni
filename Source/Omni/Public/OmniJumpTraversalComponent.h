#pragma once

#include "CoreMinimal.h"
#include "AITypes.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "Components/ActorComponent.h"
#include "OmniTypes.h"
#include "OmniJumpTraversalComponent.generated.h"

class ACharacter;
class UCharacterMovementComponent;
class UNavAreaBase;
class UNavLinkCustomComponent;
class UPathFollowingComponent;

/**
 * Internal runtime helper used by Omni authoring actors when Traversal Action is Jump.
 *
 * It owns one UNavLinkCustomComponent per generated crossing and performs a simple
 * ballistic Character launch when path following reaches that custom link. The
 * original UPathFollowingComponent is resumed automatically after landing near the
 * destination or after a safety timeout.
 */
UCLASS(Transient)
class OMNI_API UOmniJumpTraversalComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOmniJumpTraversalComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Synchronize the generated smart-link components with a new set of jump crossings. */
    void RebuildLinks(
        const TArray<FOmniJumpLinkSpec>& LinkSpecs,
        const FNavAgentSelector& SupportedAgents,
        TSubclassOf<UNavAreaBase> AreaClass,
        float InJumpApexHeight,
        float InLandingTolerance,
        float InTraversalTimeout);

    /**
     * Toggle the existing generated Jump smart links without destroying their
     * navigation identity. Disabling also cancels any traversal currently owned
     * by this component and restores its pre-jump movement state.
     */
    void SetLinksEnabled(bool bEnabled);

    /** Remove every generated jump link. */
    void ClearLinks();

    int32 GetGeneratedLinkCount() const { return JumpLinks.Num(); }
    int32 GetActiveTraversalCount() const { return ActiveTraversals.Num(); }

private:
    struct FActiveJumpTraversal
    {
        TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
        FAIRequestID MoveRequestId = FAIRequestID::InvalidRequest;
        TWeakObjectPtr<ACharacter> Character;
        TWeakObjectPtr<UNavLinkCustomComponent> Link;
        FVector Destination = FVector::ZeroVector;
        float ElapsedTime = 0.0f;
        uint8 PreviousMovementMode = 0;
        uint8 PreviousCustomMovementMode = 0;
    };

    void HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint);
    void FinishTraversalAtIndex(int32 Index, bool bRestorePreJumpMovement = false);
    void FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp);
    FVector CalculateLaunchVelocity(const ACharacter& Character, const FVector& DestinationPoint) const;
    bool IsConfigurationEquivalent(const TArray<FOmniJumpLinkSpec>& LinkSpecs, const FNavAgentSelector& SupportedAgents, TSubclassOf<UNavAreaBase> AreaClass, float InJumpApexHeight, float InLandingTolerance, float InTraversalTimeout) const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNavLinkCustomComponent>> JumpLinks;

    TArray<FActiveJumpTraversal> ActiveTraversals;

    float JumpApexHeight = 150.0f;
    float LandingTolerance = 75.0f;
    float TraversalTimeout = 3.0f;

    TArray<FOmniJumpLinkSpec> CachedLinkSpecs;
    FNavAgentSelector CachedSupportedAgents;
    TWeakObjectPtr<UClass> CachedAreaClass;
    bool bHasCachedConfiguration = false;
};
