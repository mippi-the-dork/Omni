#include "OmniPointNavLink.h"

#include "OmniJumpTraversalComponent.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniNavArea_Jump.h"
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
    CustomTraversal = CreateDefaultSubobject<UOmniCustomTraversalComponent>(TEXT("CustomTraversal"));

    AreaClass = UNavArea_Default::StaticClass();
    JumpAreaClass = UOmniNavArea_Jump::StaticClass();
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
            GenerateProjectedConnection(SourceWorld, TargetWorld, NewNativeLinks, NewJumpLinks, NewCustomLinks);
        }
    }

    NativeLinkCount = NewNativeLinks.Num();
    JumpLinkCount = NewJumpLinks.Num();
    CustomLinkCount = NewCustomLinks.Num();
    GeneratedLinkCount = NativeLinkCount + JumpLinkCount + CustomLinkCount;
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

bool AOmniPointNavLink::GenerateProjectedConnection(const FVector& SourceWorld, const FVector& TargetWorld, TArray<FNavigationLink>& OutNativeLinks, TArray<FOmniJumpLinkSpec>& OutJumpLinks, TArray<FOmniCustomLinkSpec>& OutCustomLinks)
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

    auto EmitRawLink = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseCustom) -> bool
    {
        if (bUseCustom)
        {
            FOmniCustomLinkSpec& CustomSpec = OutCustomLinks.Emplace_GetRef();
            CustomSpec.SourceWorld = SourceWorld;
            CustomSpec.TargetWorld = TargetWorld;
            CustomSpec.Direction = Direction;
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
        Preview.bUsesCustomTraversal = bUseCustom;
        return true;
    };

    auto RecordRejectedJump = [&](ENavLinkDirection::Type Direction, bool bBlocked, bool bTooFast)
    {
        if (bBlocked)
        {
            ++RejectedJumpArcCount;
        }
        if (bTooFast)
        {
            ++RejectedJumpSpeedCount;
        }

        FPreviewLink& Preview = PreviewRejectedJumpLinks.Emplace_GetRef();
        Preview.SourceWorld = SourceWorld;
        Preview.TargetWorld = TargetWorld;
        Preview.Direction = Direction;
        Preview.bUsesJumpTraversal = true;
        Preview.bUsesCustomTraversal = false;
    };

    auto ValidateJumpDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? TargetWorld : SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? SourceWorld : TargetWorld;
        bool bBlocked = false;
        bool bTooFast = false;
        if (IsJumpDirectionValid(StartWorld, EndWorld, bBlocked, bTooFast))
        {
            return true;
        }

        RecordRejectedJump(Direction, bBlocked, bTooFast);
        return false;
    };

    auto EmitLink = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseCustom) -> bool
    {
        if (bUseCustom)
        {
            return EmitRawLink(Direction, false, true);
        }

        if (!bUseJump || !bValidateJumpPath)
        {
            return EmitRawLink(Direction, bUseJump, false);
        }

        if (Direction == ENavLinkDirection::BothWays)
        {
            const bool bForwardValid = ValidateJumpDirection(ENavLinkDirection::LeftToRight);
            const bool bReverseValid = ValidateJumpDirection(ENavLinkDirection::RightToLeft);

            if (bForwardValid && bReverseValid)
            {
                return EmitRawLink(ENavLinkDirection::BothWays, true, false);
            }
            if (bForwardValid)
            {
                ++JumpRestrictedToOneWayCount;
                return EmitRawLink(ENavLinkDirection::LeftToRight, true, false);
            }
            if (bReverseValid)
            {
                ++JumpRestrictedToOneWayCount;
                return EmitRawLink(ENavLinkDirection::RightToLeft, true, false);
            }
            return false;
        }

        if (!ValidateJumpDirection(Direction))
        {
            return false;
        }

        return EmitRawLink(Direction, true, false);
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

    const FOmniTraversalPlan TraversalPlan = OmniTraversal::BuildTraversalPlan(
        GetWorld(),
        this,
        SourceWorld,
        TargetWorld,
        PairDirection,
        TraversalAction,
        AutomaticSettings);

    if (TraversalAction == EOmniTraversalAction::Automatic && bValidateAutomaticNativePath)
    {
        auto RecordAutomaticDiagnostic = [&](const FOmniAutomaticDirectionDiagnostic& Diagnostic)
        {
            if (!Diagnostic.bEvaluated || !Diagnostic.bUseJump)
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

        RecordAutomaticDiagnostic(TraversalPlan.SourceToTargetAutomatic);
        RecordAutomaticDiagnostic(TraversalPlan.TargetToSourceAutomatic);
    }

    bool bAnyAdded = false;
    int32 AddedLegCount = 0;
    for (int32 LegIndex = 0; LegIndex < TraversalPlan.LegCount; ++LegIndex)
    {
        const FOmniTraversalLeg& Leg = TraversalPlan.Legs[LegIndex];
        const bool bAdded = EmitLink(Leg.Direction, Leg.bUseJump, Leg.bUseCustom);
        bAnyAdded |= bAdded;
        AddedLegCount += bAdded ? 1 : 0;
    }

    if (TraversalPlan.bAutomaticSplit && AddedLegCount == TraversalPlan.LegCount)
    {
        ++AutomaticSplitPairCount;
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

bool AOmniPointNavLink::IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, bool& bOutBlocked, bool& bOutTooFast) const
{
    bOutBlocked = false;
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
    bOutBlocked = Result == EOmniJumpValidationResult::Blocked || Result == EOmniJumpValidationResult::Invalid;
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
        if (RejectedJumpArcCount > 0 || RejectedJumpSpeedCount > 0)
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

    if (TraversalAction == EOmniTraversalAction::Jump)
    {
        Status += FString::Printf(TEXT(" Jump traversal: %d smart link%s, apex %.0f cm."), JumpLinkCount, JumpLinkCount == 1 ? TEXT("") : TEXT("s"), JumpApexHeight);
    }
    else if (TraversalAction == EOmniTraversalAction::Custom)
    {
        Status += FString::Printf(TEXT(" Custom traversal: %d smart link%s. Bind CustomTraversal.OnTraversalRequested and call Complete Custom Traversal when movement is finished."), CustomLinkCount, CustomLinkCount == 1 ? TEXT("") : TEXT("s"));
    }
    else if (TraversalAction == EOmniTraversalAction::Automatic)
    {
        Status += FString::Printf(TEXT(" Automatic traversal: %d native, %d jump."), NativeLinkCount, JumpLinkCount);
        if (AutomaticSplitPairCount > 0)
        {
            Status += TEXT(" Both Ways was split because each direction requires a different traversal action.");
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

    if (bValidateJumpPath && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic))
    {
        const int32 RejectedJumpDirections = RejectedJumpArcCount + RejectedJumpSpeedCount;
        if (RejectedJumpDirections > 0)
        {
            Status += FString::Printf(TEXT(" %d jump direction%s rejected by path validation."), RejectedJumpDirections, RejectedJumpDirections == 1 ? TEXT(" was") : TEXT("s were"));
        }
        if (JumpRestrictedToOneWayCount > 0)
        {
            Status += TEXT(" The jump was reduced to one-way by validation.");
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
    CustomLinkCount = 0;
    bSourceOnNavMesh = false;
    bTargetOnNavMesh = false;
    RejectedSourceProjectionCount = 0;
    RejectedTargetProjectionCount = 0;
    RejectedDistanceCount = 0;
    HeightRestrictedLinkCount = 0;
    AutomaticSplitPairCount = 0;
    AutomaticStepPromotionCount = 0;
    AutomaticSlopePromotionCount = 0;
    AutomaticBlockedPromotionCount = 0;
    RejectedJumpArcCount = 0;
    RejectedJumpSpeedCount = 0;
    JumpRestrictedToOneWayCount = 0;
    bHasPreviewProjectedSource = false;
    bHasPreviewProjectedTarget = false;
    PreviewGeneratedLinks.Reset();
    PreviewRejectedJumpLinks.Reset();
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
    const FName AutoRegenerateName = GET_MEMBER_NAME_CHECKED(AOmniPointNavLink, bAutoRegenerate);

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
