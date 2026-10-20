#include "OmniMantleTraversalComponent.h"

#include "OmniTraversalUtils.h"

#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Default.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"

namespace
{
    constexpr float SpecTolerance = 0.1f;

    bool AreSpecsEquivalent(const FOmniMantleLinkSpec& A, const FOmniMantleLinkSpec& B)
    {
        return A.SourceWorld.Equals(B.SourceWorld, SpecTolerance)
            && A.TargetWorld.Equals(B.TargetWorld, SpecTolerance)
            && A.Direction == B.Direction;
    }
}

UOmniMantleTraversalComponent::UOmniMantleTraversalComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UOmniMantleTraversalComponent::RebuildLinks(
    const TArray<FOmniMantleLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    float InDuration,
    float InArcHeight,
    float InLandingTolerance,
    float InTraversalTimeout)
{
    if (IsConfigurationEquivalent(LinkSpecs, InSupportedAgents, AreaClass, InDuration, InArcHeight, InLandingTolerance, InTraversalTimeout))
    {
        return;
    }

    Duration = FMath::Max(0.05f, InDuration);
    ArcHeight = FMath::Max(0.0f, InArcHeight);
    LandingTolerance = FMath::Max(1.0f, InLandingTolerance);
    TraversalTimeout = FMath::Max(Duration + 0.1f, InTraversalTimeout);

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        ClearLinks();
        return;
    }

    while (MantleLinks.Num() > LinkSpecs.Num())
    {
        UNavLinkCustomComponent* Link = MantleLinks.Last();
        FinishTraversalsForLink(Link);
        MantleLinks.Pop();

        if (Link)
        {
            Owner->RemoveInstanceComponent(Link);
            Link->DestroyComponent();
        }
    }

    while (MantleLinks.Num() < LinkSpecs.Num())
    {
        const FName ComponentName = MakeUniqueObjectName(Owner, UNavLinkCustomComponent::StaticClass(), TEXT("OmniMantleLink"));
        UNavLinkCustomComponent* Link = NewObject<UNavLinkCustomComponent>(Owner, ComponentName, RF_Transient);
        if (!Link)
        {
            break;
        }

        Owner->AddInstanceComponent(Link);
        Link->SetCanEverAffectNavigation(true);
        Link->ForceNavigationRelevancy(true);
        Link->SetMoveReachedLink(this, &UOmniMantleTraversalComponent::HandleMoveReachedLink);

        if (Owner->GetWorld())
        {
            Link->RegisterComponent();
        }

        MantleLinks.Add(Link);
    }

    TSubclassOf<UNavArea> ResolvedAreaClass = UNavArea_Default::StaticClass();
    if (UClass* RequestedAreaClass = AreaClass.Get())
    {
        if (RequestedAreaClass->IsChildOf(UNavArea::StaticClass()))
        {
            ResolvedAreaClass = RequestedAreaClass;
        }
    }

    const FTransform OwnerTransform = Owner->GetActorTransform();
    const int32 LinkCount = FMath::Min(MantleLinks.Num(), LinkSpecs.Num());
    for (int32 Index = 0; Index < LinkCount; ++Index)
    {
        UNavLinkCustomComponent* Link = MantleLinks[Index];
        if (!Link)
        {
            continue;
        }

        const FOmniMantleLinkSpec& Spec = LinkSpecs[Index];
        const FVector RelativeStart = OwnerTransform.InverseTransformPosition(Spec.SourceWorld);
        const FVector RelativeEnd = OwnerTransform.InverseTransformPosition(Spec.TargetWorld);

        Link->SetSupportedAgents(InSupportedAgents);
        Link->SetEnabledArea(ResolvedAreaClass);
        Link->SetLinkData(RelativeStart, RelativeEnd, Spec.Direction);
        Link->SetEnabled(true);

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
    bHasCachedConfiguration = true;
}

void UOmniMantleTraversalComponent::ClearLinks()
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FinishTraversalAtIndex(Index, false);
    }

    if (AActor* Owner = GetOwner())
    {
        for (UNavLinkCustomComponent* Link : MantleLinks)
        {
            if (Link)
            {
                Owner->RemoveInstanceComponent(Link);
                Link->DestroyComponent();
            }
        }
    }

    MantleLinks.Reset();
    CachedLinkSpecs.Reset();
    CachedAreaClass.Reset();
    bHasCachedConfiguration = false;
    SetComponentTickEnabled(false);
}

void UOmniMantleTraversalComponent::HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint)
{
    UPathFollowingComponent* PathFollowing = Cast<UPathFollowingComponent>(PathCompObject);
    if (!PathFollowing || !LinkComp)
    {
        return;
    }

    AAIController* Controller = Cast<AAIController>(PathFollowing->GetOwner());
    ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
    UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
    if (!Character || !Movement)
    {
        PathFollowing->FinishUsingCustomLink(LinkComp);
        return;
    }

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].PathFollowing.Get() == PathFollowing)
        {
            FinishTraversalAtIndex(Index, false);
        }
    }

    const float CapsuleHalfHeight = Character->GetCapsuleComponent()
        ? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
        : 0.0f;

    FActiveMantleTraversal& Traversal = ActiveTraversals.Emplace_GetRef();
    Traversal.PathFollowing = PathFollowing;
    Traversal.Character = Character;
    Traversal.Link = LinkComp;
    Traversal.Start = Character->GetActorLocation();
    Traversal.Destination = DestinationPoint + FVector(0.0, 0.0, CapsuleHalfHeight);
    Traversal.ElapsedTime = 0.0f;
    Traversal.PreviousMovementMode = static_cast<uint8>(Movement->MovementMode);
    Traversal.PreviousCustomMovementMode = Movement->CustomMovementMode;

    Movement->StopMovementImmediately();
    Movement->SetMovementMode(MOVE_Flying);
    SetComponentTickEnabled(true);
}

void UOmniMantleTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FActiveMantleTraversal& Traversal = ActiveTraversals[Index];
        UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get();
        ACharacter* Character = Traversal.Character.Get();
        UNavLinkCustomComponent* Link = Traversal.Link.Get();

        if (!PathFollowing || !Character || !Link)
        {
            FinishTraversalAtIndex(Index, false);
            continue;
        }

        Traversal.ElapsedTime += DeltaTime;
        const float Alpha = FMath::Clamp(Traversal.ElapsedTime / Duration, 0.0f, 1.0f);
        const FVector DesiredLocation = OmniTraversal::CalculateMantlePathPoint(
            Traversal.Start,
            Traversal.Destination,
            Alpha,
            ArcHeight);

        FHitResult Hit;
        Character->SetActorLocation(DesiredLocation, true, &Hit, ETeleportType::None);

        const double HorizontalDistanceSq = FVector::DistSquared2D(Character->GetActorLocation(), Traversal.Destination);
        const double VerticalDistance = FMath::Abs(Character->GetActorLocation().Z - Traversal.Destination.Z);
        const double ToleranceSq = FMath::Square(static_cast<double>(LandingTolerance));
        const bool bNearDestination = HorizontalDistanceSq <= ToleranceSq && VerticalDistance <= static_cast<double>(LandingTolerance * 2.0f);
        const bool bTimedOut = Traversal.ElapsedTime >= TraversalTimeout;

        if (Alpha >= 1.0f && bNearDestination)
        {
            FinishTraversalAtIndex(Index, true);
        }
        else if (bTimedOut)
        {
            // A blocked sweep must never turn the timeout into a teleport through geometry.
            FinishTraversalAtIndex(Index, false);
        }
    }

    if (ActiveTraversals.IsEmpty())
    {
        SetComponentTickEnabled(false);
    }
}

void UOmniMantleTraversalComponent::FinishTraversalAtIndex(int32 Index, bool bSnapToDestination)
{
    if (!ActiveTraversals.IsValidIndex(Index))
    {
        return;
    }

    FActiveMantleTraversal Traversal = ActiveTraversals[Index];
    ActiveTraversals.RemoveAtSwap(Index);

    ACharacter* Character = Traversal.Character.Get();
    if (Character)
    {
        if (bSnapToDestination)
        {
            FHitResult Hit;
            Character->SetActorLocation(Traversal.Destination, true, &Hit, ETeleportType::None);
        }

        if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
        {
            const EMovementMode PreviousMode = static_cast<EMovementMode>(Traversal.PreviousMovementMode);
            Movement->SetMovementMode(PreviousMode, Traversal.PreviousCustomMovementMode);
        }
    }

    if (UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get())
    {
        if (UNavLinkCustomComponent* Link = Traversal.Link.Get())
        {
            PathFollowing->FinishUsingCustomLink(Link);
        }
    }
}

void UOmniMantleTraversalComponent::FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp)
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].Link.Get() == LinkComp)
        {
            FinishTraversalAtIndex(Index, false);
        }
    }
}

bool UOmniMantleTraversalComponent::IsConfigurationEquivalent(
    const TArray<FOmniMantleLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    float InDuration,
    float InArcHeight,
    float InLandingTolerance,
    float InTraversalTimeout) const
{
    if (!bHasCachedConfiguration
        || CachedLinkSpecs.Num() != LinkSpecs.Num()
        || MantleLinks.Num() != LinkSpecs.Num()
        || !CachedSupportedAgents.IsSame(InSupportedAgents)
        || CachedAreaClass.Get() != AreaClass.Get()
        || !FMath::IsNearlyEqual(Duration, FMath::Max(0.05f, InDuration))
        || !FMath::IsNearlyEqual(ArcHeight, FMath::Max(0.0f, InArcHeight))
        || !FMath::IsNearlyEqual(LandingTolerance, FMath::Max(1.0f, InLandingTolerance))
        || !FMath::IsNearlyEqual(TraversalTimeout, FMath::Max(FMath::Max(0.05f, InDuration) + 0.1f, InTraversalTimeout)))
    {
        return false;
    }

    if (const AActor* Owner = GetOwner())
    {
        if (Owner->GetWorld())
        {
            for (const UNavLinkCustomComponent* Link : MantleLinks)
            {
                if (!Link || !Link->IsRegistered())
                {
                    return false;
                }
            }
        }
    }

    for (int32 Index = 0; Index < LinkSpecs.Num(); ++Index)
    {
        if (!AreSpecsEquivalent(CachedLinkSpecs[Index], LinkSpecs[Index]))
        {
            return false;
        }
    }

    return true;
}
