#include "OmniJumpTraversalComponent.h"

#include "OmniTraversalUtils.h"

#include "AIController.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Default.h"
#include "NavLinkCustomComponent.h"
#include "Navigation/PathFollowingComponent.h"

UOmniJumpTraversalComponent::UOmniJumpTraversalComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UOmniJumpTraversalComponent::RebuildLinks(
    const TArray<FOmniJumpLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    float InJumpApexHeight,
    float InLandingTolerance,
    float InTraversalTimeout)
{
    if (IsConfigurationEquivalent(LinkSpecs, InSupportedAgents, AreaClass, InJumpApexHeight, InLandingTolerance, InTraversalTimeout))
    {
        return;
    }

    JumpApexHeight = FMath::Max(1.0f, InJumpApexHeight);
    LandingTolerance = FMath::Max(1.0f, InLandingTolerance);
    TraversalTimeout = FMath::Max(0.25f, InTraversalTimeout);

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        ClearLinks();
        return;
    }

    while (JumpLinks.Num() > LinkSpecs.Num())
    {
        UNavLinkCustomComponent* Link = JumpLinks.Last();
        FinishTraversalsForLink(Link);
        JumpLinks.Pop();

        if (Link)
        {
            Owner->RemoveInstanceComponent(Link);
            Link->DestroyComponent();
        }
    }

    while (JumpLinks.Num() < LinkSpecs.Num())
    {
        const FName ComponentName = MakeUniqueObjectName(Owner, UNavLinkCustomComponent::StaticClass(), TEXT("OmniJumpLink"));
        UNavLinkCustomComponent* Link = NewObject<UNavLinkCustomComponent>(Owner, ComponentName, RF_Transient);
        if (!Link)
        {
            break;
        }

        Owner->AddInstanceComponent(Link);
        Link->SetCanEverAffectNavigation(true);
        Link->ForceNavigationRelevancy(true);
        Link->SetMoveReachedLink(this, &UOmniJumpTraversalComponent::HandleMoveReachedLink);

        if (Owner->GetWorld())
        {
            Link->RegisterComponent();
        }

        JumpLinks.Add(Link);
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
    const int32 LinkCount = FMath::Min(JumpLinks.Num(), LinkSpecs.Num());
    for (int32 Index = 0; Index < LinkCount; ++Index)
    {
        UNavLinkCustomComponent* Link = JumpLinks[Index];
        if (!Link)
        {
            continue;
        }

        const FOmniJumpLinkSpec& Spec = LinkSpecs[Index];
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

void UOmniJumpTraversalComponent::ClearLinks()
{
    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FinishTraversalAtIndex(Index);
    }

    if (AActor* Owner = GetOwner())
    {
        for (UNavLinkCustomComponent* Link : JumpLinks)
        {
            if (Link)
            {
                Owner->RemoveInstanceComponent(Link);
                Link->DestroyComponent();
            }
        }
    }

    JumpLinks.Reset();
    CachedLinkSpecs.Reset();
    CachedAreaClass.Reset();
    bHasCachedConfiguration = false;
    SetComponentTickEnabled(false);
}

void UOmniJumpTraversalComponent::HandleMoveReachedLink(UNavLinkCustomComponent* LinkComp, UObject* PathCompObject, const FVector& DestinationPoint)
{
    UPathFollowingComponent* PathFollowing = Cast<UPathFollowingComponent>(PathCompObject);
    if (!PathFollowing || !LinkComp)
    {
        return;
    }

    AAIController* Controller = Cast<AAIController>(PathFollowing->GetOwner());
    ACharacter* Character = Controller ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
    if (!Character || !Character->GetCharacterMovement())
    {
        PathFollowing->FinishUsingCustomLink(LinkComp);
        return;
    }

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].PathFollowing.Get() == PathFollowing)
        {
            FinishTraversalAtIndex(Index);
        }
    }

    const float CapsuleHalfHeight = Character->GetCapsuleComponent()
        ? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
        : 0.0f;
    const FVector CharacterDestination = DestinationPoint + FVector(0.0, 0.0, CapsuleHalfHeight);
    const FVector LaunchVelocity = CalculateLaunchVelocity(*Character, CharacterDestination);
    Character->LaunchCharacter(LaunchVelocity, true, true);

    FActiveJumpTraversal& Traversal = ActiveTraversals.Emplace_GetRef();
    Traversal.PathFollowing = PathFollowing;
    Traversal.Character = Character;
    Traversal.Link = LinkComp;
    Traversal.Destination = CharacterDestination;
    Traversal.ElapsedTime = 0.0f;

    SetComponentTickEnabled(true);
}

void UOmniJumpTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        FActiveJumpTraversal& Traversal = ActiveTraversals[Index];
        UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get();
        ACharacter* Character = Traversal.Character.Get();
        UNavLinkCustomComponent* Link = Traversal.Link.Get();

        if (!PathFollowing || !Character || !Link)
        {
            ActiveTraversals.RemoveAtSwap(Index);
            continue;
        }

        Traversal.ElapsedTime += DeltaTime;

        const FVector CharacterLocation = Character->GetActorLocation();
        const double HorizontalDistanceSq = FVector::DistSquared2D(CharacterLocation, Traversal.Destination);
        const double VerticalDistance = FMath::Abs(CharacterLocation.Z - Traversal.Destination.Z);
        const double ToleranceSq = FMath::Square(static_cast<double>(LandingTolerance));
        const bool bNearDestination = HorizontalDistanceSq <= ToleranceSq && VerticalDistance <= static_cast<double>(LandingTolerance * 2.0f);

        UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
        const bool bDescendingOrGrounded = !Movement || Movement->IsMovingOnGround() || Character->GetVelocity().Z <= 0.0f;
        const bool bTimedOut = Traversal.ElapsedTime >= TraversalTimeout;

        if ((bNearDestination && bDescendingOrGrounded) || bTimedOut)
        {
            FinishTraversalAtIndex(Index);
        }
    }

    if (ActiveTraversals.IsEmpty())
    {
        SetComponentTickEnabled(false);
    }
}

void UOmniJumpTraversalComponent::FinishTraversalAtIndex(int32 Index)
{
    if (!ActiveTraversals.IsValidIndex(Index))
    {
        return;
    }

    FActiveJumpTraversal Traversal = ActiveTraversals[Index];
    ActiveTraversals.RemoveAtSwap(Index);

    if (UPathFollowingComponent* PathFollowing = Traversal.PathFollowing.Get())
    {
        if (UNavLinkCustomComponent* Link = Traversal.Link.Get())
        {
            PathFollowing->FinishUsingCustomLink(Link);
        }
    }
}

void UOmniJumpTraversalComponent::FinishTraversalsForLink(UNavLinkCustomComponent* LinkComp)
{
    if (!LinkComp)
    {
        return;
    }

    for (int32 Index = ActiveTraversals.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveTraversals[Index].Link.Get() == LinkComp)
        {
            FinishTraversalAtIndex(Index);
        }
    }
}

bool UOmniJumpTraversalComponent::IsConfigurationEquivalent(
    const TArray<FOmniJumpLinkSpec>& LinkSpecs,
    const FNavAgentSelector& InSupportedAgents,
    TSubclassOf<UNavAreaBase> AreaClass,
    float InJumpApexHeight,
    float InLandingTolerance,
    float InTraversalTimeout) const
{
    if (!bHasCachedConfiguration
        || CachedLinkSpecs.Num() != LinkSpecs.Num()
        || JumpLinks.Num() != LinkSpecs.Num()
        || !CachedSupportedAgents.IsSame(InSupportedAgents)
        || CachedAreaClass.Get() != AreaClass.Get()
        || !FMath::IsNearlyEqual(JumpApexHeight, FMath::Max(1.0f, InJumpApexHeight))
        || !FMath::IsNearlyEqual(LandingTolerance, FMath::Max(1.0f, InLandingTolerance))
        || !FMath::IsNearlyEqual(TraversalTimeout, FMath::Max(0.25f, InTraversalTimeout)))
    {
        return false;
    }

    if (const AActor* Owner = GetOwner())
    {
        if (Owner->GetWorld())
        {
            for (const UNavLinkCustomComponent* Link : JumpLinks)
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
        const FOmniJumpLinkSpec& A = CachedLinkSpecs[Index];
        const FOmniJumpLinkSpec& B = LinkSpecs[Index];
        if (!A.SourceWorld.Equals(B.SourceWorld, 0.1f)
            || !A.TargetWorld.Equals(B.TargetWorld, 0.1f)
            || A.Direction != B.Direction)
        {
            return false;
        }
    }

    return true;
}

FVector UOmniJumpTraversalComponent::CalculateLaunchVelocity(const ACharacter& Character, const FVector& DestinationPoint) const
{
    const FVector Start = Character.GetActorLocation();
    const UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
    const float GravityMagnitude = Movement ? FMath::Abs(Movement->GetGravityZ()) : 980.0f;

    FVector LaunchVelocity = FVector::ZeroVector;
    float TotalTime = 0.0f;
    if (!OmniTraversal::CalculateBallisticLaunch(
            Start,
            DestinationPoint,
            JumpApexHeight,
            GravityMagnitude,
            LaunchVelocity,
            TotalTime))
    {
        return FVector::ZeroVector;
    }

    return LaunchVelocity;
}
