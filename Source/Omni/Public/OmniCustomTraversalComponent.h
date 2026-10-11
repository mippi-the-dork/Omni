#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "Components/ActorComponent.h"
#include "OmniTypes.h"
#include "OmniCustomTraversalComponent.generated.h"

class APawn;
class UNavAreaBase;
class UNavLinkCustomComponent;
class UPathFollowingComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
    FOnOmniCustomTraversalRequested,
    int32, RequestId,
    APawn*, Pawn,
    FVector, Start,
    FVector, Destination);

/**
 * Runtime helper for designer-defined Omni traversal.
 *
 * When path following reaches one of these smart links, normal path following pauses
 * and OnTraversalRequested fires. Blueprint or C++ should perform the physical traversal
 * and call CompleteTraversal(RequestId) when it is safe for navigation to continue.
 */
UCLASS(ClassGroup=(Omni), meta=(BlueprintSpawnableComponent))
class OMNI_API UOmniCustomTraversalComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOmniCustomTraversalComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Fired when an AI reaches an Omni Custom traversal link. */
    UPROPERTY(BlueprintAssignable, Category="Omni|Custom Traversal")
    FOnOmniCustomTraversalRequested OnTraversalRequested;

    /** Resume normal path following for one active Custom traversal request. */
    UFUNCTION(BlueprintCallable, Category="Omni|Custom Traversal", meta=(DisplayName="Complete Custom Traversal"))
    void CompleteTraversal(int32 RequestId);

    /** Resume every active Custom traversal currently owned by this component. */
    UFUNCTION(BlueprintCallable, Category="Omni|Custom Traversal", meta=(DisplayName="Complete All Custom Traversals"))
    void CompleteAllTraversals();

    /** Returns true when RequestId is still waiting for completion. */
    UFUNCTION(BlueprintPure, Category="Omni|Custom Traversal")
    bool IsTraversalActive(int32 RequestId) const;

    /** Synchronize generated Custom smart links with the authoring actor. */
    void RebuildLinks(
        const TArray<FOmniCustomLinkSpec>& LinkSpecs,
        const FNavAgentSelector& SupportedAgents,
        TSubclassOf<UNavAreaBase> AreaClass,
        float InTraversalTimeout);

    /** Remove all generated Custom smart links and release active requests. */
    void ClearLinks();

    int32 GetGeneratedLinkCount() const { return CustomLinks.Num(); }
    int32 GetActiveTraversalCount() const { return ActiveTraversals.Num(); }

private:
    struct FActiveCustomTraversal
    {
        int32 RequestId = INDEX_NONE;
        TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
        TWeakObjectPtr<APawn> Pawn;
        TWeakObjectPtr<UNavLinkCustomComponent> Link;
        FVector Start = FVector::ZeroVector;
        FVector Destination = FVector::ZeroVector;
        float ElapsedTime = 0.0f;
    };

    void HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint);
    void FinishTraversalAtIndex(int32 Index);
    void FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp);
    bool IsConfigurationEquivalent(const TArray<FOmniCustomLinkSpec>& LinkSpecs, const FNavAgentSelector& SupportedAgents, TSubclassOf<UNavAreaBase> AreaClass, float InTraversalTimeout) const;

    UPROPERTY(Transient)
    TArray<TObjectPtr<UNavLinkCustomComponent>> CustomLinks;

    TArray<FActiveCustomTraversal> ActiveTraversals;

    float TraversalTimeout = 5.0f;
    int32 NextRequestId = 1;

    TArray<FOmniCustomLinkSpec> CachedLinkSpecs;
    FNavAgentSelector CachedSupportedAgents;
    TWeakObjectPtr<UClass> CachedAreaClass;
    bool bHasCachedConfiguration = false;
};
