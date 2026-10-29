#include "OmniCustomTraversalComponent.h"

#include "AIController.h"
#include "GameFramework/Pawn.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Default.h"
#include "NavAreas/NavArea_Null.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"

UOmniCustomTraversalComponent::UOmniCustomTraversalComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UOmniCustomTraversalComponent::RebuildLinks(
    const TArray<FOmniCustomLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    TSubclassOf<UNavAreaBase> DisabledAreaClass,
    bool bLinksEnabled,
    float InTraversalTimeout)
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        ClearLinks();
        return;
    }

    // Cached specs are only reusable while the transient smart-link components
    // that implement them still belong to this actor. Editor reconstruction,
    // Undo/Redo, or component teardown can invalidate those objects without
    // changing the authored link specs. Rebuild from a clean state when that
    // happens instead of letting an equal-config fast path preserve stale links.
    for (const UNavLinkCustomComponent* Link : CustomLinks)
    {
        if (!IsValid(Link) || Link->GetOwner() != Owner)
        {
            ClearLinks();
            break;
        }
    }

    if (IsConfigurationEquivalent(LinkSpecs, InSupportedAgents, AreaClass, DisabledAreaClass, bLinksEnabled, InTraversalTimeout))
    {
        return;
    }

    // Runtime smart-link enable/disable is intentionally independent from the authored
    // default. Preserve an explicit runtime override across regeneration (for example
    // after a dynamic navigation rebuild or endpoint update). If the authored default
    // itself changes, that is a deliberate authoring change and becomes authoritative
    // again, clearing the transient runtime override.
    const bool bAuthoredEnabledChanged = bHasCachedConfiguration && bCachedAuthoredLinksEnabled != bLinksEnabled;
    if (!bHasCachedConfiguration || bAuthoredEnabledChanged)
    {
        bCachedLinksEnabled = bLinksEnabled;
        bHasRuntimeEnabledOverride = false;
    }

    const bool bEffectiveLinksEnabled = bHasRuntimeEnabledOverride ? bCachedLinksEnabled : bLinksEnabled;

    TraversalTimeout = FMath::Max(0.25f, InTraversalTimeout);

    while (CustomLinks.Num() > LinkSpecs.Num())
    {
        UNavLinkCustomComponent* Link = CustomLinks.Last();
        FinishTraversalsForLink(Link, EOmniCustomTraversalFinishReason::Cancelled);
        CustomLinks.Pop();

        if (Link)
        {
            Owner->RemoveInstanceComponent(Link);
            Link->DestroyComponent();
        }
    }

    while (CustomLinks.Num() < LinkSpecs.Num())
    {
        const FName ComponentName = MakeUniqueObjectName(Owner, UNavLinkCustomComponent::StaticClass(), TEXT("OmniCustomLink"));
        UNavLinkCustomComponent* Link = NewObject<UNavLinkCustomComponent>(Owner, ComponentName, RF_Transient);
        if (!Link)
        {
            break;
        }

        Owner->AddInstanceComponent(Link);
        Link->SetCanEverAffectNavigation(true);
        Link->ForceNavigationRelevancy(true);
        Link->SetMoveReachedLink(this, &UOmniCustomTraversalComponent::HandleMoveReachedLink);

        if (Owner->GetWorld())
        {
            Link->RegisterComponent();
        }

        CustomLinks.Add(Link);
    }

    TSubclassOf<UNavArea> ResolvedAreaClass = UNavArea_Default::StaticClass();
    if (UClass* RequestedAreaClass = AreaClass.Get())
    {
        if (RequestedAreaClass->IsChildOf(UNavArea::StaticClass()))
        {
            ResolvedAreaClass = RequestedAreaClass;
        }
    }

    TSubclassOf<UNavArea> ResolvedDisabledAreaClass = UNavArea_Null::StaticClass();
    if (UClass* RequestedDisabledAreaClass = DisabledAreaClass.Get())
    {
        if (RequestedDisabledAreaClass->IsChildOf(UNavArea::StaticClass()))
        {
            ResolvedDisabledAreaClass = RequestedDisabledAreaClass;
        }
    }

    const FTransform OwnerTransform = Owner->GetActorTransform();
    const int32 LinkCount = FMath::Min(CustomLinks.Num(), LinkSpecs.Num());
    for (int32 Index = 0; Index < LinkCount; ++Index)
    {
        UNavLinkCustomComponent* Link = CustomLinks[Index];
        if (!Link)
        {
            continue;
        }

        const FOmniCustomLinkSpec& Spec = LinkSpecs[Index];
        const FVector RelativeStart = OwnerTransform.InverseTransformPosition(Spec.SourceWorld);
        const FVector RelativeEnd = OwnerTransform.InverseTransformPosition(Spec.TargetWorld);

        Link->SetSupportedAgents(InSupportedAgents);
        Link->SetEnabledArea(ResolvedAreaClass);
        Link->SetDisabledArea(ResolvedDisabledAreaClass);
        Link->SetLinkData(RelativeStart, RelativeEnd, Spec.Direction);
        Link->SetEnabled(bEffectiveLinksEnabled);

        if (!Link->IsRegistered() && Owner->GetWorld())
        {
            Link->RegisterComponent();
        }

        if (Link->IsRegistered())
        {
            Link->RefreshNavigationModifiers();
        }
    }

    CachedLinkSpecs = LinkSpecs;
    CachedSupportedAgents = InSupportedAgents;
    CachedAreaClass = AreaClass.Get();
    CachedDisabledAreaClass = DisabledAreaClass.Get();
    bCachedAuthoredLinksEnabled = bLinksEnabled;
    bCachedLinksEnabled = bEffectiveLinksEnabled;
    bHasCachedConfiguration = true;
}

void UOmniCustomTraversalComponent::SetLinksEnabled(bool bEnabled)
{
    bCachedLinksEnabled = bEnabled;
    bHasRuntimeEnabledOverride = true;
    for (UNavLinkCustomComponent* Link : CustomLinks)
    {
        if (IsValid(Link))
        {
            Link->SetEnabled(bEnabled);
        }
    }
}

void UOmniCustomTraversalComponent::ClearLinks()
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FinishTraversalAtIndex(Index, EOmniCustomTraversalFinishReason::Cancelled);
    }

    if (AActor* Owner = GetOwner())
    {
        for (UNavLinkCustomComponent* Link : CustomLinks)
        {
            if (IsValid(Link) && Link->GetOwner() == Owner)
            {
                Owner->RemoveInstanceComponent(Link);
                Link->DestroyComponent();
            }
        }
    }

    CustomLinks.Reset();
    CachedLinkSpecs.Reset();
    CachedAreaClass.Reset();
    CachedDisabledAreaClass.Reset();
    bCachedAuthoredLinksEnabled = true;
    bCachedLinksEnabled = true;
    bHasRuntimeEnabledOverride = false;
    bHasCachedConfiguration = false;
    SetComponentTickEnabled(false);
}

void UOmniCustomTraversalComponent::HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint)
{
    UPathFollowingComponent* PathFollowing = Cast<UPathFollowingComponent>(PathCompObject);
    if (!PathFollowing || !LinkComp)
    {
        return;
    }

    AAIController* Controller = Cast<AAIController>(PathFollowing->GetOwner());
    APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
    if (!Pawn)
    {
        PathFollowing->FinishUsingCustomLink(LinkComp);
        return;
    }

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].PathFollowing.Get() == PathFollowing)
        {
            FinishTraversalAtIndex(Index, EOmniCustomTraversalFinishReason::Cancelled);
        }
    }

    FActiveCustomTraversal& Traversal = ActiveTraversals.Emplace_GetRef();
    Traversal.Request.RequestId = NextRequestId;
    NextRequestId = NextRequestId >= MAX_int32 ? 1 : NextRequestId + 1;
    Traversal.Request.Pawn = Pawn;
    Traversal.Request.OmniActor = GetOwner();
    Traversal.Request.Start = Pawn->GetActorLocation();
    Traversal.Request.Destination = DestinationPoint;
    Traversal.PathFollowing = PathFollowing;
    Traversal.Link = LinkComp;
    Traversal.ElapsedTime = 0.0f;

    const int32 LinkIndex = FindLinkIndex(LinkComp);
    if (CachedLinkSpecs.IsValidIndex(LinkIndex))
    {
        const FOmniCustomLinkSpec& Spec = CachedLinkSpecs[LinkIndex];
        Traversal.Request.Source = Spec.SourceWorld;
        Traversal.Request.Target = Spec.TargetWorld;

        bool bSourceToTarget = true;
        if (Spec.Direction == ENavLinkDirection::RightToLeft)
        {
            bSourceToTarget = false;
        }
        else if (Spec.Direction == ENavLinkDirection::BothWays)
        {
            const double DistanceToTargetSq = FVector::DistSquared(DestinationPoint, Spec.TargetWorld);
            const double DistanceToSourceSq = FVector::DistSquared(DestinationPoint, Spec.SourceWorld);
            bSourceToTarget = DistanceToTargetSq <= DistanceToSourceSq;
        }

        Traversal.Request.Direction = bSourceToTarget
            ? EOmniTraversalDirection::SourceToTarget
            : EOmniTraversalDirection::TargetToSource;
        Traversal.Request.TraversalId = bSourceToTarget
            ? Spec.SourceToTargetTraversalId
            : Spec.TargetToSourceTraversalId;
    }
    else
    {
        // The link should always have a matching cached spec, but keep a useful fallback
        // if a project mutates components while a path is already using the actor.
        Traversal.Request.Source = Traversal.Request.Start;
        Traversal.Request.Target = DestinationPoint;
        Traversal.Request.Direction = EOmniTraversalDirection::SourceToTarget;
        Traversal.Request.TraversalId = NAME_None;
    }

    SetComponentTickEnabled(true);

    // Copy before broadcasting. A Blueprint listener is allowed to complete the request
    // synchronously, which can remove the active array entry while delegates are executing.
    const FOmniCustomTraversalRequest RequestCopy = Traversal.Request;
    const bool bHasRequestListener = OnTraversalRequestedDetailed.IsBound() || OnTraversalRequested.IsBound();
    if (OnTraversalRequestedDetailed.IsBound())
    {
        OnTraversalRequestedDetailed.Broadcast(RequestCopy);
    }

    if (OnTraversalRequested.IsBound())
    {
        OnTraversalRequested.Broadcast(
            RequestCopy.RequestId,
            Pawn,
            RequestCopy.Start,
            RequestCopy.Destination);
    }

    if (!bHasRequestListener)
    {
        // Never deadlock AI path following just because no Blueprint/C++ listener was bound.
        FinishTraversalAtIndex(ActiveTraversals.Num() - 1, EOmniCustomTraversalFinishReason::NoListener);
    }
}

void UOmniCustomTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FActiveCustomTraversal& Traversal = ActiveTraversals[Index];
        UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get();
        UNavLinkCustomComponent* Link = Traversal.Link.Get();
        if (!PathFollowing || !Link)
        {
            FinishTraversalAtIndex(Index, EOmniCustomTraversalFinishReason::Cancelled);
            continue;
        }

        // AI MoveTo/Behavior Tree Move To may abort or replace a move while both
        // objects remain alive. Treat loss of this exact current custom link as an
        // immediate cancellation instead of holding the request until timeout.
        if (PathFollowing->GetCurrentCustomLinkOb() != Link)
        {
            FinishTraversalAtIndex(Index, EOmniCustomTraversalFinishReason::Cancelled);
            continue;
        }

        Traversal.ElapsedTime += DeltaTime;
        if (Traversal.ElapsedTime >= TraversalTimeout)
        {
            FinishTraversalAtIndex(Index, EOmniCustomTraversalFinishReason::TimedOut);
        }
    }

    if (ActiveTraversals.IsEmpty())
    {
        SetComponentTickEnabled(false);
    }
}

void UOmniCustomTraversalComponent::CompleteTraversal(int32 RequestId)
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].Request.RequestId == RequestId)
        {
            const FActiveCustomTraversal& Traversal = ActiveTraversals[Index];
            UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get();
            UNavLinkCustomComponent* Link = Traversal.Link.Get();
            const bool bStillOwnsCurrentLink = PathFollowing && Link && PathFollowing->GetCurrentCustomLinkOb() == Link;
            FinishTraversalAtIndex(
                Index,
                bStillOwnsCurrentLink ? EOmniCustomTraversalFinishReason::Completed : EOmniCustomTraversalFinishReason::Cancelled);
            return;
        }
    }
}

void UOmniCustomTraversalComponent::CompleteAllTraversals()
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        const FActiveCustomTraversal& Traversal = ActiveTraversals[Index];
        UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get();
        UNavLinkCustomComponent* Link = Traversal.Link.Get();
        const bool bStillOwnsCurrentLink = PathFollowing && Link && PathFollowing->GetCurrentCustomLinkOb() == Link;
        FinishTraversalAtIndex(
            Index,
            bStillOwnsCurrentLink ? EOmniCustomTraversalFinishReason::Completed : EOmniCustomTraversalFinishReason::Cancelled);
    }
}

bool UOmniCustomTraversalComponent::IsTraversalActive(int32 RequestId) const
{
    for (const FActiveCustomTraversal& Traversal : ActiveTraversals)
    {
        if (Traversal.Request.RequestId == RequestId)
        {
            return true;
        }
    }
    return false;
}

bool UOmniCustomTraversalComponent::GetTraversalRequest(int32 RequestId, FOmniCustomTraversalRequest& OutRequest) const
{
    for (const FActiveCustomTraversal& Traversal : ActiveTraversals)
    {
        if (Traversal.Request.RequestId == RequestId)
        {
            OutRequest = Traversal.Request;
            return true;
        }
    }

    OutRequest = FOmniCustomTraversalRequest();
    return false;
}

void UOmniCustomTraversalComponent::FinishTraversalAtIndex(int32 Index, EOmniCustomTraversalFinishReason Reason)
{
    if (!ActiveTraversals.IsValidIndex(Index))
    {
        return;
    }

    FActiveCustomTraversal Traversal = ActiveTraversals[Index];
    ActiveTraversals.RemoveAtSwap(Index);

    if (UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get())
    {
        if (UNavLinkCustomComponent* Link = Traversal.Link.Get())
        {
            // Completion is only valid while this request still owns Unreal's current
            // custom link. Do not let late Blueprint completion or stale teardown
            // resume/advance a newer AI move that has already replaced this one.
            if (PathFollowing->GetCurrentCustomLinkOb() == Link)
            {
                PathFollowing->FinishUsingCustomLink(Link);
            }
        }
    }

    if (OnTraversalFinished.IsBound())
    {
        OnTraversalFinished.Broadcast(Traversal.Request, Reason);
    }

    if (ActiveTraversals.IsEmpty())
    {
        SetComponentTickEnabled(false);
    }
}

void UOmniCustomTraversalComponent::FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp, EOmniCustomTraversalFinishReason Reason)
{
    if (!LinkComp)
    {
        return;
    }

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].Link.Get() == LinkComp)
        {
            FinishTraversalAtIndex(Index, Reason);
        }
    }
}

int32 UOmniCustomTraversalComponent::FindLinkIndex(const UNavLinkCustomComponent* LinkComp) const
{
    if (!LinkComp)
    {
        return INDEX_NONE;
    }

    for (int32 Index = 0; Index < CustomLinks.Num(); ++Index)
    {
        if (CustomLinks[Index] == LinkComp)
        {
            return Index;
        }
    }

    return INDEX_NONE;
}

bool UOmniCustomTraversalComponent::IsConfigurationEquivalent(
    const TArray<FOmniCustomLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    TSubclassOf<UNavAreaBase> DisabledAreaClass,
    bool bLinksEnabled,
    float InTraversalTimeout) const
{
    if (!bHasCachedConfiguration
        || CachedLinkSpecs.Num() != LinkSpecs.Num()
        || CustomLinks.Num() != LinkSpecs.Num()
        || !CachedSupportedAgents.IsSame(InSupportedAgents)
        || CachedAreaClass.Get() != AreaClass.Get()
        || CachedDisabledAreaClass.Get() != DisabledAreaClass.Get()
        || bCachedAuthoredLinksEnabled != bLinksEnabled
        || !FMath::IsNearlyEqual(TraversalTimeout, FMath::Max(0.25f, InTraversalTimeout)))
    {
        return false;
    }

    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return false;
    }

    for (const UNavLinkCustomComponent* Link : CustomLinks)
    {
        if (!IsValid(Link)
            || Link->GetOwner() != Owner
            || (Owner->GetWorld() && !Link->IsRegistered())
            || !Link->GetSupportedAgents().IsSame(InSupportedAgents))
        {
            return false;
        }
    }

    for (int32 Index = 0; Index < LinkSpecs.Num(); ++Index)
    {
        const FOmniCustomLinkSpec& A = CachedLinkSpecs[Index];
        const FOmniCustomLinkSpec& B = LinkSpecs[Index];
        if (!A.SourceWorld.Equals(B.SourceWorld, 0.1f)
            || !A.TargetWorld.Equals(B.TargetWorld, 0.1f)
            || A.Direction != B.Direction
            || A.SourceToTargetTraversalId != B.SourceToTargetTraversalId
            || A.TargetToSourceTraversalId != B.TargetToSourceTraversalId)
        {
            return false;
        }
    }

    return true;
}
