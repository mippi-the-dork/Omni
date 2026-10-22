#include "OmniPointNavLink.h"

#include "OmniJumpTraversalComponent.h"
#include "OmniMantleTraversalComponent.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniNavArea_Jump.h"
#include "OmniNavArea_Mantle.h"
#include "OmniTraversalUtils.h"
#include "OmniPreviewUtils.h"

#include "AI/NavigationSystemBase.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavLinkComponent.h"

#if WITH_EDITOR
#include "Engine/World.h"
#endif

namespace OmniPointNavLinkPrivate
{
    constexpr float LinkComparisonTolerance = 0.1f;
}

AOmniPointNavLink::AOmniPointNavLink()
{
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
#else
    PrimaryActorTick.bCanEverTick = false;
#endif

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    SourcePoint = CreateDefaultSubobject<USphereComponent>(TEXT("SourcePoint"));
    SourcePoint->SetupAttachment(SceneRoot);
    SourcePoint->SetRelativeLocation(FVector(-200.0, 0.0, 0.0));
    SourcePoint->SetSphereRadius(24.0f);
    SourcePoint->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SourcePoint->SetGenerateOverlapEvents(false);
    SourcePoint->SetCanEverAffectNavigation(false);
    SourcePoint->ShapeColor = FColor(70, 200, 255);
    SourcePoint->SetLineThickness(3.0f);
    SourcePoint->bDrawOnlyIfSelected = true;

    TargetPoint = CreateDefaultSubobject<USphereComponent>(TEXT("TargetPoint"));
    TargetPoint->SetupAttachment(SceneRoot);
    TargetPoint->SetRelativeLocation(FVector(200.0, 0.0, 0.0));
    TargetPoint->SetSphereRadius(24.0f);
    TargetPoint->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TargetPoint->SetGenerateOverlapEvents(false);
    TargetPoint->SetCanEverAffectNavigation(false);
    TargetPoint->ShapeColor = FColor(255, 160, 60);
    TargetPoint->SetLineThickness(3.0f);
    TargetPoint->bDrawOnlyIfSelected = true;

    GeneratedLinks = CreateDefaultSubobject<UNavLinkComponent>(TEXT("GeneratedLinks"));
    GeneratedLinks->SetupAttachment(SceneRoot);
    GeneratedLinks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GeneratedLinks->SetGenerateOverlapEvents(false);

    JumpTraversal = CreateDefaultSubobject<UOmniJumpTraversalComponent>(TEXT("JumpTraversal"));
    MantleTraversal = CreateDefaultSubobject<UOmniMantleTraversalComponent>(TEXT("MantleTraversal"));
    CustomTraversal = CreateDefaultSubobject<UOmniCustomTraversalComponent>(TEXT("CustomTraversal"));

    AreaClass = UNavArea_Default::StaticClass();
    JumpAreaClass = UOmniNavArea_Jump::StaticClass();
    MantleAreaClass = UOmniNavArea_Mantle::StaticClass();
    SupportedAgents = FNavAgentSelector(FNavAgentSelector::AllAgentsMask);
    GenerationStatus = FText::FromString(TEXT("Place Source Point and Target Point over navigable regions."));
}

void AOmniPointNavLink::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    ApplyEditorVisualStyle();
    if (bAutoRegenerate)
    {
        RegenerateLink();
    }
}

void AOmniPointNavLink::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    BindNavigationGenerationDelegate();
    RegenerateLink();
}

void AOmniPointNavLink::PostUnregisterAllComponents()
{
    UnbindNavigationGenerationDelegate();
    Super::PostUnregisterAllComponents();
}

void AOmniPointNavLink::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
#if WITH_EDITOR
    DrawEditorPreview();
#endif
}

#if WITH_EDITOR
bool AOmniPointNavLink::ShouldTickIfViewportsOnly() const
{
    return true;
}
#endif

void AOmniPointNavLink::RegenerateLink()
{
    if (bIsRegenerating || !GeneratedLinks || !SourcePoint || !TargetPoint)
    {
        return;
    }

    TGuardValue<bool> RegenerationGuard(bIsRegenerating, true);
    ResetDebugState();

    TArray<FNavigationLink> NewNativeLinks;
    TArray<FOmniJumpLinkSpec> NewJumpLinks;
    TArray<FOmniMantleLinkSpec> NewMantleLinks;
    TArray<FOmniCustomLinkSpec> NewCustomLinks;

    if (bEnabled)
    {
        FVector SourceWorld = FVector::ZeroVector;
        FVector TargetWorld = FVector::ZeroVector;

        bSourceOnNavMesh = ProjectEndpoint(SourcePoint->GetComponentLocation(), true, SourceWorld);
        bTargetOnNavMesh = ProjectEndpoint(TargetPoint->GetComponentLocation(), false, TargetWorld);

        if (bSourceOnNavMesh)
        {
            PreviewProjectedSource = SourceWorld;
            bHasPreviewProjectedSource = true;
        }
        if (bTargetOnNavMesh)
        {
            PreviewProjectedTarget = TargetWorld;
            bHasPreviewProjectedTarget = true;
        }

        if (bSourceOnNavMesh && bTargetOnNavMesh)
        {
            GenerateProjectedConnection(SourceWorld, TargetWorld, NewNativeLinks, NewJumpLinks, NewMantleLinks, NewCustomLinks);
        }
    }

    NativeLinkCount = NewNativeLinks.Num();
    JumpLinkCount = NewJumpLinks.Num();
    MantleLinkCount = NewMantleLinks.Num();
    CustomLinkCount = NewCustomLinks.Num();
    GeneratedLinkCount = NativeLinkCount + JumpLinkCount + MantleLinkCount + CustomLinkCount;
    UpdateGenerationStatus();

    if (JumpTraversal)
    {
        if (bEnabled && !NewJumpLinks.IsEmpty())
        {
            JumpTraversal->RebuildLinks(NewJumpLinks, SupportedAgents, JumpAreaClass, JumpApexHeight, JumpLandingTolerance, JumpTraversalTimeout);
        }
        else
        {
            JumpTraversal->ClearLinks();
        }
    }

    if (MantleTraversal)
    {
        if (bEnabled && !NewMantleLinks.IsEmpty())
        {
            MantleTraversal->RebuildLinks(NewMantleLinks, SupportedAgents, MantleAreaClass, MantleDuration, MantleArcHeight, MantleLandingTolerance, MantleTraversalTimeout);
        }
        else
        {
            MantleTraversal->ClearLinks();
        }
    }

    if (CustomTraversal)
    {
        if (bEnabled && !NewCustomLinks.IsEmpty())
        {
            CustomTraversal->RebuildLinks(NewCustomLinks, SupportedAgents, AreaClass, CustomTraversalTimeout);
        }
        else
        {
            CustomTraversal->ClearLinks();
        }
    }

    if (AreLinkArraysEquivalent(GeneratedLinks->Links, NewNativeLinks))
    {
        return;
    }

    GeneratedLinks->Links = MoveTemp(NewNativeLinks);
    GeneratedLinks->UpdateBounds();
    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

void AOmniPointNavLink::SwapSourceAndTarget()
{
    if (!SourcePoint || !TargetPoint)
    {
        return;
    }

    SourcePoint->Modify();
    TargetPoint->Modify();

    const FTransform SourceTransform = SourcePoint->GetRelativeTransform();
    const FTransform TargetTransform = TargetPoint->GetRelativeTransform();
    SourcePoint->SetRelativeTransform(TargetTransform);
    TargetPoint->SetRelativeTransform(SourceTransform);

    ApplyEditorVisualStyle();
    RegenerateLink();
}

void AOmniPointNavLink::ResetPoints()
{
    if (!SourcePoint || !TargetPoint)
    {
        return;
    }

    SourcePoint->Modify();
    TargetPoint->Modify();
    SourcePoint->SetRelativeLocation(FVector(-200.0, 0.0, 0.0));
    SourcePoint->SetRelativeRotation(FRotator::ZeroRotator);
    SourcePoint->SetRelativeScale3D(FVector::OneVector);
    TargetPoint->SetRelativeLocation(FVector(200.0, 0.0, 0.0));
    TargetPoint->SetRelativeRotation(FRotator::ZeroRotator);
    TargetPoint->SetRelativeScale3D(FVector::OneVector);
    ApplyEditorVisualStyle();
    RegenerateLink();
}

bool AOmniPointNavLink::ProjectEndpoint(const FVector& CandidateWorld, bool bSource, FVector& OutProjectedWorld)
{
    UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld());
    if (!NavSystem)
    {
        if (bSource)
        {
            ++RejectedSourceProjectionCount;
        }
        else
        {
            ++RejectedTargetProjectionCount;
        }
        return false;
    }

    const FVector SafeExtent(
        FMath::Max(0.0, ProjectionExtent.X),
        FMath::Max(0.0, ProjectionExtent.Y),
        FMath::Max(0.0, ProjectionExtent.Z));

    FNavLocation ProjectedLocation;
    const bool bProjected = NavSystem->ProjectPointToNavigation(
        CandidateWorld,
        ProjectedLocation,
        SafeExtent,
        static_cast<const FNavAgentProperties*>(nullptr),
        FSharedConstNavQueryFilter());

    if (!bProjected)
    {
        if (bSource)
        {
            ++RejectedSourceProjectionCount;
        }
        else
        {
            ++RejectedTargetProjectionCount;
        }
        return false;
    }

    if (MaximumProjectionDistance > 0.0f)
    {
        const double MaxProjectionDistanceSq = FMath::Square(static_cast<double>(MaximumProjectionDistance));
        if (FVector::DistSquared(CandidateWorld, ProjectedLocation.Location) > MaxProjectionDistanceSq)
        {
            if (bSource)
            {
                ++RejectedSourceProjectionCount;
            }
            else
            {
                ++RejectedTargetProjectionCount;
            }
            return false;
        }
    }

    OutProjectedWorld = ProjectedLocation.Location;
    return true;
}

bool AOmniPointNavLink::GenerateProjectedConnection(const FVector& SourceWorld, const FVector& TargetWorld, TArray<FNavigationLink>& OutNativeLinks, TArray<FOmniJumpLinkSpec>& OutJumpLinks, TArray<FOmniMantleLinkSpec>& OutMantleLinks, TArray<FOmniCustomLinkSpec>& OutCustomLinks)
{
    if (MaximumLinkDistance > 0.0f)
    {
        const double MaxLinkDistanceSq = FMath::Square(static_cast<double>(MaximumLinkDistance));
        if (FVector::DistSquared(SourceWorld, TargetWorld) > MaxLinkDistanceSq)
        {
            ++RejectedDistanceCount;
            return false;
        }
    }

    ENavLinkDirection::Type PairDirection = GetNativeDirection();
    bool bHeightRestricted = false;
    if (!ResolvePairDirection(SourceWorld, TargetWorld, PairDirection, bHeightRestricted))
    {
        return false;
    }

    if (bHeightRestricted)
    {
        ++HeightRestrictedLinkCount;
    }

    UClass* ResolvedAreaClass = AreaClass.Get();
    if (!ResolvedAreaClass)
    {
        ResolvedAreaClass = UNavArea_Default::StaticClass();
    }

    const FTransform LinkTransform = GeneratedLinks->GetComponentTransform();

    auto EmitRawLink = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseMantle, bool bUseCustom) -> bool
    {
        if (bUseCustom)
        {
            FOmniCustomLinkSpec& CustomSpec = OutCustomLinks.Emplace_GetRef();
            CustomSpec.SourceWorld = SourceWorld;
            CustomSpec.TargetWorld = TargetWorld;
            CustomSpec.Direction = Direction;
            CustomSpec.SourceToTargetTraversalId = bUseDirectionalTraversalActions
                ? SourceToTargetCustomTraversalId
                : CustomTraversalId;
            CustomSpec.TargetToSourceTraversalId = bUseDirectionalTraversalActions
                ? TargetToSourceCustomTraversalId
                : CustomTraversalId;
        }
        else if (bUseMantle)
        {
            FOmniMantleLinkSpec& MantleSpec = OutMantleLinks.Emplace_GetRef();
            MantleSpec.SourceWorld = SourceWorld;
            MantleSpec.TargetWorld = TargetWorld;
            MantleSpec.Direction = Direction;
        }
        else if (bUseJump)
        {
            FOmniJumpLinkSpec& JumpSpec = OutJumpLinks.Emplace_GetRef();
            JumpSpec.SourceWorld = SourceWorld;
            JumpSpec.TargetWorld = TargetWorld;
            JumpSpec.Direction = Direction;
        }
        else
        {
            const FVector SourceLocal = LinkTransform.InverseTransformPosition(SourceWorld);
            const FVector TargetLocal = LinkTransform.InverseTransformPosition(TargetWorld);

            FNavigationLink& NewLink = OutNativeLinks.Emplace_GetRef(SourceLocal, TargetLocal);
            NewLink.Direction = Direction;
            NewLink.SnapRadius = FMath::Max(1.0f, SnapRadius);
            NewLink.LeftProjectHeight = FMath::Max(0.0f, NativeProjectionHeight);
            NewLink.MaxFallDownLength = FMath::Max(0.0f, NativeProjectionHeight);
            NewLink.bUseSnapHeight = false;
            NewLink.bSnapToCheapestArea = false;
            NewLink.SupportedAgents = SupportedAgents;
            NewLink.SetAreaClass(ResolvedAreaClass);
        }

        FPreviewLink& Preview = PreviewGeneratedLinks.Emplace_GetRef();
        Preview.SourceWorld = SourceWorld;
        Preview.TargetWorld = TargetWorld;
        Preview.Direction = Direction;
        Preview.bUsesJumpTraversal = bUseJump;
        Preview.bUsesMantleTraversal = bUseMantle;
        Preview.bUsesCustomTraversal = bUseCustom;
        return true;
    };

    auto RecordRejectedJump = [&](ENavLinkDirection::Type Direction, int32 FailureCode, bool bTooFast)
    {
        if (bTooFast)
        {
            ++RejectedJumpSpeedCount;
        }
        else
        {
            switch (FailureCode)
            {
            case 1: ++RejectedJumpDepartureSupportCount; break;
            case 2: ++RejectedJumpDepartureClearanceCount; break;
            case 3: ++RejectedJumpPathCount; break;
            case 4: ++RejectedJumpLandingSupportCount; break;
            case 5: ++RejectedJumpLandingClearanceCount; break;
            default: ++RejectedJumpInvalidCount; break;
            }
        }

        FPreviewLink& Preview = PreviewRejectedJumpLinks.Emplace_GetRef();
        Preview.SourceWorld = SourceWorld;
        Preview.TargetWorld = TargetWorld;
        Preview.Direction = Direction;
        Preview.bUsesJumpTraversal = true;
        Preview.bUsesMantleTraversal = false;
        Preview.bUsesCustomTraversal = false;
        Preview.ValidationFailureCode = static_cast<uint8>(FailureCode);
        Preview.bValidationSpeedFailure = bTooFast;
    };

    auto ValidateJumpDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? TargetWorld : SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? SourceWorld : TargetWorld;
        int32 FailureCode = 0;
        bool bTooFast = false;
        if (IsJumpDirectionValid(StartWorld, EndWorld, FailureCode, bTooFast))
        {
            return true;
        }

        RecordRejectedJump(Direction, FailureCode, bTooFast);
        return false;
    };

    auto RecordRejectedMantle = [&](ENavLinkDirection::Type Direction, int32 FailureCode)
    {
        switch (FailureCode)
        {
        case 1: ++RejectedMantleDepartureSupportCount; break;
        case 2: ++RejectedMantleDepartureClearanceCount; break;
        case 3: ++RejectedMantlePathCount; break;
        case 4: ++RejectedMantleLandingSupportCount; break;
        case 5: ++RejectedMantleLandingClearanceCount; break;
        default: ++RejectedMantleInvalidCount; break;
        }

        FPreviewLink& Preview = PreviewRejectedMantleLinks.Emplace_GetRef();
        Preview.SourceWorld = SourceWorld;
        Preview.TargetWorld = TargetWorld;
        Preview.Direction = Direction;
        Preview.bUsesJumpTraversal = false;
        Preview.bUsesMantleTraversal = true;
        Preview.bUsesCustomTraversal = false;
        Preview.ValidationFailureCode = static_cast<uint8>(FailureCode);
        Preview.bValidationSpeedFailure = false;
    };

    auto ValidateMantleDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? TargetWorld : SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? SourceWorld : TargetWorld;
        int32 FailureCode = 0;
        if (IsMantleDirectionValid(StartWorld, EndWorld, FailureCode))
        {
            return true;
        }

        RecordRejectedMantle(Direction, FailureCode);
        return false;
    };

    auto EmitLink = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseMantle, bool bUseCustom) -> bool
    {
        if (bUseCustom)
        {
            return EmitRawLink(Direction, false, false, true);
        }

        if (bUseMantle)
        {
            if (!bValidateMantlePath)
            {
                return EmitRawLink(Direction, false, true, false);
            }

            if (Direction == ENavLinkDirection::BothWays)
            {
                const bool bForwardValid = ValidateMantleDirection(ENavLinkDirection::LeftToRight);
                const bool bReverseValid = ValidateMantleDirection(ENavLinkDirection::RightToLeft);

                if (bForwardValid && bReverseValid)
                {
                    return EmitRawLink(ENavLinkDirection::BothWays, false, true, false);
                }
                if (bForwardValid)
                {
                    ++MantleRestrictedToOneWayCount;
                    return EmitRawLink(ENavLinkDirection::LeftToRight, false, true, false);
                }
                if (bReverseValid)
                {
                    ++MantleRestrictedToOneWayCount;
                    return EmitRawLink(ENavLinkDirection::RightToLeft, false, true, false);
                }
                return false;
            }

            if (!ValidateMantleDirection(Direction))
            {
                return false;
            }

            return EmitRawLink(Direction, false, true, false);
        }

        if (!bUseJump || !bValidateJumpPath)
        {
            return EmitRawLink(Direction, bUseJump, false, false);
        }

        if (Direction == ENavLinkDirection::BothWays)
        {
            const bool bForwardValid = ValidateJumpDirection(ENavLinkDirection::LeftToRight);
            const bool bReverseValid = ValidateJumpDirection(ENavLinkDirection::RightToLeft);

            if (bForwardValid && bReverseValid)
            {
                return EmitRawLink(ENavLinkDirection::BothWays, true, false, false);
            }
            if (bForwardValid)
            {
                ++JumpRestrictedToOneWayCount;
                return EmitRawLink(ENavLinkDirection::LeftToRight, true, false, false);
            }
            if (bReverseValid)
            {
                ++JumpRestrictedToOneWayCount;
                return EmitRawLink(ENavLinkDirection::RightToLeft, true, false, false);
            }
            return false;
        }

        if (!ValidateJumpDirection(Direction))
        {
            return false;
        }

        return EmitRawLink(Direction, true, false, false);
    };

    FOmniAutomaticTraversalSettings AutomaticSettings;
    AutomaticSettings.RiseThreshold = AutomaticRiseThreshold;
    AutomaticSettings.NativeDropMaxDistance = AutomaticNativeDropMaxDistance;
    AutomaticSettings.GroundProbeDepth = AutomaticGroundProbeDepth;
    AutomaticSettings.GroundProbeSpacing = AutomaticGroundProbeSpacing;
    AutomaticSettings.bValidateNativePath = bValidateAutomaticNativePath;
    AutomaticSettings.MaxStepHeight = AutomaticMaxStepHeight;
    AutomaticSettings.MaxGroundSlopeDegrees = AutomaticMaxGroundSlopeDegrees;
    AutomaticSettings.AgentRadius = TraversalAgentRadius;
    AutomaticSettings.AgentHalfHeight = TraversalAgentHalfHeight;
    AutomaticSettings.bAllowAutomaticMantle = bAutomaticUseMantle;
    AutomaticSettings.bRequireMantleLedge = bAutomaticRequireMantleLedge;
    AutomaticSettings.MantleMaxHeight = AutomaticMantleMaxHeight;
    AutomaticSettings.MantleMaxReach = AutomaticMantleMaxReach;
    AutomaticSettings.bValidateMantlePath = bValidateMantlePath;
    AutomaticSettings.MantleArcHeight = MantleArcHeight;
    AutomaticSettings.MantleValidationSegments = MantleValidationSegments;

    const FOmniTraversalPlan TraversalPlan = OmniTraversal::BuildDirectionalTraversalPlan(
        GetWorld(),
        this,
        SourceWorld,
        TargetWorld,
        PairDirection,
        TraversalAction,
        bUseDirectionalTraversalActions,
        SourceToTargetTraversalAction,
        TargetToSourceTraversalAction,
        AutomaticSettings);

    if (UsesTraversalAction(EOmniTraversalAction::Automatic))
    {
        auto RecordAutomaticDiagnostic = [&](const FOmniAutomaticDirectionDiagnostic& Diagnostic, ENavLinkDirection::Type Direction)
        {
            if (!Diagnostic.bEvaluated)
            {
                return;
            }

            if (Diagnostic.bMantleValidationFallback)
            {
                ++AutomaticMantleValidationFallbackCount;
                const FVector MantleStartWorld = Direction == ENavLinkDirection::RightToLeft ? TargetWorld : SourceWorld;
                const FVector MantleEndWorld = Direction == ENavLinkDirection::RightToLeft ? SourceWorld : TargetWorld;
                int32 FailureCode = 0;
                IsMantleDirectionValid(MantleStartWorld, MantleEndWorld, FailureCode);
                RecordRejectedMantle(Direction, FailureCode);
            }

            if (Diagnostic.bMantleLedgeFallback)
            {
                ++AutomaticMantleLedgeFallbackCount;
            }

            if (Diagnostic.bUseMantle)
            {
                ++AutomaticMantlePromotionCount;
                return;
            }

            if (!Diagnostic.bUseJump)
            {
                return;
            }

            switch (Diagnostic.GroundResult)
            {
            case EOmniGroundTraversalResult::StepTooHigh:
                ++AutomaticStepPromotionCount;
                break;
            case EOmniGroundTraversalResult::SlopeTooSteep:
                ++AutomaticSlopePromotionCount;
                break;
            case EOmniGroundTraversalResult::Blocked:
                ++AutomaticBlockedPromotionCount;
                break;
            default:
                break;
            }
        };

        RecordAutomaticDiagnostic(TraversalPlan.SourceToTargetAutomatic, ENavLinkDirection::LeftToRight);
        RecordAutomaticDiagnostic(TraversalPlan.TargetToSourceAutomatic, ENavLinkDirection::RightToLeft);
    }

    bool bAnyAdded = false;
    int32 AddedLegCount = 0;
    for (int32 LegIndex = 0; LegIndex < TraversalPlan.LegCount; ++LegIndex)
    {
        const FOmniTraversalLeg& Leg = TraversalPlan.Legs[LegIndex];
        const bool bAdded = EmitLink(Leg.Direction, Leg.bUseJump, Leg.bUseMantle, Leg.bUseCustom);
        if (bAdded && UsesTraversalAction(EOmniTraversalAction::Automatic) && PreviewGeneratedLinks.Num() > 0)
        {
            auto IsMantleFallback = [](const FOmniAutomaticDirectionDiagnostic& Diagnostic)
            {
                return Diagnostic.bEvaluated && (Diagnostic.bMantleValidationFallback || Diagnostic.bMantleLedgeFallback);
            };

            bool bAutomaticMantleFallback = false;
            if (Leg.Direction == ENavLinkDirection::LeftToRight)
            {
                bAutomaticMantleFallback = IsMantleFallback(TraversalPlan.SourceToTargetAutomatic);
            }
            else if (Leg.Direction == ENavLinkDirection::RightToLeft)
            {
                bAutomaticMantleFallback = IsMantleFallback(TraversalPlan.TargetToSourceAutomatic);
            }
            else
            {
                bAutomaticMantleFallback = IsMantleFallback(TraversalPlan.SourceToTargetAutomatic) || IsMantleFallback(TraversalPlan.TargetToSourceAutomatic);
            }

            PreviewGeneratedLinks.Last().bAutomaticMantleFallback = bAutomaticMantleFallback;
        }
        bAnyAdded |= bAdded;
        AddedLegCount += bAdded ? 1 : 0;
    }

    if (TraversalPlan.bAutomaticSplit && AddedLegCount == TraversalPlan.LegCount)
    {
        ++AutomaticSplitPairCount;
    }

    if (TraversalPlan.bDirectionalSplit && AddedLegCount == TraversalPlan.LegCount)
    {
        ++DirectionalSplitPairCount;
    }

    return bAnyAdded;
}

bool AOmniPointNavLink::IsTraversalAllowedByHeight(const FVector& StartWorld, const FVector& EndWorld) const
{
    if (!bUseHeightLimits)
    {
        return true;
    }

    const double DeltaZ = EndWorld.Z - StartWorld.Z;
    if (DeltaZ > 0.0)
    {
        return DeltaZ <= static_cast<double>(FMath::Max(0.0f, MaximumRise)) + KINDA_SMALL_NUMBER;
    }

    return -DeltaZ <= static_cast<double>(FMath::Max(0.0f, MaximumDrop)) + KINDA_SMALL_NUMBER;
}

bool AOmniPointNavLink::ResolvePairDirection(const FVector& SourceWorld, const FVector& TargetWorld, ENavLinkDirection::Type& OutDirection, bool& bOutHeightRestricted) const
{
    bOutHeightRestricted = false;

    if (!bUseHeightLimits)
    {
        OutDirection = GetNativeDirection();
        return true;
    }

    const bool bSourceToTargetAllowed = IsTraversalAllowedByHeight(SourceWorld, TargetWorld);
    const bool bTargetToSourceAllowed = IsTraversalAllowedByHeight(TargetWorld, SourceWorld);

    switch (LinkDirection)
    {
    case EOmniPointLinkDirection::SourceToTarget:
        OutDirection = ENavLinkDirection::LeftToRight;
        return bSourceToTargetAllowed;
    case EOmniPointLinkDirection::TargetToSource:
        OutDirection = ENavLinkDirection::RightToLeft;
        return bTargetToSourceAllowed;
    case EOmniPointLinkDirection::BothWays:
    default:
        if (bSourceToTargetAllowed && bTargetToSourceAllowed)
        {
            OutDirection = ENavLinkDirection::BothWays;
            return true;
        }
        if (bSourceToTargetAllowed)
        {
            OutDirection = ENavLinkDirection::LeftToRight;
            bOutHeightRestricted = true;
            return true;
        }
        if (bTargetToSourceAllowed)
        {
            OutDirection = ENavLinkDirection::RightToLeft;
            bOutHeightRestricted = true;
            return true;
        }
        return false;
    }
}

bool AOmniPointNavLink::IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode, bool& bOutTooFast) const
{
    OutFailureCode = 0;
    bOutTooFast = false;

    FOmniJumpValidationSettings Settings;
    Settings.bValidatePath = bValidateJumpPath;
    Settings.ApexHeight = JumpApexHeight;
    Settings.AgentRadius = TraversalAgentRadius;
    Settings.AgentHalfHeight = TraversalAgentHalfHeight;
    Settings.MaxHorizontalSpeed = MaximumJumpHorizontalSpeed;
    Settings.ArcSegments = JumpValidationSegments;

    const EOmniJumpValidationResult Result = OmniTraversal::ValidateJumpPath(GetWorld(), this, StartWorld, EndWorld, Settings);
    if (Result == EOmniJumpValidationResult::Valid)
    {
        return true;
    }

    bOutTooFast = Result == EOmniJumpValidationResult::TooFast;
    switch (Result)
    {
    case EOmniJumpValidationResult::DepartureSupport: OutFailureCode = 1; break;
    case EOmniJumpValidationResult::DepartureClearance: OutFailureCode = 2; break;
    case EOmniJumpValidationResult::PathBlocked: OutFailureCode = 3; break;
    case EOmniJumpValidationResult::LandingSupport: OutFailureCode = 4; break;
    case EOmniJumpValidationResult::LandingClearance: OutFailureCode = 5; break;
    case EOmniJumpValidationResult::Invalid: OutFailureCode = 6; break;
    default: break;
    }
    return false;
}

bool AOmniPointNavLink::IsMantleDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode) const
{
    OutFailureCode = 0;

    FOmniMantleValidationSettings Settings;
    Settings.bValidatePath = bValidateMantlePath;
    Settings.ArcHeight = MantleArcHeight;
    Settings.AgentRadius = TraversalAgentRadius;
    Settings.AgentHalfHeight = TraversalAgentHalfHeight;
    Settings.PathSegments = MantleValidationSegments;

    const EOmniMantleValidationResult Result = OmniTraversal::ValidateMantlePath(GetWorld(), this, StartWorld, EndWorld, Settings);
    if (Result == EOmniMantleValidationResult::Valid)
    {
        return true;
    }

    switch (Result)
    {
    case EOmniMantleValidationResult::DepartureSupport: OutFailureCode = 1; break;
    case EOmniMantleValidationResult::DepartureClearance: OutFailureCode = 2; break;
    case EOmniMantleValidationResult::PathBlocked: OutFailureCode = 3; break;
    case EOmniMantleValidationResult::LandingSupport: OutFailureCode = 4; break;
    case EOmniMantleValidationResult::LandingClearance: OutFailureCode = 5; break;
    case EOmniMantleValidationResult::Invalid: OutFailureCode = 6; break;
    default: break;
    }
    return false;
}

bool AOmniPointNavLink::AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const
{
    if (A.Num() != B.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const FNavigationLink& LinkA = A[Index];
        const FNavigationLink& LinkB = B[Index];
        if (!LinkA.Left.Equals(LinkB.Left, OmniPointNavLinkPrivate::LinkComparisonTolerance)
            || !LinkA.Right.Equals(LinkB.Right, OmniPointNavLinkPrivate::LinkComparisonTolerance)
            || LinkA.Direction != LinkB.Direction
            || !FMath::IsNearlyEqual(LinkA.SnapRadius, LinkB.SnapRadius, OmniPointNavLinkPrivate::LinkComparisonTolerance)
            || !FMath::IsNearlyEqual(LinkA.LeftProjectHeight, LinkB.LeftProjectHeight, OmniPointNavLinkPrivate::LinkComparisonTolerance)
            || !FMath::IsNearlyEqual(LinkA.MaxFallDownLength, LinkB.MaxFallDownLength, OmniPointNavLinkPrivate::LinkComparisonTolerance)
            || LinkA.bUseSnapHeight != LinkB.bUseSnapHeight
            || LinkA.bSnapToCheapestArea != LinkB.bSnapToCheapestArea
            || !LinkA.SupportedAgents.IsSame(LinkB.SupportedAgents)
            || LinkA.GetAreaClass() != LinkB.GetAreaClass())
        {
            return false;
        }
    }

    return true;
}

ENavLinkDirection::Type AOmniPointNavLink::GetNativeDirection() const
{
    switch (LinkDirection)
    {
    case EOmniPointLinkDirection::SourceToTarget:
        return ENavLinkDirection::LeftToRight;
    case EOmniPointLinkDirection::TargetToSource:
        return ENavLinkDirection::RightToLeft;
    case EOmniPointLinkDirection::BothWays:
    default:
        return ENavLinkDirection::BothWays;
    }
}

bool AOmniPointNavLink::UsesTraversalAction(EOmniTraversalAction Action) const
{
    if (!bUseDirectionalTraversalActions)
    {
        return TraversalAction == Action;
    }

    return SourceToTargetTraversalAction == Action
        || TargetToSourceTraversalAction == Action;
}

void AOmniPointNavLink::UpdateGenerationStatus()
{
    if (!bEnabled)
    {
        GenerationStatus = FText::FromString(TEXT("Disabled. No link is being generated."));
        return;
    }

    if (!bSourceOnNavMesh)
    {
        GenerationStatus = FText::FromString(TEXT("Source Point could not project to NavMesh. Move it closer to a navigable surface or expand Projection Extent under Advanced."));
        return;
    }

    if (!bTargetOnNavMesh)
    {
        GenerationStatus = FText::FromString(TEXT("Target Point could not project to NavMesh. Move it closer to a navigable surface or expand Projection Extent under Advanced."));
        return;
    }

    if (GeneratedLinkCount == 0)
    {
        if ((RejectedJumpDepartureSupportCount + RejectedJumpDepartureClearanceCount + RejectedJumpPathCount + RejectedJumpLandingSupportCount + RejectedJumpLandingClearanceCount + RejectedJumpSpeedCount + RejectedJumpInvalidCount) > 0)
        {
            GenerationStatus = FText::FromString(TEXT("No link generated. Jump validation rejected the candidate direction(s). Clear obstacles, increase Jump Apex Height, or adjust advanced jump validation settings."));
        }
        else if (bUseHeightLimits)
        {
            GenerationStatus = FText::FromString(TEXT("No link generated. The points do not satisfy the current Max Rise / Max Drop limits."));
        }
        else if (RejectedDistanceCount > 0)
        {
            GenerationStatus = FText::FromString(TEXT("No link generated. Source and Target are farther apart than Max Crossing Distance."));
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("No valid link generated. Check Source, Target, and traversal settings."));
        }
        return;
    }

    FString Status = FString::Printf(TEXT("Ready: %d link%s."), GeneratedLinkCount, GeneratedLinkCount == 1 ? TEXT("") : TEXT("s"));

    if (bUseDirectionalTraversalActions)
    {
        Status += FString::Printf(
            TEXT(" Directional traversal: %d native, %d jump, %d mantle, %d custom."),
            NativeLinkCount,
            JumpLinkCount,
            MantleLinkCount,
            CustomLinkCount);

        if (DirectionalSplitPairCount > 0)
        {
            Status += TEXT(" Source to Target and Target to Source use different traversal actions.");
        }

        if (AutomaticSplitPairCount > 0)
        {
            Status += TEXT(" Automatic traversal also split Both Ways because the two directions resolved differently.");
        }

        if (CustomLinkCount > 0)
        {
            Status += TEXT(" Bind CustomTraversal.OnTraversalRequested and call Complete Custom Traversal when custom movement is finished.");
        }

        if (UsesTraversalAction(EOmniTraversalAction::Automatic) && AutomaticMantlePromotionCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Automatic selected Mantle for %d short upward direction%s."),
                AutomaticMantlePromotionCount,
                AutomaticMantlePromotionCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (UsesTraversalAction(EOmniTraversalAction::Automatic) && AutomaticMantleValidationFallbackCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Mantle validation redirected %d Automatic direction%s to Jump."),
                AutomaticMantleValidationFallbackCount,
                AutomaticMantleValidationFallbackCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (UsesTraversalAction(EOmniTraversalAction::Automatic) && AutomaticMantleLedgeFallbackCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Ledge detection redirected %d Automatic direction%s to Jump because no usable mantle ledge geometry was found."),
                AutomaticMantleLedgeFallbackCount,
                AutomaticMantleLedgeFallbackCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (UsesTraversalAction(EOmniTraversalAction::Automatic) && bValidateAutomaticNativePath)
        {
            const int32 NativeValidationPromotions = AutomaticStepPromotionCount + AutomaticSlopePromotionCount + AutomaticBlockedPromotionCount;
            if (NativeValidationPromotions > 0)
            {
                Status += FString::Printf(
                    TEXT(" Native validation promoted %d direction%s to Jump (step %d, slope %d, clearance %d)."),
                    NativeValidationPromotions,
                    NativeValidationPromotions == 1 ? TEXT("") : TEXT("s"),
                    AutomaticStepPromotionCount,
                    AutomaticSlopePromotionCount,
                    AutomaticBlockedPromotionCount);
            }
        }
    }
    else if (TraversalAction == EOmniTraversalAction::Jump)
    {
        Status += FString::Printf(TEXT(" Jump traversal: %d smart link%s, apex %.0f cm."), JumpLinkCount, JumpLinkCount == 1 ? TEXT("") : TEXT("s"), JumpApexHeight);
    }
    else if (TraversalAction == EOmniTraversalAction::Mantle)
    {
        Status += FString::Printf(TEXT(" Mantle traversal: %d smart link%s, duration %.2f s, arc %.0f cm."), MantleLinkCount, MantleLinkCount == 1 ? TEXT("") : TEXT("s"), MantleDuration, MantleArcHeight);
    }
    else if (TraversalAction == EOmniTraversalAction::Custom)
    {
        Status += FString::Printf(TEXT(" Custom traversal: %d smart link%s. Bind CustomTraversal.OnTraversalRequested and call Complete Custom Traversal when movement is finished."), CustomLinkCount, CustomLinkCount == 1 ? TEXT("") : TEXT("s"));
    }
    else if (TraversalAction == EOmniTraversalAction::Automatic)
    {
        Status += FString::Printf(TEXT(" Automatic traversal: %d native, %d mantle, %d jump."), NativeLinkCount, MantleLinkCount, JumpLinkCount);
        if (AutomaticSplitPairCount > 0)
        {
            Status += TEXT(" Both Ways was split because each direction requires a different traversal action.");
        }

        if (AutomaticMantlePromotionCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Automatic selected Mantle for %d short upward direction%s."),
                AutomaticMantlePromotionCount,
                AutomaticMantlePromotionCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (AutomaticMantleValidationFallbackCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Mantle validation redirected %d Automatic direction%s to Jump."),
                AutomaticMantleValidationFallbackCount,
                AutomaticMantleValidationFallbackCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (AutomaticMantleLedgeFallbackCount > 0)
        {
            Status += FString::Printf(
                TEXT(" Ledge detection redirected %d Automatic direction%s to Jump because no usable mantle ledge geometry was found."),
                AutomaticMantleLedgeFallbackCount,
                AutomaticMantleLedgeFallbackCount == 1 ? TEXT("") : TEXT("s"));
        }

        if (bValidateAutomaticNativePath)
        {
            const int32 NativeValidationPromotions = AutomaticStepPromotionCount + AutomaticSlopePromotionCount + AutomaticBlockedPromotionCount;
            if (NativeValidationPromotions > 0)
            {
                Status += FString::Printf(
                    TEXT(" Native validation promoted %d direction%s to Jump (step %d, slope %d, clearance %d)."),
                    NativeValidationPromotions,
                    NativeValidationPromotions == 1 ? TEXT("") : TEXT("s"),
                    AutomaticStepPromotionCount,
                    AutomaticSlopePromotionCount,
                    AutomaticBlockedPromotionCount);
            }
        }
    }

    if (bValidateJumpPath && (UsesTraversalAction(EOmniTraversalAction::Jump) || UsesTraversalAction(EOmniTraversalAction::Automatic)))
    {
        const int32 RejectedJumpDirections = (RejectedJumpDepartureSupportCount + RejectedJumpDepartureClearanceCount + RejectedJumpPathCount + RejectedJumpLandingSupportCount + RejectedJumpLandingClearanceCount + RejectedJumpSpeedCount + RejectedJumpInvalidCount);
        if (RejectedJumpDirections > 0)
        {
            Status += FString::Printf(
                TEXT(" %d jump direction%s rejected (departure support %d, departure clearance %d, path %d, landing support %d, landing clearance %d, speed %d, invalid %d)."),
                RejectedJumpDirections,
                RejectedJumpDirections == 1 ? TEXT(" was") : TEXT("s were"),
                RejectedJumpDepartureSupportCount,
                RejectedJumpDepartureClearanceCount,
                RejectedJumpPathCount,
                RejectedJumpLandingSupportCount,
                RejectedJumpLandingClearanceCount,
                RejectedJumpSpeedCount,
                RejectedJumpInvalidCount);
        }
        if (JumpRestrictedToOneWayCount > 0)
        {
            Status += TEXT(" The jump was reduced to one-way by validation.");
        }
    }

    if (bValidateMantlePath && (UsesTraversalAction(EOmniTraversalAction::Mantle) || UsesTraversalAction(EOmniTraversalAction::Automatic)))
    {
        const int32 RejectedMantleDirections = (RejectedMantleDepartureSupportCount + RejectedMantleDepartureClearanceCount + RejectedMantlePathCount + RejectedMantleLandingSupportCount + RejectedMantleLandingClearanceCount + RejectedMantleInvalidCount);
        if (RejectedMantleDirections > 0)
        {
            Status += FString::Printf(
                TEXT(" %d mantle direction%s rejected (departure support %d, departure clearance %d, path %d, landing support %d, landing clearance %d, invalid %d)."),
                RejectedMantleDirections,
                RejectedMantleDirections == 1 ? TEXT(" was") : TEXT("s were"),
                RejectedMantleDepartureSupportCount,
                RejectedMantleDepartureClearanceCount,
                RejectedMantlePathCount,
                RejectedMantleLandingSupportCount,
                RejectedMantleLandingClearanceCount,
                RejectedMantleInvalidCount);
        }
        if (MantleRestrictedToOneWayCount > 0)
        {
            Status += TEXT(" The mantle was reduced to one-way by validation.");
        }
    }

    if (bUseHeightLimits)
    {
        Status += FString::Printf(TEXT(" Height limits: Rise %.0f cm, Drop %.0f cm."), MaximumRise, MaximumDrop);
        if (HeightRestrictedLinkCount > 0)
        {
            Status += TEXT(" Both Ways was reduced to one-way by height.");
        }
    }

    GenerationStatus = FText::FromString(Status);
}

void AOmniPointNavLink::ApplyEditorVisualStyle()
{
    if (SourcePoint)
    {
        SourcePoint->ShapeColor = FColor(70, 200, 255);
        SourcePoint->SetLineThickness(3.0f);
        SourcePoint->bDrawOnlyIfSelected = true;
        SourcePoint->MarkRenderStateDirty();
    }

    if (TargetPoint)
    {
        TargetPoint->ShapeColor = FColor(255, 160, 60);
        TargetPoint->SetLineThickness(3.0f);
        TargetPoint->bDrawOnlyIfSelected = true;
        TargetPoint->MarkRenderStateDirty();
    }
}

void AOmniPointNavLink::ResetDebugState()
{
    GeneratedLinkCount = 0;
    NativeLinkCount = 0;
    JumpLinkCount = 0;
    MantleLinkCount = 0;
    CustomLinkCount = 0;
    bSourceOnNavMesh = false;
    bTargetOnNavMesh = false;
    RejectedSourceProjectionCount = 0;
    RejectedTargetProjectionCount = 0;
    RejectedDistanceCount = 0;
    HeightRestrictedLinkCount = 0;
    AutomaticSplitPairCount = 0;
    DirectionalSplitPairCount = 0;
    AutomaticStepPromotionCount = 0;
    AutomaticSlopePromotionCount = 0;
    AutomaticBlockedPromotionCount = 0;
    AutomaticMantlePromotionCount = 0;
    AutomaticMantleValidationFallbackCount = 0;
    AutomaticMantleLedgeFallbackCount = 0;
    RejectedJumpDepartureSupportCount = 0;
    RejectedJumpDepartureClearanceCount = 0;
    RejectedJumpPathCount = 0;
    RejectedJumpLandingSupportCount = 0;
    RejectedJumpLandingClearanceCount = 0;
    RejectedJumpSpeedCount = 0;
    RejectedJumpInvalidCount = 0;
    JumpRestrictedToOneWayCount = 0;
    RejectedMantleDepartureSupportCount = 0;
    RejectedMantleDepartureClearanceCount = 0;
    RejectedMantlePathCount = 0;
    RejectedMantleLandingSupportCount = 0;
    RejectedMantleLandingClearanceCount = 0;
    RejectedMantleInvalidCount = 0;
    MantleRestrictedToOneWayCount = 0;
    bHasPreviewProjectedSource = false;
    bHasPreviewProjectedTarget = false;
    PreviewGeneratedLinks.Reset();
    PreviewRejectedJumpLinks.Reset();
    PreviewRejectedMantleLinks.Reset();
}

#if WITH_EDITOR
void AOmniPointNavLink::DrawEditorPreview() const
{
    UWorld* World = GetWorld();
    if (!bShowPreview || !World || World->WorldType != EWorldType::Editor || !IsSelected())
    {
        return;
    }

    const FVector PreviewOffset(0.0, 0.0, 8.0);
    const bool bDrawSamples = PreviewDetail != EOmniPreviewDetail::LinksOnly;
    const bool bDrawDiagnostics = PreviewDetail == EOmniPreviewDetail::Diagnostics;

    auto GetValidationReason = [](uint8 FailureCode, bool bSpeedFailure) -> FString
    {
        if (bSpeedFailure)
        {
            return TEXT("Horizontal Speed");
        }

        switch (FailureCode)
        {
        case 1: return TEXT("Departure Support");
        case 2: return TEXT("Departure Clearance");
        case 3: return TEXT("Path Blocked");
        case 4: return TEXT("Landing Support");
        case 5: return TEXT("Landing Clearance");
        default: return TEXT("Invalid");
        }
    };

    if (bDrawSamples && bHasPreviewProjectedSource)
    {
        OmniPreview::DrawSamplePoint(World, PreviewProjectedSource, OmniPreview::SourceColor(), 14.0f, PreviewOffset);
    }
    if (bDrawSamples && bHasPreviewProjectedTarget)
    {
        OmniPreview::DrawSamplePoint(World, PreviewProjectedTarget, OmniPreview::TargetColor(), 14.0f, PreviewOffset);
    }

    for (const FPreviewLink& Link : PreviewGeneratedLinks)
    {
        if (Link.bUsesCustomTraversal)
        {
            OmniPreview::DrawCustomTraversal(World, Link.SourceWorld, Link.TargetWorld, Link.Direction, 2.0f, PreviewOffset);
        }
        else if (Link.bUsesMantleTraversal)
        {
            OmniPreview::DrawMantleTraversal(World, Link.SourceWorld, Link.TargetWorld, Link.Direction, MantleArcHeight, MantleValidationSegments, 2.0f, PreviewOffset);
        }
        else
        {
            OmniPreview::DrawTraversal(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                Link.bUsesJumpTraversal,
                JumpApexHeight,
                JumpValidationSegments,
                2.0f,
                PreviewOffset);
        }

        if (Link.bAutomaticMantleFallback)
        {
            OmniPreview::DrawFallbackMarker(World, Link.SourceWorld, Link.TargetWorld, PreviewOffset);
        }
    }

    if (bDrawDiagnostics)
    {
        const FColor RejectedColor = OmniPreview::RejectedColor();
        for (const FPreviewLink& Link : PreviewRejectedJumpLinks)
        {
            OmniPreview::DrawTraversal(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                true,
                JumpApexHeight,
                JumpValidationSegments,
                2.0f,
                PreviewOffset,
                &RejectedColor);
            OmniPreview::DrawRejectedReasonLabel(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                GetValidationReason(Link.ValidationFailureCode, Link.bValidationSpeedFailure),
                PreviewOffset);
            OmniPreview::DrawRejectedFailureMarker(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                Link.ValidationFailureCode,
                Link.bValidationSpeedFailure,
                false,
                JumpApexHeight,
                PreviewOffset);
        }

        for (const FPreviewLink& Link : PreviewRejectedMantleLinks)
        {
            OmniPreview::DrawMantleTraversal(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                MantleArcHeight,
                MantleValidationSegments,
                2.0f,
                PreviewOffset,
                &RejectedColor);
            OmniPreview::DrawRejectedReasonLabel(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                GetValidationReason(Link.ValidationFailureCode, false),
                PreviewOffset);
            OmniPreview::DrawRejectedFailureMarker(
                World,
                Link.SourceWorld,
                Link.TargetWorld,
                Link.Direction,
                Link.ValidationFailureCode,
                false,
                true,
                MantleArcHeight,
                PreviewOffset);
        }
    }
}
#endif

void AOmniPointNavLink::NotifyNavigationSystem()
{
    if (!GeneratedLinks || !GeneratedLinks->IsRegistered() || !GeneratedLinks->GetWorld())
    {
        return;
    }

    FNavigationSystem::UpdateComponentData(*GeneratedLinks);
}

void AOmniPointNavLink::BindNavigationGenerationDelegate()
{
    if (!bRegenerateAfterNavigationBuild || !GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.AddUniqueDynamic(this, &AOmniPointNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniPointNavLink::UnbindNavigationGenerationDelegate()
{
    if (!GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.RemoveDynamic(this, &AOmniPointNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniPointNavLink::HandleNavigationGenerationFinished(ANavigationData* NavData)
{
    if (!bRegenerateAfterNavigationBuild || !NavData || NavData->GetWorld() != GetWorld())
    {
        return;
    }

    RegenerateLink();
}

#if WITH_EDITOR
void AOmniPointNavLink::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    UnbindNavigationGenerationDelegate();
    BindNavigationGenerationDelegate();
    ApplyEditorVisualStyle();

    const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    const FName DirectionalActionsName = GET_MEMBER_NAME_CHECKED(AOmniPointNavLink, bUseDirectionalTraversalActions);
    const FName AutoRegenerateName = GET_MEMBER_NAME_CHECKED(AOmniPointNavLink, bAutoRegenerate);

    if (PropertyName == DirectionalActionsName && bUseDirectionalTraversalActions)
    {
        // Enabling directional actions should preserve the actor's current behavior
        // until the designer intentionally changes one of the two directions.
        SourceToTargetTraversalAction = TraversalAction;
        TargetToSourceTraversalAction = TraversalAction;
        if (TraversalAction == EOmniTraversalAction::Custom)
        {
            SourceToTargetCustomTraversalId = CustomTraversalId;
            TargetToSourceCustomTraversalId = CustomTraversalId;
        }
    }

    if (bAutoRegenerate || PropertyName == AutoRegenerateName)
    {
        if (bAutoRegenerate)
        {
            RegenerateLink();
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("Automatic updates are off. Use Regenerate Link after editing the points."));
        }
    }
}

void AOmniPointNavLink::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);
    if (bFinished && bAutoRegenerate)
    {
        RegenerateLink();
    }
}

void AOmniPointNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    ApplyEditorVisualStyle();
    RegenerateLink();
}
#endif
