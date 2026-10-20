#include "OmniVolumeNavLink.h"

#include "OmniJumpTraversalComponent.h"
#include "OmniMantleTraversalComponent.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniNavArea_Jump.h"
#include "OmniNavArea_Mantle.h"
#include "OmniTraversalUtils.h"
#include "OmniPreviewUtils.h"

#include "AI/NavigationSystemBase.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavLinkComponent.h"

#if WITH_EDITOR
#include "Engine/World.h"
#endif

namespace OmniVolumeNavLinkPrivate
{
    constexpr float LinkComparisonTolerance = 0.1f;
}

AOmniVolumeNavLink::AOmniVolumeNavLink()
{
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
#else
    PrimaryActorTick.bCanEverTick = false;
#endif

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    SourceBox = CreateDefaultSubobject<UBoxComponent>(TEXT("SourceBox"));
    SourceBox->SetupAttachment(SceneRoot);
    SourceBox->SetRelativeLocation(FVector(-300.0, 0.0, 0.0));
    SourceBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));
    SourceBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SourceBox->SetGenerateOverlapEvents(false);
    SourceBox->SetCanEverAffectNavigation(false);
    SourceBox->ShapeColor = FColor(70, 200, 255);
    SourceBox->SetLineThickness(3.0f);
    SourceBox->bDrawOnlyIfSelected = true;

    TargetBox = CreateDefaultSubobject<UBoxComponent>(TEXT("TargetBox"));
    TargetBox->SetupAttachment(SceneRoot);
    TargetBox->SetRelativeLocation(FVector(300.0, 0.0, 0.0));
    TargetBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));
    TargetBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TargetBox->SetGenerateOverlapEvents(false);
    TargetBox->SetCanEverAffectNavigation(false);
    TargetBox->ShapeColor = FColor(255, 160, 60);
    TargetBox->SetLineThickness(3.0f);
    TargetBox->bDrawOnlyIfSelected = true;

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
    GenerationStatus = FText::FromString(TEXT("Place Source and Target over navigable regions."));
}

void AOmniVolumeNavLink::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    ApplyEditorVisualStyle();
    if (bAutoRegenerate)
    {
        RegenerateLinks();
    }
}

void AOmniVolumeNavLink::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    BindNavigationGenerationDelegate();
    RegenerateLinks();
}

void AOmniVolumeNavLink::PostUnregisterAllComponents()
{
    UnbindNavigationGenerationDelegate();
    Super::PostUnregisterAllComponents();
}

void AOmniVolumeNavLink::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

#if WITH_EDITOR
    DrawEditorPreview();
#endif
}

#if WITH_EDITOR
bool AOmniVolumeNavLink::ShouldTickIfViewportsOnly() const
{
    return true;
}
#endif

void AOmniVolumeNavLink::RegenerateLinks()
{
    if (bIsRegenerating || !GeneratedLinks || !SourceBox || !TargetBox)
    {
        return;
    }

    TGuardValue<bool> RegenerationGuard(bIsRegenerating, true);
    ResetDebugCounters();

    TArray<FNavigationLink> NewLinks;
    TArray<FOmniJumpLinkSpec> NewJumpLinks;
    TArray<FOmniMantleLinkSpec> NewMantleLinks;
    TArray<FOmniCustomLinkSpec> NewCustomLinks;

    if (bEnabled)
    {
        const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
        NewLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));
        NewJumpLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));
        NewMantleLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));
        NewCustomLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));

        TArray<FProjectedGridSample> SourceSamples;
        TArray<FProjectedGridSample> TargetSamples;
        BuildProjectedGrid(*SourceBox, true, SourceSamples);
        BuildProjectedGrid(*TargetBox, false, TargetSamples);

        ValidSourceSampleCount = SourceSamples.Num();
        ValidTargetSampleCount = TargetSamples.Num();

        if (!SourceSamples.IsEmpty() && !TargetSamples.IsEmpty())
        {
            TArray<FProjectedPair> CandidatePairs;
            CandidatePairs.Reserve(SourceSamples.Num() + TargetSamples.Num());

            if (PairingMode == EOmniPairingMode::MatchedGrid)
            {
                BuildMatchedGridPairs(SourceSamples, TargetSamples, CandidatePairs);
            }
            else
            {
                BuildNearestPairs(SourceSamples, TargetSamples, CandidatePairs);
            }

            auto GeneratedCount = [&NewLinks, &NewJumpLinks, &NewMantleLinks, &NewCustomLinks]()
            {
                return NewLinks.Num() + NewJumpLinks.Num() + NewMantleLinks.Num() + NewCustomLinks.Num();
            };

            TBitArray<> AttemptedPairs(false, CandidatePairs.Num());
            const int32 SpreadAttemptCount = FMath::Min(SafeMaximumLinks, CandidatePairs.Num());
            for (int32 AttemptIndex = 0; AttemptIndex < SpreadAttemptCount && GeneratedCount() < SafeMaximumLinks; ++AttemptIndex)
            {
                const int32 PairIndex = SpreadAttemptCount <= 1
                    ? CandidatePairs.Num() / 2
                    : FMath::RoundToInt(
                        static_cast<double>(AttemptIndex) * static_cast<double>(CandidatePairs.Num() - 1)
                        / static_cast<double>(SpreadAttemptCount - 1));

                AttemptedPairs[PairIndex] = true;
                AddProjectedPair(CandidatePairs[PairIndex], NewLinks, NewJumpLinks, NewMantleLinks, NewCustomLinks);
            }

            for (int32 PairIndex = 0; PairIndex < CandidatePairs.Num() && GeneratedCount() < SafeMaximumLinks; ++PairIndex)
            {
                if (!AttemptedPairs[PairIndex])
                {
                    AddProjectedPair(CandidatePairs[PairIndex], NewLinks, NewJumpLinks, NewMantleLinks, NewCustomLinks);
                }
            }
        }
    }

    NativeLinkCount = NewLinks.Num();
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

    if (AreLinkArraysEquivalent(GeneratedLinks->Links, NewLinks))
    {
        return;
    }

    GeneratedLinks->Links = MoveTemp(NewLinks);
    GeneratedLinks->UpdateBounds();
    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

void AOmniVolumeNavLink::SwapSourceAndTarget()
{
    if (!SourceBox || !TargetBox)
    {
        return;
    }

    SourceBox->Modify();
    TargetBox->Modify();

    const FTransform SourceTransform = SourceBox->GetRelativeTransform();
    const FVector SourceExtent = SourceBox->GetUnscaledBoxExtent();
    const FTransform TargetTransform = TargetBox->GetRelativeTransform();
    const FVector TargetExtent = TargetBox->GetUnscaledBoxExtent();

    SourceBox->SetRelativeTransform(TargetTransform);
    SourceBox->SetBoxExtent(TargetExtent);
    TargetBox->SetRelativeTransform(SourceTransform);
    TargetBox->SetBoxExtent(SourceExtent);

    ApplyEditorVisualStyle();
    RegenerateLinks();
}

void AOmniVolumeNavLink::MatchTargetSizeToSource()
{
    if (!SourceBox || !TargetBox)
    {
        return;
    }

    TargetBox->Modify();
    TargetBox->SetBoxExtent(SourceBox->GetUnscaledBoxExtent());
    TargetBox->SetRelativeScale3D(SourceBox->GetRelativeScale3D());
    ApplyEditorVisualStyle();
    RegenerateLinks();
}

void AOmniVolumeNavLink::ResetVolumes()
{
    if (!SourceBox || !TargetBox)
    {
        return;
    }

    SourceBox->Modify();
    TargetBox->Modify();

    SourceBox->SetRelativeTransform(FTransform(FQuat::Identity, FVector(-300.0, 0.0, 0.0), FVector::OneVector));
    SourceBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));

    TargetBox->SetRelativeTransform(FTransform(FQuat::Identity, FVector(300.0, 0.0, 0.0), FVector::OneVector));
    TargetBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));

    ApplyEditorVisualStyle();
    RegenerateLinks();
}

bool AOmniVolumeNavLink::BuildProjectedGrid(const UBoxComponent& Box, bool bSourceVolume, TArray<FProjectedGridSample>& OutSamples)
{
    OutSamples.Reset();

    const FVector Extent = Box.GetUnscaledBoxExtent();
    const FTransform BoxTransform = Box.GetComponentTransform();
    const FVector Scale = BoxTransform.GetScale3D();

    const double WorldLengthX = 2.0 * Extent.X * FMath::Abs(Scale.X);
    const double WorldLengthY = 2.0 * Extent.Y * FMath::Abs(Scale.Y);
    const double SafeSpacing = FMath::Max(10.0f, GetEffectiveLinkSpacing());

    int32 SampleCountX = CalculateAxisSampleCount(WorldLengthX, SafeSpacing);
    int32 SampleCountY = CalculateAxisSampleCount(WorldLengthY, SafeSpacing);

    const int32 SafeMaximumSamples = FMath::Clamp(MaximumGridSamplesPerVolume, 1, 4096);
    const int64 InitialSampleCount = static_cast<int64>(SampleCountX) * static_cast<int64>(SampleCountY);

    if (InitialSampleCount > SafeMaximumSamples)
    {
        bGridSampleCapReached = true;
        const double ScaleFactor = FMath::Sqrt(static_cast<double>(InitialSampleCount) / static_cast<double>(SafeMaximumSamples));
        const double AdjustedSpacing = SafeSpacing * ScaleFactor;
        SampleCountX = CalculateAxisSampleCount(WorldLengthX, AdjustedSpacing);
        SampleCountY = CalculateAxisSampleCount(WorldLengthY, AdjustedSpacing);

        while (static_cast<int64>(SampleCountX) * static_cast<int64>(SampleCountY) > SafeMaximumSamples)
        {
            if (SampleCountX >= SampleCountY && SampleCountX > 1)
            {
                --SampleCountX;
            }
            else if (SampleCountY > 1)
            {
                --SampleCountY;
            }
            else
            {
                break;
            }
        }
    }

    const int32 RawGridCount = SampleCountX * SampleCountY;
    if (bSourceVolume)
    {
        SourceGridCandidateCount = RawGridCount;
    }
    else
    {
        TargetGridCandidateCount = RawGridCount;
    }

    OutSamples.Reserve(RawGridCount);

    for (int32 YIndex = 0; YIndex < SampleCountY; ++YIndex)
    {
        const double YAlpha = SampleCountY <= 1
            ? 0.5
            : static_cast<double>(YIndex) / static_cast<double>(SampleCountY - 1);
        const double LocalY = FMath::Lerp(-Extent.Y, Extent.Y, YAlpha);

        for (int32 XIndex = 0; XIndex < SampleCountX; ++XIndex)
        {
            const double XAlpha = SampleCountX <= 1
                ? 0.5
                : static_cast<double>(XIndex) / static_cast<double>(SampleCountX - 1);
            const double LocalX = FMath::Lerp(-Extent.X, Extent.X, XAlpha);

            const FVector CandidateWorld = BoxTransform.TransformPosition(FVector(LocalX, LocalY, 0.0));

            FVector ProjectedWorld;
            bool bOutsideVolume = false;
            if (!ProjectCandidate(CandidateWorld, Box, ProjectedWorld, bOutsideVolume))
            {
                if (bOutsideVolume)
                {
                    ++RejectedOutsideVolumeCount;
                }
                else if (bSourceVolume)
                {
                    ++RejectedSourceProjectionCount;
                }
                else
                {
                    ++RejectedTargetProjectionCount;
                }
                continue;
            }

            FProjectedGridSample& NewSample = OutSamples.Emplace_GetRef();
            NewSample.ProjectedWorld = ProjectedWorld;
            NewSample.GridUV = FVector2D(XAlpha, YAlpha);

            if (bSourceVolume)
            {
                PreviewSourceSamples.Add(ProjectedWorld);
            }
            else
            {
                PreviewTargetSamples.Add(ProjectedWorld);
            }
        }
    }

    return !OutSamples.IsEmpty();
}

bool AOmniVolumeNavLink::ProjectCandidate(const FVector& CandidateWorld, const UBoxComponent& OwnerBox, FVector& OutProjectedWorld, bool& bOutOutsideVolume) const
{
    bOutOutsideVolume = false;

    UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld());
    if (!NavSystem)
    {
        return false;
    }

    FNavLocation ProjectedLocation;
    const FVector SafeExtent(
        FMath::Max(0.0, ProjectionExtent.X),
        FMath::Max(0.0, ProjectionExtent.Y),
        FMath::Max(0.0, ProjectionExtent.Z));

    const bool bProjected = NavSystem->ProjectPointToNavigation(
        CandidateWorld,
        ProjectedLocation,
        SafeExtent,
        static_cast<const FNavAgentProperties*>(nullptr),
        FSharedConstNavQueryFilter());

    if (!bProjected)
    {
        return false;
    }

    if (MaximumProjectionDistance > 0.0f)
    {
        const double MaxProjectionDistanceSq = FMath::Square(static_cast<double>(MaximumProjectionDistance));
        if (FVector::DistSquared(CandidateWorld, ProjectedLocation.Location) > MaxProjectionDistanceSq)
        {
            return false;
        }
    }

    if (!IsProjectedPointInsideBox(ProjectedLocation.Location, OwnerBox))
    {
        bOutOutsideVolume = true;
        return false;
    }

    OutProjectedWorld = ProjectedLocation.Location;
    return true;
}

bool AOmniVolumeNavLink::IsProjectedPointInsideBox(const FVector& WorldPoint, const UBoxComponent& Box) const
{
    const FVector LocalPoint = Box.GetComponentTransform().InverseTransformPosition(WorldPoint);
    const FVector Extent = Box.GetUnscaledBoxExtent();
    const double Tolerance = FMath::Max(0.0f, ProjectionContainmentTolerance);

    return FMath::Abs(LocalPoint.X) <= Extent.X + Tolerance
        && FMath::Abs(LocalPoint.Y) <= Extent.Y + Tolerance
        && FMath::Abs(LocalPoint.Z) <= Extent.Z + Tolerance;
}

void AOmniVolumeNavLink::BuildNearestPairs(const TArray<FProjectedGridSample>& SourceSamples, const TArray<FProjectedGridSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const
{
    OutPairs.Reset();
    OutPairs.Reserve(SourceSamples.Num() + TargetSamples.Num());

    const int32 MaxCount = FMath::Max(SourceSamples.Num(), TargetSamples.Num());
    for (int32 Index = 0; Index < MaxCount; ++Index)
    {
        if (Index < SourceSamples.Num())
        {
            const FProjectedGridSample& SourceSample = SourceSamples[Index];
            const int32 TargetIndex = FindNearestUsableSampleIndex(SourceSample.ProjectedWorld, true, TargetSamples);
            if (TargetIndex != INDEX_NONE)
            {
                FProjectedPair& Pair = OutPairs.Emplace_GetRef();
                Pair.SourceWorld = SourceSample.ProjectedWorld;
                Pair.TargetWorld = TargetSamples[TargetIndex].ProjectedWorld;
            }
        }

        if (Index < TargetSamples.Num())
        {
            const FProjectedGridSample& TargetSample = TargetSamples[Index];
            const int32 SourceIndex = FindNearestUsableSampleIndex(TargetSample.ProjectedWorld, false, SourceSamples);
            if (SourceIndex != INDEX_NONE)
            {
                FProjectedPair& Pair = OutPairs.Emplace_GetRef();
                Pair.SourceWorld = SourceSamples[SourceIndex].ProjectedWorld;
                Pair.TargetWorld = TargetSample.ProjectedWorld;
            }
        }
    }
}

void AOmniVolumeNavLink::BuildMatchedGridPairs(const TArray<FProjectedGridSample>& SourceSamples, const TArray<FProjectedGridSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const
{
    OutPairs.Reset();

    if (SourceSamples.IsEmpty() || TargetSamples.IsEmpty())
    {
        return;
    }

    // Matched Grid is intentionally one-to-one. The side with fewer valid NavMesh
    // samples drives the pairing, and each sample on the other side may be used only
    // once. Matching happens in normalized local grid coordinates rather than world
    // space, so correspondence survives different box sizes, offsets, and rotation.
    const bool bSourceDrives = SourceSamples.Num() <= TargetSamples.Num();
    const TArray<FProjectedGridSample>& DriverSamples = bSourceDrives ? SourceSamples : TargetSamples;
    const TArray<FProjectedGridSample>& MatchSamples = bSourceDrives ? TargetSamples : SourceSamples;

    OutPairs.Reserve(DriverSamples.Num());
    TBitArray<> UsedMatches(false, MatchSamples.Num());

    for (const FProjectedGridSample& DriverSample : DriverSamples)
    {
        const int32 MatchIndex = FindNearestUnusedSampleByUV(DriverSample, bSourceDrives, MatchSamples, UsedMatches);
        if (MatchIndex == INDEX_NONE)
        {
            continue;
        }

        UsedMatches[MatchIndex] = true;

        FProjectedPair& Pair = OutPairs.Emplace_GetRef();
        if (bSourceDrives)
        {
            Pair.SourceWorld = DriverSample.ProjectedWorld;
            Pair.TargetWorld = MatchSamples[MatchIndex].ProjectedWorld;
        }
        else
        {
            Pair.SourceWorld = MatchSamples[MatchIndex].ProjectedWorld;
            Pair.TargetWorld = DriverSample.ProjectedWorld;
        }
    }
}

bool AOmniVolumeNavLink::AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks, TArray<FOmniJumpLinkSpec>& InOutJumpLinks, TArray<FOmniMantleLinkSpec>& InOutMantleLinks, TArray<FOmniCustomLinkSpec>& InOutCustomLinks)
{
    ++CandidateCount;

    const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
    if (InOutLinks.Num() + InOutJumpLinks.Num() + InOutMantleLinks.Num() + InOutCustomLinks.Num() >= SafeMaximumLinks)
    {
        return false;
    }

    if (MaximumLinkDistance > 0.0f)
    {
        const double MaxLinkDistanceSq = FMath::Square(static_cast<double>(MaximumLinkDistance));
        if (FVector::DistSquared(Pair.SourceWorld, Pair.TargetWorld) > MaxLinkDistanceSq)
        {
            ++RejectedDistanceCount;
            return false;
        }
    }

    ENavLinkDirection::Type PairDirection = GetNativeDirection();
    bool bHeightRestricted = false;
    if (!ResolvePairDirection(Pair.SourceWorld, Pair.TargetWorld, PairDirection, bHeightRestricted))
    {
        return false;
    }

    const double MergeDistanceSq = FMath::Square(static_cast<double>(FMath::Max(0.0f, EndpointMergeDistance)));
    if (MergeDistanceSq > 0.0)
    {
        for (const FProjectedPair& ExistingPair : PreviewGeneratedPairs)
        {
            if (FVector::DistSquared(ExistingPair.SourceWorld, Pair.SourceWorld) <= MergeDistanceSq
                && FVector::DistSquared(ExistingPair.TargetWorld, Pair.TargetWorld) <= MergeDistanceSq)
            {
                ++MergedDuplicateCount;
                return false;
            }
        }
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
        if (InOutLinks.Num() + InOutJumpLinks.Num() + InOutMantleLinks.Num() + InOutCustomLinks.Num() >= SafeMaximumLinks)
        {
            return false;
        }

        if (bUseCustom)
        {
            FOmniCustomLinkSpec& CustomSpec = InOutCustomLinks.Emplace_GetRef();
            CustomSpec.SourceWorld = Pair.SourceWorld;
            CustomSpec.TargetWorld = Pair.TargetWorld;
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
            FOmniMantleLinkSpec& MantleSpec = InOutMantleLinks.Emplace_GetRef();
            MantleSpec.SourceWorld = Pair.SourceWorld;
            MantleSpec.TargetWorld = Pair.TargetWorld;
            MantleSpec.Direction = Direction;
        }
        else if (bUseJump)
        {
            FOmniJumpLinkSpec& JumpSpec = InOutJumpLinks.Emplace_GetRef();
            JumpSpec.SourceWorld = Pair.SourceWorld;
            JumpSpec.TargetWorld = Pair.TargetWorld;
            JumpSpec.Direction = Direction;
        }
        else
        {
            const FVector SourceLocal = LinkTransform.InverseTransformPosition(Pair.SourceWorld);
            const FVector TargetLocal = LinkTransform.InverseTransformPosition(Pair.TargetWorld);

            FNavigationLink& NewLink = InOutLinks.Emplace_GetRef(SourceLocal, TargetLocal);
            NewLink.Direction = Direction;
            NewLink.SnapRadius = FMath::Max(1.0f, SnapRadius);
            NewLink.LeftProjectHeight = FMath::Max(0.0f, NativeProjectionHeight);
            NewLink.MaxFallDownLength = FMath::Max(0.0f, NativeProjectionHeight);
            NewLink.bUseSnapHeight = false;
            NewLink.bSnapToCheapestArea = false;
            NewLink.SupportedAgents = SupportedAgents;
            NewLink.SetAreaClass(ResolvedAreaClass);
        }

        FProjectedPair PreviewPair = Pair;
        PreviewPair.NativeDirection = Direction;
        PreviewPair.bUsesJumpTraversal = bUseJump;
        PreviewPair.bUsesMantleTraversal = bUseMantle;
        PreviewPair.bUsesCustomTraversal = bUseCustom;
        PreviewGeneratedPairs.Add(PreviewPair);
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

        FProjectedPair PreviewPair = Pair;
        PreviewPair.NativeDirection = Direction;
        PreviewPair.bUsesJumpTraversal = true;
        PreviewPair.bUsesMantleTraversal = false;
        PreviewPair.bUsesCustomTraversal = false;
        PreviewRejectedJumpPairs.Add(PreviewPair);
    };

    auto ValidateJumpDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.TargetWorld : Pair.SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.SourceWorld : Pair.TargetWorld;
        bool bBlocked = false;
        bool bTooFast = false;
        if (IsJumpDirectionValid(StartWorld, EndWorld, bBlocked, bTooFast))
        {
            return true;
        }

        RecordRejectedJump(Direction, bBlocked, bTooFast);
        return false;
    };

    auto RecordRejectedMantle = [&](ENavLinkDirection::Type Direction)
    {
        ++RejectedMantlePathCount;

        FProjectedPair PreviewPair = Pair;
        PreviewPair.NativeDirection = Direction;
        PreviewPair.bUsesJumpTraversal = false;
        PreviewPair.bUsesMantleTraversal = true;
        PreviewPair.bUsesCustomTraversal = false;
        PreviewRejectedMantlePairs.Add(PreviewPair);
    };

    auto ValidateMantleDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.TargetWorld : Pair.SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.SourceWorld : Pair.TargetWorld;
        if (IsMantleDirectionValid(StartWorld, EndWorld))
        {
            return true;
        }

        RecordRejectedMantle(Direction);
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
    AutomaticSettings.AgentRadius = JumpValidationAgentRadius;
    AutomaticSettings.AgentHalfHeight = JumpValidationAgentHalfHeight;
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
        Pair.SourceWorld,
        Pair.TargetWorld,
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
                RecordRejectedMantle(Direction);
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

bool AOmniVolumeNavLink::AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const
{
    if (A.Num() != B.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const FNavigationLink& LinkA = A[Index];
        const FNavigationLink& LinkB = B[Index];

        if (!LinkA.Left.Equals(LinkB.Left, OmniVolumeNavLinkPrivate::LinkComparisonTolerance)
            || !LinkA.Right.Equals(LinkB.Right, OmniVolumeNavLinkPrivate::LinkComparisonTolerance)
            || LinkA.Direction != LinkB.Direction
            || !FMath::IsNearlyEqual(LinkA.SnapRadius, LinkB.SnapRadius)
            || !FMath::IsNearlyEqual(LinkA.LeftProjectHeight, LinkB.LeftProjectHeight)
            || !FMath::IsNearlyEqual(LinkA.MaxFallDownLength, LinkB.MaxFallDownLength)
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

int32 AOmniVolumeNavLink::CalculateAxisSampleCount(double WorldLength, double Spacing)
{
    if (WorldLength <= KINDA_SMALL_NUMBER)
    {
        return 1;
    }

    return FMath::Max(2, FMath::CeilToInt(WorldLength / FMath::Max(1.0, Spacing)) + 1);
}

int32 AOmniVolumeNavLink::FindNearestUsableSampleIndex(const FVector& Point, bool bPointIsSource, const TArray<FProjectedGridSample>& Samples) const
{
    int32 BestIndex = INDEX_NONE;
    double BestDistanceSq = TNumericLimits<double>::Max();

    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FVector SourceWorld = bPointIsSource ? Point : Samples[Index].ProjectedWorld;
        const FVector TargetWorld = bPointIsSource ? Samples[Index].ProjectedWorld : Point;
        if (!IsPairUsable(SourceWorld, TargetWorld))
        {
            continue;
        }

        const double DistanceSq = FVector::DistSquared(Point, Samples[Index].ProjectedWorld);
        if (DistanceSq < BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestIndex = Index;
        }
    }

    return BestIndex;
}

int32 AOmniVolumeNavLink::FindNearestUnusedSampleByUV(const FProjectedGridSample& DriverSample, bool bSourceDrives, const TArray<FProjectedGridSample>& Samples, const TBitArray<>& UsedSamples) const
{
    int32 BestIndex = INDEX_NONE;
    double BestDistanceSq = TNumericLimits<double>::Max();

    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        if (UsedSamples[Index])
        {
            continue;
        }

        const FVector SourceWorld = bSourceDrives ? DriverSample.ProjectedWorld : Samples[Index].ProjectedWorld;
        const FVector TargetWorld = bSourceDrives ? Samples[Index].ProjectedWorld : DriverSample.ProjectedWorld;
        if (!IsPairUsable(SourceWorld, TargetWorld))
        {
            continue;
        }

        const double DistanceSq = (Samples[Index].GridUV - DriverSample.GridUV).SizeSquared();
        if (DistanceSq < BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestIndex = Index;
        }
    }

    return BestIndex;
}

bool AOmniVolumeNavLink::IsPairUsable(const FVector& SourceWorld, const FVector& TargetWorld) const
{
    if (MaximumLinkDistance > 0.0f)
    {
        const double MaxLinkDistanceSq = FMath::Square(static_cast<double>(MaximumLinkDistance));
        if (FVector::DistSquared(SourceWorld, TargetWorld) > MaxLinkDistanceSq)
        {
            return false;
        }
    }

    ENavLinkDirection::Type PairDirection = GetNativeDirection();
    bool bHeightRestricted = false;
    return ResolvePairDirection(SourceWorld, TargetWorld, PairDirection, bHeightRestricted);
}

bool AOmniVolumeNavLink::IsTraversalAllowedByHeight(const FVector& StartWorld, const FVector& EndWorld) const
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

bool AOmniVolumeNavLink::ResolvePairDirection(const FVector& SourceWorld, const FVector& TargetWorld, ENavLinkDirection::Type& OutDirection, bool& bOutHeightRestricted) const
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
    case EOmniVolumeLinkDirection::SourceToTarget:
        OutDirection = ENavLinkDirection::LeftToRight;
        return bSourceToTargetAllowed;

    case EOmniVolumeLinkDirection::TargetToSource:
        OutDirection = ENavLinkDirection::RightToLeft;
        return bTargetToSourceAllowed;

    case EOmniVolumeLinkDirection::BothWays:
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


bool AOmniVolumeNavLink::IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, bool& bOutBlocked, bool& bOutTooFast) const
{
    bOutBlocked = false;
    bOutTooFast = false;

    FOmniJumpValidationSettings Settings;
    Settings.bValidatePath = bValidateJumpPath;
    Settings.ApexHeight = JumpApexHeight;
    Settings.AgentRadius = JumpValidationAgentRadius;
    Settings.AgentHalfHeight = JumpValidationAgentHalfHeight;
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

bool AOmniVolumeNavLink::IsMantleDirectionValid(const FVector& StartWorld, const FVector& EndWorld) const
{
    FOmniMantleValidationSettings Settings;
    Settings.bValidatePath = bValidateMantlePath;
    Settings.ArcHeight = MantleArcHeight;
    Settings.AgentRadius = JumpValidationAgentRadius;
    Settings.AgentHalfHeight = JumpValidationAgentHalfHeight;
    Settings.PathSegments = MantleValidationSegments;

    return OmniTraversal::ValidateMantlePath(GetWorld(), this, StartWorld, EndWorld, Settings) == EOmniMantleValidationResult::Valid;
}

float AOmniVolumeNavLink::GetEffectiveLinkSpacing() const
{
    switch (CoverageDensity)
    {
    case EOmniCoverageDensity::Sparse:
        return 200.0f;
    case EOmniCoverageDensity::Dense:
        return 50.0f;
    case EOmniCoverageDensity::Custom:
        return FMath::Max(10.0f, LinkSpacing);
    case EOmniCoverageDensity::Balanced:
    default:
        return 100.0f;
    }
}

ENavLinkDirection::Type AOmniVolumeNavLink::GetNativeDirection() const
{
    switch (LinkDirection)
    {
    case EOmniVolumeLinkDirection::SourceToTarget:
        return ENavLinkDirection::LeftToRight;
    case EOmniVolumeLinkDirection::TargetToSource:
        return ENavLinkDirection::RightToLeft;
    case EOmniVolumeLinkDirection::BothWays:
    default:
        return ENavLinkDirection::BothWays;
    }
}

bool AOmniVolumeNavLink::UsesTraversalAction(EOmniTraversalAction Action) const
{
    if (!bUseDirectionalTraversalActions)
    {
        return TraversalAction == Action;
    }

    return SourceToTargetTraversalAction == Action
        || TargetToSourceTraversalAction == Action;
}

void AOmniVolumeNavLink::UpdateGenerationStatus()
{
    if (!bEnabled)
    {
        GenerationStatus = FText::FromString(TEXT("Disabled. No links are being generated."));
        return;
    }

    if (ValidSourceSampleCount == 0)
    {
        GenerationStatus = FText::FromString(TEXT("No NavMesh found inside Source Box. Move or resize Source over a navigable region."));
        return;
    }

    if (ValidTargetSampleCount == 0)
    {
        GenerationStatus = FText::FromString(TEXT("No NavMesh found inside Target Box. Move or resize Target over a navigable region."));
        return;
    }

    if (GeneratedLinkCount == 0)
    {
        if (RejectedJumpArcCount > 0 || RejectedJumpSpeedCount > 0)
        {
            GenerationStatus = FText::FromString(TEXT("No links generated. Jump validation rejected the candidate jump directions. Clear obstacles, increase Jump Apex Height, or adjust the advanced jump validation settings."));
        }
        else if (bUseHeightLimits)
        {
            GenerationStatus = FText::FromString(TEXT("No links generated. No sample pairs satisfy Max Crossing Distance and the current Max Rise / Max Drop limits."));
        }
        else if (RejectedDistanceCount > 0)
        {
            GenerationStatus = FText::FromString(TEXT("No links generated. The volumes may be farther apart than Max Crossing Distance."));
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("No valid links generated. Check the two volumes and expand Advanced only if projection needs tuning."));
        }
        return;
    }

    FString Status = FString::Printf(TEXT("Ready: %d links (%s)."), GeneratedLinkCount, PairingMode == EOmniPairingMode::MatchedGrid ? TEXT("Matched Grid") : TEXT("Nearest"));

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
            Status += FString::Printf(
                TEXT(" %d pair%s split by explicit directional actions."),
                DirectionalSplitPairCount,
                DirectionalSplitPairCount == 1 ? TEXT(" was") : TEXT("s were"));
        }

        if (AutomaticSplitPairCount > 0)
        {
            Status += FString::Printf(
                TEXT(" %d pair%s split by Automatic traversal."),
                AutomaticSplitPairCount,
                AutomaticSplitPairCount == 1 ? TEXT(" was") : TEXT("s were"));
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
            Status += FString::Printf(TEXT(" %d bidirectional pair%s split by traversal type."), AutomaticSplitPairCount, AutomaticSplitPairCount == 1 ? TEXT(" was") : TEXT("s were"));
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
        const int32 RejectedJumpDirections = RejectedJumpArcCount + RejectedJumpSpeedCount;
        if (RejectedJumpDirections > 0)
        {
            Status += FString::Printf(TEXT(" %d jump direction%s rejected by path validation."), RejectedJumpDirections, RejectedJumpDirections == 1 ? TEXT(" was") : TEXT("s were"));
        }
        if (JumpRestrictedToOneWayCount > 0)
        {
            Status += FString::Printf(TEXT(" %d bidirectional jump pair%s reduced to one-way by validation."), JumpRestrictedToOneWayCount, JumpRestrictedToOneWayCount == 1 ? TEXT(" was") : TEXT("s were"));
        }
    }

    if (bValidateMantlePath && (UsesTraversalAction(EOmniTraversalAction::Mantle) || UsesTraversalAction(EOmniTraversalAction::Automatic)))
    {
        if (RejectedMantlePathCount > 0)
        {
            Status += FString::Printf(TEXT(" %d mantle direction%s rejected by path validation."), RejectedMantlePathCount, RejectedMantlePathCount == 1 ? TEXT(" was") : TEXT("s were"));
        }
        if (MantleRestrictedToOneWayCount > 0)
        {
            Status += FString::Printf(TEXT(" %d bidirectional mantle pair%s reduced to one-way by validation."), MantleRestrictedToOneWayCount, MantleRestrictedToOneWayCount == 1 ? TEXT(" was") : TEXT("s were"));
        }
    }

    if (bUseHeightLimits)
    {
        Status += FString::Printf(TEXT(" Height limits: Rise %.0f cm, Drop %.0f cm."), MaximumRise, MaximumDrop);
        if (HeightRestrictedLinkCount > 0)
        {
            Status += FString::Printf(TEXT(" %d pair%s reduced to one-way by height."), HeightRestrictedLinkCount, HeightRestrictedLinkCount == 1 ? TEXT(" was") : TEXT("s were"));
        }
    }

    const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
    if (GeneratedLinkCount >= SafeMaximumLinks)
    {
        Status += TEXT(" Maximum Generated Links was reached.");
    }

    if (bGridSampleCapReached)
    {
        Status += TEXT(" Grid sampling was automatically limited by the safety cap.");
    }

    GenerationStatus = FText::FromString(Status);
}

void AOmniVolumeNavLink::ApplyEditorVisualStyle()
{
    if (SourceBox)
    {
        SourceBox->ShapeColor = FColor(70, 200, 255);
        SourceBox->SetLineThickness(3.0f);
        SourceBox->bDrawOnlyIfSelected = true;
        SourceBox->MarkRenderStateDirty();
    }

    if (TargetBox)
    {
        TargetBox->ShapeColor = FColor(255, 160, 60);
        TargetBox->SetLineThickness(3.0f);
        TargetBox->bDrawOnlyIfSelected = true;
        TargetBox->MarkRenderStateDirty();
    }
}

void AOmniVolumeNavLink::ResetDebugCounters()
{
    GeneratedLinkCount = 0;
    NativeLinkCount = 0;
    JumpLinkCount = 0;
    MantleLinkCount = 0;
    CustomLinkCount = 0;
    SourceGridCandidateCount = 0;
    TargetGridCandidateCount = 0;
    ValidSourceSampleCount = 0;
    ValidTargetSampleCount = 0;
    CandidateCount = 0;
    RejectedSourceProjectionCount = 0;
    RejectedTargetProjectionCount = 0;
    RejectedOutsideVolumeCount = 0;
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
    RejectedJumpArcCount = 0;
    RejectedJumpSpeedCount = 0;
    JumpRestrictedToOneWayCount = 0;
    RejectedMantlePathCount = 0;
    MantleRestrictedToOneWayCount = 0;
    MergedDuplicateCount = 0;
    bGridSampleCapReached = false;
    PreviewSourceSamples.Reset();
    PreviewTargetSamples.Reset();
    PreviewGeneratedPairs.Reset();
    PreviewRejectedJumpPairs.Reset();
    PreviewRejectedMantlePairs.Reset();
}

#if WITH_EDITOR
void AOmniVolumeNavLink::DrawEditorPreview() const
{
    UWorld* World = GetWorld();
    if (!bShowPreview || !World || World->WorldType != EWorldType::Editor || !IsSelected())
    {
        return;
    }

    const FVector PreviewOffset(0.0, 0.0, 8.0);
    const bool bDrawSamples = PreviewDetail != EOmniPreviewDetail::LinksOnly;
    const bool bDrawDiagnostics = PreviewDetail == EOmniPreviewDetail::Diagnostics;

    if (bDrawSamples)
    {
        for (const FVector& Point : PreviewSourceSamples)
        {
            OmniPreview::DrawSamplePoint(World, Point, OmniPreview::SourceColor(), 10.0f, PreviewOffset);
        }

        for (const FVector& Point : PreviewTargetSamples)
        {
            OmniPreview::DrawSamplePoint(World, Point, OmniPreview::TargetColor(), 10.0f, PreviewOffset);
        }
    }

    for (const FProjectedPair& Pair : PreviewGeneratedPairs)
    {
        if (Pair.bUsesCustomTraversal)
        {
            OmniPreview::DrawCustomTraversal(World, Pair.SourceWorld, Pair.TargetWorld, Pair.NativeDirection, 1.5f, PreviewOffset);
        }
        else if (Pair.bUsesMantleTraversal)
        {
            OmniPreview::DrawMantleTraversal(World, Pair.SourceWorld, Pair.TargetWorld, Pair.NativeDirection, MantleArcHeight, MantleValidationSegments, 1.5f, PreviewOffset);
        }
        else
        {
            OmniPreview::DrawTraversal(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                Pair.bUsesJumpTraversal,
                JumpApexHeight,
                JumpValidationSegments,
                1.5f,
                PreviewOffset);
        }
    }

    if (bDrawDiagnostics)
    {
        const FColor RejectedColor = OmniPreview::RejectedColor();
        for (const FProjectedPair& Pair : PreviewRejectedJumpPairs)
        {
            OmniPreview::DrawTraversal(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                true,
                JumpApexHeight,
                JumpValidationSegments,
                2.0f,
                PreviewOffset,
                &RejectedColor);
        }

        for (const FProjectedPair& Pair : PreviewRejectedMantlePairs)
        {
            OmniPreview::DrawMantleTraversal(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                MantleArcHeight,
                MantleValidationSegments,
                2.0f,
                PreviewOffset,
                &RejectedColor);
        }
    }
}
#endif

void AOmniVolumeNavLink::NotifyNavigationSystem()
{
    if (!GeneratedLinks || !GeneratedLinks->IsRegistered() || !GeneratedLinks->GetWorld())
    {
        return;
    }

    FNavigationSystem::UpdateComponentData(*GeneratedLinks);
}

void AOmniVolumeNavLink::BindNavigationGenerationDelegate()
{
    if (!bRegenerateAfterNavigationBuild || !GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.AddUniqueDynamic(this, &AOmniVolumeNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniVolumeNavLink::UnbindNavigationGenerationDelegate()
{
    if (!GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.RemoveDynamic(this, &AOmniVolumeNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniVolumeNavLink::HandleNavigationGenerationFinished(ANavigationData* NavData)
{
    if (!bRegenerateAfterNavigationBuild || !NavData || NavData->GetWorld() != GetWorld())
    {
        return;
    }

    RegenerateLinks();
}

#if WITH_EDITOR
void AOmniVolumeNavLink::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    UnbindNavigationGenerationDelegate();
    BindNavigationGenerationDelegate();
    ApplyEditorVisualStyle();

    const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    const FName DirectionalActionsName = GET_MEMBER_NAME_CHECKED(AOmniVolumeNavLink, bUseDirectionalTraversalActions);
    const FName AutoRegenerateName = GET_MEMBER_NAME_CHECKED(AOmniVolumeNavLink, bAutoRegenerate);

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
            RegenerateLinks();
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("Automatic updates are off. Use Regenerate Links after editing volumes."));
        }
    }
}

void AOmniVolumeNavLink::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);

    if (bFinished && bAutoRegenerate)
    {
        RegenerateLinks();
    }
}

void AOmniVolumeNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    ApplyEditorVisualStyle();
    RegenerateLinks();
}
#endif
