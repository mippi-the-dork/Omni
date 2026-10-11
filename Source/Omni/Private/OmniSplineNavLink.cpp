#include "OmniSplineNavLink.h"

#include "OmniJumpTraversalComponent.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniNavArea_Jump.h"
#include "OmniTraversalUtils.h"
#include "OmniPreviewUtils.h"

#include "AI/NavigationSystemBase.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavLinkComponent.h"

#if WITH_EDITOR
#include "Engine/World.h"
#endif

namespace OmniSplineNavLinkPrivate
{
    constexpr float LinkComparisonTolerance = 0.1f;
}

AOmniSplineNavLink::AOmniSplineNavLink()
{
#if WITH_EDITOR
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
#else
    PrimaryActorTick.bCanEverTick = false;
#endif

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    SourceSpline = CreateDefaultSubobject<USplineComponent>(TEXT("SourceSpline"));
    SourceSpline->SetupAttachment(SceneRoot);
    SourceSpline->SetRelativeLocation(FVector(-300.0, 0.0, 0.0));
    SourceSpline->SetCanEverAffectNavigation(false);

    TargetSpline = CreateDefaultSubobject<USplineComponent>(TEXT("TargetSpline"));
    TargetSpline->SetupAttachment(SceneRoot);
    TargetSpline->SetRelativeLocation(FVector(300.0, 0.0, 0.0));
    TargetSpline->SetCanEverAffectNavigation(false);

    GeneratedLinks = CreateDefaultSubobject<UNavLinkComponent>(TEXT("GeneratedLinks"));
    GeneratedLinks->SetupAttachment(SceneRoot);
    GeneratedLinks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GeneratedLinks->SetGenerateOverlapEvents(false);

    JumpTraversal = CreateDefaultSubobject<UOmniJumpTraversalComponent>(TEXT("JumpTraversal"));
    CustomTraversal = CreateDefaultSubobject<UOmniCustomTraversalComponent>(TEXT("CustomTraversal"));

    AreaClass = UNavArea_Default::StaticClass();
    JumpAreaClass = UOmniNavArea_Jump::StaticClass();
    SupportedAgents = FNavAgentSelector(FNavAgentSelector::AllAgentsMask);
    GenerationStatus = FText::FromString(TEXT("Shape Source and Target splines over navigable regions."));

    auto InitializeSpline = [](USplineComponent& Spline)
    {
        Spline.ClearSplinePoints(false);
        Spline.AddSplinePoint(FVector(0.0, -300.0, 0.0), ESplineCoordinateSpace::Local, false);
        Spline.AddSplinePoint(FVector(0.0, 300.0, 0.0), ESplineCoordinateSpace::Local, false);
        Spline.SetClosedLoop(false, false);
        Spline.UpdateSpline();
    };

    InitializeSpline(*SourceSpline);
    InitializeSpline(*TargetSpline);
}

void AOmniSplineNavLink::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    ApplyEditorVisualStyle();
    if (bAutoRegenerate)
    {
        RegenerateLinks();
    }
}

void AOmniSplineNavLink::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    BindNavigationGenerationDelegate();
    RegenerateLinks();
}

void AOmniSplineNavLink::PostUnregisterAllComponents()
{
    UnbindNavigationGenerationDelegate();
    Super::PostUnregisterAllComponents();
}

void AOmniSplineNavLink::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

#if WITH_EDITOR
    DrawEditorPreview();
#endif
}

#if WITH_EDITOR
bool AOmniSplineNavLink::ShouldTickIfViewportsOnly() const
{
    return true;
}
#endif

void AOmniSplineNavLink::RegenerateLinks()
{
    if (bIsRegenerating || !GeneratedLinks || !SourceSpline || !TargetSpline)
    {
        return;
    }

    TGuardValue<bool> RegenerationGuard(bIsRegenerating, true);
    ResetDebugCounters();

    TArray<FNavigationLink> NewLinks;
    TArray<FOmniJumpLinkSpec> NewJumpLinks;
    TArray<FOmniCustomLinkSpec> NewCustomLinks;

    if (bEnabled)
    {
        const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
        NewLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));
        NewJumpLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));
        NewCustomLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));

        TArray<FProjectedSplineSample> SourceSamples;
        TArray<FProjectedSplineSample> TargetSamples;
        BuildProjectedSamples(*SourceSpline, true, SourceSamples);
        BuildProjectedSamples(*TargetSpline, false, TargetSamples);

        ValidSourceSampleCount = SourceSamples.Num();
        ValidTargetSampleCount = TargetSamples.Num();

        if (!SourceSamples.IsEmpty() && !TargetSamples.IsEmpty())
        {
            TArray<FProjectedPair> CandidatePairs;
            CandidatePairs.Reserve(SourceSamples.Num() + TargetSamples.Num());

            if (PairingMode == EOmniPairingMode::MatchedGrid)
            {
                const bool bSourceFilledArea = SourceSpline->IsClosedLoop() && ClosedSplineCoverage == EOmniClosedSplineCoverage::FilledArea;
                const bool bTargetFilledArea = TargetSpline->IsClosedLoop() && ClosedSplineCoverage == EOmniClosedSplineCoverage::FilledArea;

                if (bSourceFilledArea && bTargetFilledArea)
                {
                    BuildMatchedAreaPairs(SourceSamples, TargetSamples, CandidatePairs);
                }
                else if (!bSourceFilledArea && !bTargetFilledArea)
                {
                    const bool bWrap = SourceSpline->IsClosedLoop() && TargetSpline->IsClosedLoop();
                    BuildMatchedPairs(SourceSamples, TargetSamples, bWrap, CandidatePairs);
                }
                else
                {
                    bMatchedGridFallbackToNearest = true;
                    BuildNearestPairs(SourceSamples, TargetSamples, CandidatePairs);
                }
            }
            else
            {
                BuildNearestPairs(SourceSamples, TargetSamples, CandidatePairs);
            }

            auto GeneratedCount = [&NewLinks, &NewJumpLinks, &NewCustomLinks]()
            {
                return NewLinks.Num() + NewJumpLinks.Num() + NewCustomLinks.Num();
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
                AddProjectedPair(CandidatePairs[PairIndex], NewLinks, NewJumpLinks, NewCustomLinks);
            }

            for (int32 PairIndex = 0; PairIndex < CandidatePairs.Num() && GeneratedCount() < SafeMaximumLinks; ++PairIndex)
            {
                if (!AttemptedPairs[PairIndex])
                {
                    AddProjectedPair(CandidatePairs[PairIndex], NewLinks, NewJumpLinks, NewCustomLinks);
                }
            }
        }
    }

    NativeLinkCount = NewLinks.Num();
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

    if (AreLinkArraysEquivalent(GeneratedLinks->Links, NewLinks))
    {
        return;
    }

    GeneratedLinks->Links = MoveTemp(NewLinks);
    GeneratedLinks->UpdateBounds();
    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

void AOmniSplineNavLink::ResetSplines()
{
    if (!SourceSpline || !TargetSpline)
    {
        return;
    }

    SourceSpline->Modify();
    TargetSpline->Modify();

    auto ResetSpline = [](USplineComponent& Spline)
    {
        Spline.ClearSplinePoints(false);
        Spline.AddSplinePoint(FVector(0.0, -300.0, 0.0), ESplineCoordinateSpace::Local, false);
        Spline.AddSplinePoint(FVector(0.0, 300.0, 0.0), ESplineCoordinateSpace::Local, false);
        Spline.SetClosedLoop(false, false);
        Spline.UpdateSpline();
    };

    ResetSpline(*SourceSpline);
    ResetSpline(*TargetSpline);
    SourceSpline->SetRelativeTransform(FTransform(FQuat::Identity, FVector(-300.0, 0.0, 0.0), FVector::OneVector));
    TargetSpline->SetRelativeTransform(FTransform(FQuat::Identity, FVector(300.0, 0.0, 0.0), FVector::OneVector));

    ApplyEditorVisualStyle();
    RegenerateLinks();
}

bool AOmniSplineNavLink::BuildProjectedSamples(const USplineComponent& Spline, bool bSourceSpline, TArray<FProjectedSplineSample>& OutSamples)
{
    if (Spline.IsClosedLoop() && ClosedSplineCoverage == EOmniClosedSplineCoverage::FilledArea)
    {
        return BuildFilledAreaSamples(Spline, bSourceSpline, OutSamples);
    }

    return BuildPerimeterSamples(Spline, bSourceSpline, OutSamples);
}

bool AOmniSplineNavLink::BuildPerimeterSamples(const USplineComponent& Spline, bool bSourceSpline, TArray<FProjectedSplineSample>& OutSamples)
{
    OutSamples.Reset();

    const float SplineLength = Spline.GetSplineLength();
    if (SplineLength <= KINDA_SMALL_NUMBER)
    {
        if (bSourceSpline)
        {
            SourceCandidateCount = 0;
        }
        else
        {
            TargetCandidateCount = 0;
        }
        return false;
    }

    const float SafeSpacing = FMath::Max(10.0f, GetEffectiveLinkSpacing());
    const bool bClosedLoop = Spline.IsClosedLoop();

    int32 SampleCount = bClosedLoop
        ? FMath::Max(3, FMath::CeilToInt(SplineLength / SafeSpacing))
        : FMath::Max(2, FMath::CeilToInt(SplineLength / SafeSpacing) + 1);

    const int32 SafeMaximumSamples = FMath::Clamp(MaximumSamplesPerSpline, 2, 4096);
    if (SampleCount > SafeMaximumSamples)
    {
        SampleCount = SafeMaximumSamples;
        bSampleCapReached = true;
    }

    if (bSourceSpline)
    {
        SourceCandidateCount = SampleCount;
    }
    else
    {
        TargetCandidateCount = SampleCount;
    }

    OutSamples.Reserve(SampleCount);

    for (int32 Index = 0; Index < SampleCount; ++Index)
    {
        const float Alpha = bClosedLoop
            ? static_cast<float>(Index) / static_cast<float>(SampleCount)
            : (SampleCount <= 1 ? 0.0f : static_cast<float>(Index) / static_cast<float>(SampleCount - 1));

        const float Distance = Alpha * SplineLength;
        const FVector CandidateWorld = Spline.GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);

        FVector ProjectedWorld;
        if (!ProjectCandidate(CandidateWorld, ProjectedWorld))
        {
            if (bSourceSpline)
            {
                ++RejectedSourceProjectionCount;
            }
            else
            {
                ++RejectedTargetProjectionCount;
            }
            continue;
        }

        FProjectedSplineSample& Sample = OutSamples.Emplace_GetRef();
        Sample.ProjectedWorld = ProjectedWorld;
        Sample.NormalizedDistance = Alpha;

        if (bSourceSpline)
        {
            PreviewSourceSamples.Add(ProjectedWorld);
        }
        else
        {
            PreviewTargetSamples.Add(ProjectedWorld);
        }
    }

    return !OutSamples.IsEmpty();
}

bool AOmniSplineNavLink::BuildFilledAreaSamples(const USplineComponent& Spline, bool bSourceSpline, TArray<FProjectedSplineSample>& OutSamples)
{
    OutSamples.Reset();

    if (!Spline.IsClosedLoop())
    {
        return BuildPerimeterSamples(Spline, bSourceSpline, OutSamples);
    }

    const float SplineLength = Spline.GetSplineLength();
    if (SplineLength <= KINDA_SMALL_NUMBER)
    {
        if (bSourceSpline)
        {
            SourceCandidateCount = 0;
        }
        else
        {
            TargetCandidateCount = 0;
        }
        return false;
    }

    const float SafeSpacing = FMath::Max(10.0f, GetEffectiveLinkSpacing());
    const int32 SafeMaximumSamples = FMath::Clamp(MaximumSamplesPerSpline, 3, 4096);

    // Sample the actual curve densely enough to preserve curved closed-spline boundaries.
    const float BoundarySpacing = FMath::Min(SafeSpacing, 100.0f);
    const int32 MaximumBoundarySamples = FMath::Max(8, FMath::Min(256, SafeMaximumSamples));
    const int32 BoundarySampleCount = FMath::Clamp(FMath::CeilToInt(SplineLength / BoundarySpacing), 8, MaximumBoundarySamples);

    TArray<FVector2D> LocalPolygon;
    LocalPolygon.Reserve(BoundarySampleCount);

    double AverageLocalZ = 0.0;
    FVector2D BoundsMin(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
    FVector2D BoundsMax(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());

    for (int32 Index = 0; Index < BoundarySampleCount; ++Index)
    {
        const float Alpha = static_cast<float>(Index) / static_cast<float>(BoundarySampleCount);
        const FVector LocalPoint = Spline.GetLocationAtDistanceAlongSpline(Alpha * SplineLength, ESplineCoordinateSpace::Local);
        const FVector2D LocalPoint2D(LocalPoint.X, LocalPoint.Y);

        LocalPolygon.Add(LocalPoint2D);
        AverageLocalZ += LocalPoint.Z;
        BoundsMin.X = FMath::Min(BoundsMin.X, LocalPoint2D.X);
        BoundsMin.Y = FMath::Min(BoundsMin.Y, LocalPoint2D.Y);
        BoundsMax.X = FMath::Max(BoundsMax.X, LocalPoint2D.X);
        BoundsMax.Y = FMath::Max(BoundsMax.Y, LocalPoint2D.Y);
    }

    AverageLocalZ /= static_cast<double>(BoundarySampleCount);

    const double Width = BoundsMax.X - BoundsMin.X;
    const double Height = BoundsMax.Y - BoundsMin.Y;
    if (Width <= KINDA_SMALL_NUMBER || Height <= KINDA_SMALL_NUMBER || LocalPolygon.Num() < 3)
    {
        return BuildPerimeterSamples(Spline, bSourceSpline, OutSamples);
    }

    int32 GridCountX = FMath::Max(1, FMath::CeilToInt(Width / SafeSpacing));
    int32 GridCountY = FMath::Max(1, FMath::CeilToInt(Height / SafeSpacing));
    const int32 UncappedGridCount = GridCountX * GridCountY;

    while (GridCountX * GridCountY > SafeMaximumSamples)
    {
        bSampleCapReached = true;
        if (GridCountX >= GridCountY && GridCountX > 1)
        {
            --GridCountX;
        }
        else if (GridCountY > 1)
        {
            --GridCountY;
        }
        else
        {
            break;
        }
    }

    if (UncappedGridCount > SafeMaximumSamples)
    {
        bSampleCapReached = true;
    }

    OutSamples.Reserve(FMath::Min(GridCountX * GridCountY, SafeMaximumSamples));
    int32 CandidateCountInsideArea = 0;

    const FTransform SplineTransform = Spline.GetComponentTransform();

    for (int32 YIndex = 0; YIndex < GridCountY; ++YIndex)
    {
        const double YAlpha = (static_cast<double>(YIndex) + 0.5) / static_cast<double>(GridCountY);
        const double LocalY = FMath::Lerp(BoundsMin.Y, BoundsMax.Y, YAlpha);

        for (int32 XIndex = 0; XIndex < GridCountX; ++XIndex)
        {
            const double XAlpha = (static_cast<double>(XIndex) + 0.5) / static_cast<double>(GridCountX);
            const double LocalX = FMath::Lerp(BoundsMin.X, BoundsMax.X, XAlpha);
            const FVector2D LocalPoint2D(LocalX, LocalY);

            if (!IsPointInsidePolygon2D(LocalPoint2D, LocalPolygon))
            {
                continue;
            }

            ++CandidateCountInsideArea;

            const FVector CandidateLocal(LocalX, LocalY, AverageLocalZ);
            const FVector CandidateWorld = SplineTransform.TransformPosition(CandidateLocal);

            FVector ProjectedWorld;
            if (!ProjectCandidate(CandidateWorld, ProjectedWorld))
            {
                if (bSourceSpline)
                {
                    ++RejectedSourceProjectionCount;
                }
                else
                {
                    ++RejectedTargetProjectionCount;
                }
                continue;
            }

            // ProjectPointToNavigation can move the result laterally. Keep filled-area samples inside the authored boundary.
            const FVector ProjectedLocal = SplineTransform.InverseTransformPosition(ProjectedWorld);
            if (!IsPointInsidePolygon2D(FVector2D(ProjectedLocal.X, ProjectedLocal.Y), LocalPolygon))
            {
                if (bSourceSpline)
                {
                    ++RejectedSourceProjectionCount;
                }
                else
                {
                    ++RejectedTargetProjectionCount;
                }
                continue;
            }

            FProjectedSplineSample& Sample = OutSamples.Emplace_GetRef();
            Sample.ProjectedWorld = ProjectedWorld;
            Sample.NormalizedArea = FVector2D(
                Width <= KINDA_SMALL_NUMBER ? 0.5 : (LocalX - BoundsMin.X) / Width,
                Height <= KINDA_SMALL_NUMBER ? 0.5 : (LocalY - BoundsMin.Y) / Height);
            Sample.bHasNormalizedArea = true;

            if (bSourceSpline)
            {
                PreviewSourceSamples.Add(ProjectedWorld);
            }
            else
            {
                PreviewTargetSamples.Add(ProjectedWorld);
            }
        }
    }

    if (bSourceSpline)
    {
        SourceCandidateCount = CandidateCountInsideArea;
    }
    else
    {
        TargetCandidateCount = CandidateCountInsideArea;
    }

    return !OutSamples.IsEmpty();
}

bool AOmniSplineNavLink::ProjectCandidate(const FVector& CandidateWorld, FVector& OutProjectedWorld) const
{
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

    OutProjectedWorld = ProjectedLocation.Location;
    return true;
}

void AOmniSplineNavLink::BuildNearestPairs(const TArray<FProjectedSplineSample>& SourceSamples, const TArray<FProjectedSplineSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const
{
    OutPairs.Reset();
    OutPairs.Reserve(SourceSamples.Num() + TargetSamples.Num());

    const int32 MaxCount = FMath::Max(SourceSamples.Num(), TargetSamples.Num());
    for (int32 Index = 0; Index < MaxCount; ++Index)
    {
        if (Index < SourceSamples.Num())
        {
            const FProjectedSplineSample& SourceSample = SourceSamples[Index];
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
            const FProjectedSplineSample& TargetSample = TargetSamples[Index];
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

void AOmniSplineNavLink::BuildMatchedPairs(const TArray<FProjectedSplineSample>& SourceSamples, const TArray<FProjectedSplineSample>& TargetSamples, bool bWrapNormalizedDistance, TArray<FProjectedPair>& OutPairs) const
{
    OutPairs.Reset();

    if (SourceSamples.IsEmpty() || TargetSamples.IsEmpty())
    {
        return;
    }

    const bool bSourceDrives = SourceSamples.Num() <= TargetSamples.Num();
    const TArray<FProjectedSplineSample>& DriverSamples = bSourceDrives ? SourceSamples : TargetSamples;
    const TArray<FProjectedSplineSample>& MatchSamples = bSourceDrives ? TargetSamples : SourceSamples;

    OutPairs.Reserve(DriverSamples.Num());
    TBitArray<> UsedMatches(false, MatchSamples.Num());

    for (const FProjectedSplineSample& DriverSample : DriverSamples)
    {
        const int32 MatchIndex = FindNearestUnusedSampleByNormalizedDistance(
            DriverSample,
            bSourceDrives,
            MatchSamples,
            UsedMatches,
            bWrapNormalizedDistance);

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

void AOmniSplineNavLink::BuildMatchedAreaPairs(const TArray<FProjectedSplineSample>& SourceSamples, const TArray<FProjectedSplineSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const
{
    OutPairs.Reset();

    if (SourceSamples.IsEmpty() || TargetSamples.IsEmpty())
    {
        return;
    }

    const bool bSourceDrives = SourceSamples.Num() <= TargetSamples.Num();
    const TArray<FProjectedSplineSample>& DriverSamples = bSourceDrives ? SourceSamples : TargetSamples;
    const TArray<FProjectedSplineSample>& MatchSamples = bSourceDrives ? TargetSamples : SourceSamples;

    OutPairs.Reserve(DriverSamples.Num());
    TBitArray<> UsedMatches(false, MatchSamples.Num());

    for (const FProjectedSplineSample& DriverSample : DriverSamples)
    {
        if (!DriverSample.bHasNormalizedArea)
        {
            continue;
        }

        const int32 MatchIndex = FindNearestUnusedSampleByNormalizedArea(DriverSample, bSourceDrives, MatchSamples, UsedMatches);
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

bool AOmniSplineNavLink::AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks, TArray<FOmniJumpLinkSpec>& InOutJumpLinks, TArray<FOmniCustomLinkSpec>& InOutCustomLinks)
{
    ++CandidateCount;

    const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
    if (InOutLinks.Num() + InOutJumpLinks.Num() + InOutCustomLinks.Num() >= SafeMaximumLinks)
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

    auto EmitRawLink = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseCustom) -> bool
    {
        if (InOutLinks.Num() + InOutJumpLinks.Num() + InOutCustomLinks.Num() >= SafeMaximumLinks)
        {
            return false;
        }

        if (bUseCustom)
        {
            FOmniCustomLinkSpec& CustomSpec = InOutCustomLinks.Emplace_GetRef();
            CustomSpec.SourceWorld = Pair.SourceWorld;
            CustomSpec.TargetWorld = Pair.TargetWorld;
            CustomSpec.Direction = Direction;
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
    AutomaticSettings.AgentRadius = JumpValidationAgentRadius;
    AutomaticSettings.AgentHalfHeight = JumpValidationAgentHalfHeight;

    const FOmniTraversalPlan TraversalPlan = OmniTraversal::BuildTraversalPlan(
        GetWorld(),
        this,
        Pair.SourceWorld,
        Pair.TargetWorld,
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

bool AOmniSplineNavLink::AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const
{
    if (A.Num() != B.Num())
    {
        return false;
    }

    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const FNavigationLink& LinkA = A[Index];
        const FNavigationLink& LinkB = B[Index];

        if (!LinkA.Left.Equals(LinkB.Left, OmniSplineNavLinkPrivate::LinkComparisonTolerance)
            || !LinkA.Right.Equals(LinkB.Right, OmniSplineNavLinkPrivate::LinkComparisonTolerance)
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

int32 AOmniSplineNavLink::FindNearestUsableSampleIndex(const FVector& Point, bool bPointIsSource, const TArray<FProjectedSplineSample>& Samples) const
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

int32 AOmniSplineNavLink::FindNearestUnusedSampleByNormalizedDistance(const FProjectedSplineSample& DriverSample, bool bSourceDrives, const TArray<FProjectedSplineSample>& Samples, const TBitArray<>& UsedSamples, bool bWrap) const
{
    int32 BestIndex = INDEX_NONE;
    double BestDistance = TNumericLimits<double>::Max();

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

        double Distance = FMath::Abs(static_cast<double>(Samples[Index].NormalizedDistance - DriverSample.NormalizedDistance));
        if (bWrap)
        {
            Distance = FMath::Min(Distance, 1.0 - Distance);
        }

        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            BestIndex = Index;
        }
    }

    return BestIndex;
}

int32 AOmniSplineNavLink::FindNearestUnusedSampleByNormalizedArea(const FProjectedSplineSample& DriverSample, bool bSourceDrives, const TArray<FProjectedSplineSample>& Samples, const TBitArray<>& UsedSamples) const
{
    int32 BestIndex = INDEX_NONE;
    double BestDistanceSq = TNumericLimits<double>::Max();

    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        if (UsedSamples[Index] || !Samples[Index].bHasNormalizedArea)
        {
            continue;
        }

        const FVector SourceWorld = bSourceDrives ? DriverSample.ProjectedWorld : Samples[Index].ProjectedWorld;
        const FVector TargetWorld = bSourceDrives ? Samples[Index].ProjectedWorld : DriverSample.ProjectedWorld;
        if (!IsPairUsable(SourceWorld, TargetWorld))
        {
            continue;
        }

        const double DistanceSq = FVector2D::DistSquared(DriverSample.NormalizedArea, Samples[Index].NormalizedArea);
        if (DistanceSq < BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestIndex = Index;
        }
    }

    return BestIndex;
}

bool AOmniSplineNavLink::IsPairUsable(const FVector& SourceWorld, const FVector& TargetWorld) const
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

bool AOmniSplineNavLink::IsTraversalAllowedByHeight(const FVector& StartWorld, const FVector& EndWorld) const
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

bool AOmniSplineNavLink::ResolvePairDirection(const FVector& SourceWorld, const FVector& TargetWorld, ENavLinkDirection::Type& OutDirection, bool& bOutHeightRestricted) const
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
    case EOmniSplineLinkDirection::SourceToTarget:
        OutDirection = ENavLinkDirection::LeftToRight;
        return bSourceToTargetAllowed;

    case EOmniSplineLinkDirection::TargetToSource:
        OutDirection = ENavLinkDirection::RightToLeft;
        return bTargetToSourceAllowed;

    case EOmniSplineLinkDirection::BothWays:
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

bool AOmniSplineNavLink::IsPointInsidePolygon2D(const FVector2D& Point, const TArray<FVector2D>& Polygon)
{
    if (Polygon.Num() < 3)
    {
        return false;
    }

    bool bInside = false;
    int32 PreviousIndex = Polygon.Num() - 1;

    for (int32 Index = 0; Index < Polygon.Num(); ++Index)
    {
        const FVector2D& A = Polygon[Index];
        const FVector2D& B = Polygon[PreviousIndex];

        const bool bCrossesScanline = ((A.Y > Point.Y) != (B.Y > Point.Y));
        if (bCrossesScanline)
        {
            const double Denominator = B.Y - A.Y;
            if (!FMath::IsNearlyZero(Denominator))
            {
                const double CrossingX = (B.X - A.X) * (Point.Y - A.Y) / Denominator + A.X;
                if (Point.X < CrossingX)
                {
                    bInside = !bInside;
                }
            }
        }

        PreviousIndex = Index;
    }

    return bInside;
}

bool AOmniSplineNavLink::IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, bool& bOutBlocked, bool& bOutTooFast) const
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

float AOmniSplineNavLink::GetEffectiveLinkSpacing() const
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

ENavLinkDirection::Type AOmniSplineNavLink::GetNativeDirection() const
{
    switch (LinkDirection)
    {
    case EOmniSplineLinkDirection::SourceToTarget:
        return ENavLinkDirection::LeftToRight;
    case EOmniSplineLinkDirection::TargetToSource:
        return ENavLinkDirection::RightToLeft;
    case EOmniSplineLinkDirection::BothWays:
    default:
        return ENavLinkDirection::BothWays;
    }
}

void AOmniSplineNavLink::UpdateGenerationStatus()
{
    if (!bEnabled)
    {
        GenerationStatus = FText::FromString(TEXT("Disabled. No links are being generated."));
        return;
    }

    if (!SourceSpline || SourceSpline->GetSplineLength() <= KINDA_SMALL_NUMBER)
    {
        GenerationStatus = FText::FromString(TEXT("Source Spline has no usable length."));
        return;
    }

    if (!TargetSpline || TargetSpline->GetSplineLength() <= KINDA_SMALL_NUMBER)
    {
        GenerationStatus = FText::FromString(TEXT("Target Spline has no usable length."));
        return;
    }

    const bool bSourceFilledArea = SourceSpline->IsClosedLoop() && ClosedSplineCoverage == EOmniClosedSplineCoverage::FilledArea;
    const bool bTargetFilledArea = TargetSpline->IsClosedLoop() && ClosedSplineCoverage == EOmniClosedSplineCoverage::FilledArea;

    if (ValidSourceSampleCount == 0)
    {
        GenerationStatus = FText::FromString(bSourceFilledArea
            ? TEXT("No NavMesh found inside Source closed spline. Adjust the region or projection in Advanced.")
            : TEXT("No NavMesh found along Source Spline. Move the spline onto a navigable region or adjust projection in Advanced."));
        return;
    }

    if (ValidTargetSampleCount == 0)
    {
        GenerationStatus = FText::FromString(bTargetFilledArea
            ? TEXT("No NavMesh found inside Target closed spline. Adjust the region or projection in Advanced.")
            : TEXT("No NavMesh found along Target Spline. Move the spline onto a navigable region or adjust projection in Advanced."));
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
            GenerationStatus = FText::FromString(TEXT("No links generated. The shapes may be farther apart than Max Crossing Distance."));
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("No valid links generated. Check spline placement and NavMesh coverage."));
        }
        return;
    }

    const TCHAR* PairingName = PairingMode == EOmniPairingMode::MatchedGrid ? TEXT("Matched Grid") : TEXT("Nearest");
    const TCHAR* SourceShape = !SourceSpline->IsClosedLoop()
        ? TEXT("Open")
        : (bSourceFilledArea ? TEXT("Closed Filled") : TEXT("Closed Perimeter"));
    const TCHAR* TargetShape = !TargetSpline->IsClosedLoop()
        ? TEXT("Open")
        : (bTargetFilledArea ? TEXT("Closed Filled") : TEXT("Closed Perimeter"));

    FString Status = FString::Printf(TEXT("Ready: %d links (%s, %s to %s)."), GeneratedLinkCount, PairingName, SourceShape, TargetShape);

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
            Status += FString::Printf(TEXT(" %d bidirectional pair%s split by traversal type."), AutomaticSplitPairCount, AutomaticSplitPairCount == 1 ? TEXT(" was") : TEXT("s were"));
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
            Status += FString::Printf(TEXT(" %d bidirectional jump pair%s reduced to one-way by validation."), JumpRestrictedToOneWayCount, JumpRestrictedToOneWayCount == 1 ? TEXT(" was") : TEXT("s were"));
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

    if (bMatchedGridFallbackToNearest)
    {
        Status += TEXT(" Matched Grid fell back to Nearest because filled-area and path sampling were mixed.");
    }

    const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
    if (GeneratedLinkCount >= SafeMaximumLinks)
    {
        Status += TEXT(" Generated-link safety cap reached.");
    }

    if (bSampleCapReached)
    {
        Status += TEXT(" Shape sampling was limited by the safety cap.");
    }

    GenerationStatus = FText::FromString(Status);
}

void AOmniSplineNavLink::ApplyEditorVisualStyle()
{
#if WITH_EDITOR
    if (SourceSpline)
    {
        const FLinearColor SourceColor = FLinearColor(FColor(70, 200, 255));
        SourceSpline->SetUnselectedSplineSegmentColor(SourceColor);
        SourceSpline->SetSelectedSplineSegmentColor(SourceColor);
        SourceSpline->MarkRenderStateDirty();
    }

    if (TargetSpline)
    {
        const FLinearColor TargetColor = FLinearColor(FColor(255, 160, 60));
        TargetSpline->SetUnselectedSplineSegmentColor(TargetColor);
        TargetSpline->SetSelectedSplineSegmentColor(TargetColor);
        TargetSpline->MarkRenderStateDirty();
    }
#endif
}

void AOmniSplineNavLink::ResetDebugCounters()
{
    GeneratedLinkCount = 0;
    NativeLinkCount = 0;
    JumpLinkCount = 0;
    CustomLinkCount = 0;
    ValidSourceSampleCount = 0;
    ValidTargetSampleCount = 0;
    SourceCandidateCount = 0;
    TargetCandidateCount = 0;
    CandidateCount = 0;
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
    MergedDuplicateCount = 0;
    bSampleCapReached = false;
    bMatchedGridFallbackToNearest = false;
    PreviewSourceSamples.Reset();
    PreviewTargetSamples.Reset();
    PreviewGeneratedPairs.Reset();
    PreviewRejectedJumpPairs.Reset();
}

#if WITH_EDITOR
void AOmniSplineNavLink::DrawEditorPreview() const
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
    }
}
#endif

void AOmniSplineNavLink::NotifyNavigationSystem()
{
    if (!GeneratedLinks || !GeneratedLinks->IsRegistered() || !GeneratedLinks->GetWorld())
    {
        return;
    }

    FNavigationSystem::UpdateComponentData(*GeneratedLinks);
}

void AOmniSplineNavLink::BindNavigationGenerationDelegate()
{
    if (!bRegenerateAfterNavigationBuild || !GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.AddUniqueDynamic(this, &AOmniSplineNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniSplineNavLink::UnbindNavigationGenerationDelegate()
{
    if (!GetWorld())
    {
        return;
    }

    if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
    {
        NavSystem->OnNavigationGenerationFinishedDelegate.RemoveDynamic(this, &AOmniSplineNavLink::HandleNavigationGenerationFinished);
    }
}

void AOmniSplineNavLink::HandleNavigationGenerationFinished(ANavigationData* NavData)
{
    if (!bRegenerateAfterNavigationBuild || !NavData || NavData->GetWorld() != GetWorld())
    {
        return;
    }

    RegenerateLinks();
}

#if WITH_EDITOR
void AOmniSplineNavLink::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    UnbindNavigationGenerationDelegate();
    BindNavigationGenerationDelegate();
    ApplyEditorVisualStyle();

    const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    const FName AutoRegenerateName = GET_MEMBER_NAME_CHECKED(AOmniSplineNavLink, bAutoRegenerate);

    if (bAutoRegenerate || PropertyName == AutoRegenerateName)
    {
        if (bAutoRegenerate)
        {
            RegenerateLinks();
        }
        else
        {
            GenerationStatus = FText::FromString(TEXT("Automatic updates are off. Use Regenerate Links after editing splines."));
        }
    }
}

void AOmniSplineNavLink::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);

    if (bFinished && bAutoRegenerate)
    {
        RegenerateLinks();
    }
}

void AOmniSplineNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    ApplyEditorVisualStyle();
    RegenerateLinks();
}
#endif
