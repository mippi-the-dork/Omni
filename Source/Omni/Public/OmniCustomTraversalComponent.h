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

/** Original compact request event retained for existing Blueprint/C++ bindings. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(
    FOnOmniCustomTraversalRequested,
    int32, RequestId,
    APawn*, Pawn,
    FVector, Start,
    FVector, Destination);

/** Full Custom traversal context including Omni actor, traversal ID, and Source/Target direction. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
    FOnOmniCustomTraversalRequestedDetailed,
    FOmniCustomTraversalRequest, Request);

/** Fired whenever a Custom traversal request stops waiting for project movement logic. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
    FOnOmniCustomTraversalFinished,
    FOmniCustomTraversalRequest, Request,
    EOmniCustomTraversalFinishReason, Reason);

/**
 * Runtime helper for designer-defined Omni traversal.
 *
 * When path following reaches one of these smart links, normal path following pauses
 * and the request events fire. Blueprint or C++ should perform the physical traversal
 * and call CompleteTraversal(RequestId) when it is safe for navigation to continue.
 */
UCLASS(ClassGroup=(Omni), meta=(BlueprintSpawnableComponent))
class OMNI_API UOmniCustomTraversalComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOmniCustomTraversalComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /**
     * Compact request event from Omni 0.15/0.16. Retained for compatibility.
     * Prefer OnTraversalRequestedDetailed for new work that needs IDs or direction.
     */
    UPROPERTY(BlueprintAssignable, Category="Omni|Custom Traversal")
    FOnOmniCustomTraversalRequested OnTraversalRequested;

    /** Full request payload including Traversal ID, direction, actor, and authored Source/Target endpoints. */
    UPROPERTY(BlueprintAssignable, Category="Omni|Custom Traversal")
    FOnOmniCustomTraversalRequestedDetailed OnTraversalRequestedDetailed;

    /** Reports completion, timeout, cancellation, or the no-listener safety release. */
    UPROPERTY(BlueprintAssignable, Category="Omni|Custom Traversal")
    FOnOmniCustomTraversalFinished OnTraversalFinished;

    /** Resume normal path following for one active Custom traversal request. */
    UFUNCTION(BlueprintCallable, Category="Omni|Custom Traversal", meta=(DisplayName="Complete Custom Traversal"))
    void CompleteTraversal(int32 RequestId);

    /** Resume every active Custom traversal currently owned by this component. */
    UFUNCTION(BlueprintCallable, Category="Omni|Custom Traversal", meta=(DisplayName="Complete All Custom Traversals"))
    void CompleteAllTraversals();

    /** Returns true when RequestId is still waiting for completion. */
    UFUNCTION(BlueprintPure, Category="Omni|Custom Traversal")
    bool IsTraversalActive(int32 RequestId) const;

    /** Retrieves the full context for an active request. */
    UFUNCTION(BlueprintCallable, Category="Omni|Custom Traversal", meta=(DisplayName="Get Custom Traversal Request"))
    bool GetTraversalRequest(int32 RequestId, FOmniCustomTraversalRequest& OutRequest) const;

    /** Synchronize generated Custom smart links with the authoring actor. */
    void RebuildLinks(
        const TArray<FOmniCustomLinkSpec>& LinkSpecs,
        const FNavAgentSelector& SupportedAgents,
        TSubclassOf<UNavAreaBase> AreaClass,
        float InTraversalTimeout);

    /** Remove all generated Custom smart links and cancel active requests. */
    void ClearLinks();

    int32 GetGeneratedLinkCount() const { return CustomLinks.Num(); }
    int32 GetActiveTraversalCount() const { return ActiveTraversals.Num(); }

private:
    struct FActiveCustomTraversal
    {
        FOmniCustomTraversalRequest Request;
        TWeakObjectPtr<UPathFollowingComponent> PathFollowing;
        TWeakObjectPtr<UNavLinkCustomComponent> Link;
        float ElapsedTime = 0.0f;
    };

    void HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint);
    void FinishTraversalAtIndex(int32 Index, EOmniCustomTraversalFinishReason Reason);
    void FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp, EOmniCustomTraversalFinishReason Reason);
    int32 FindLinkIndex(const UNavLinkCustomComponent* LinkComp) const;
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
