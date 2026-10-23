#include "OmniSplineNavLink.h"
#include "HAL/PlatformTime.h"

#include "OmniEditorDiagnostics.h"

#include "OmniJumpTraversalComponent.h"
#include "OmniMantleTraversalComponent.h"
#include "OmniCustomTraversalComponent.h"
#include "OmniNavArea_Jump.h"
#include "OmniNavArea_Mantle.h"
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
    MantleTraversal = CreateDefaultSubobject<UOmniMantleTraversalComponent>(TEXT("MantleTraversal"));
    CustomTraversal = CreateDefaultSubobject<UOmniCustomTraversalComponent>(TEXT("CustomTraversal"));

    AreaClass = UNavArea_Default::StaticClass();
    JumpAreaClass = UOmniNavArea_Jump::StaticClass();
    MantleAreaClass = UOmniNavArea_Mantle::StaticClass();
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
#if WITH_EDITOR
        if (GetWorld() && GetWorld()->WorldType == EWorldType::Editor)
        {
            QueueEditorRegeneration();
        }
        else
#endif
        {
            RegenerateLinks();
        }
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
    if (bEditorRegenerationPending)
    {
        bEditorRegenerationPending = false;
        if (bAutoRegenerate)
        {
            RegenerateLinks();
        }
    }

    DrawEditorPreview();
#endif
}

#if WITH_EDITOR
bool AOmniSplineNavLink::ShouldTickIfViewportsOnly() const
{
    return true;
}
#endif

void AOmniSplineNavLink::SelectDiagnosticBlockers()
{
    TArray<FString> BlockingObjects;
    auto Collect = [&BlockingObjects](const TArray<FProjectedPair>& Pairs)
    {
        for (const FProjectedPair& Pair : Pairs)
        {
            if (!Pair.BlockingObject.IsEmpty())
            {
                BlockingObjects.AddUnique(Pair.BlockingObject);
            }
        }
    };

    Collect(PreviewRejectedJumpPairs);
    Collect(PreviewRejectedMantlePairs);

    OmniEditorDiagnostics::SelectBlockingActors(GetWorld(), BlockingObjects);
}

void AOmniSplineNavLink::FocusDiagnosticBlockers()
{
    TArray<FString> BlockingObjects;
    auto Collect = [&BlockingObjects](const TArray<FProjectedPair>& Pairs)
    {
        for (const FProjectedPair& Pair : Pairs)
        {
            if (!Pair.BlockingObject.IsEmpty())
            {
                BlockingObjects.AddUnique(Pair.BlockingObject);
            }
        }
    };

    Collect(PreviewRejectedJumpPairs);
    Collect(PreviewRejectedMantlePairs);

    OmniEditorDiagnostics::FocusBlockingActors(GetWorld(), BlockingObjects);
}

void AOmniSplineNavLink::FocusDiagnosticFailures()
{
    TArray<FVector> FailureLocations;

    auto Collect = [&FailureLocations](const TArray<FProjectedPair>& Links, bool bMantleTraversal, float ArcHeight)
    {
        for (const FProjectedPair& Link : Links)
        {
            FailureLocations.Add(OmniPreview::GetRejectedFailureLocation(
                Link.SourceWorld,
                Link.TargetWorld,
                Link.NativeDirection,
                Link.ValidationFailureCode,
                Link.bValidationSpeedFailure,
                bMantleTraversal,
                ArcHeight,
                Link.bHasBlockingLocation,
                Link.BlockingLocation));
        }
    };

    Collect(PreviewRejectedJumpPairs, false, JumpApexHeight);
    Collect(PreviewRejectedMantlePairs, true, MantleArcHeight);

    OmniEditorDiagnostics::FocusFailureLocations(FailureLocations);
}

void AOmniSplineNavLink::RecheckDiagnostics()
{
    RegenerateLinks();

    if (!PreviewRejectedJumpPairs.IsEmpty() || !PreviewRejectedMantlePairs.IsEmpty())
    {
        FocusDiagnosticFailures();
    }
}

void AOmniSplineNavLink::RegenerateLinks()
{
    if (bIsRegenerating || !GeneratedLinks || !SourceSpline || !TargetSpline)
    {
        return;
    }

#if WITH_EDITOR
    bEditorRegenerationPending = false;
#endif

    TGuardValue<bool> RegenerationGuard(bIsRegenerating, true);
    const double GenerationStartSeconds = FPlatformTime::Seconds();
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

    LastGenerationTimeMs = static_cast<float>((FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0);

    if (AreLinkArraysEquivalent(GeneratedLinks->Links, NewLinks))
    {
        return;
    }

    GeneratedLinks->Links = MoveTemp(NewLinks);
    GeneratedLinks->UpdateBounds();
    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

void AOmniSplineNavLink::SwapSourceAndTarget()
{
    if (!SourceSpline || !TargetSpline)
    {
        return;
    }

    SourceSpline->Modify();
    TargetSpline->Modify();

    struct FSplineSnapshot
    {
        FTransform RelativeTransform;
        TArray<FSplinePoint> Points;
        bool bClosedLoop = false;
    };

    const auto CaptureSpline = [](const USplineComponent& Spline)
    {
        FSplineSnapshot Snapshot;
        Snapshot.RelativeTransform = Spline.GetRelativeTransform();
        Snapshot.bClosedLoop = Spline.IsClosedLoop();

        const int32 PointCount = Spline.GetNumberOfSplinePoints();
        Snapshot.Points.Reserve(PointCount);
        for (int32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
        {
            Snapshot.Points.Add(Spline.GetSplinePointAt(PointIndex, ESplineCoordinateSpace::Local));
        }

        return Snapshot;
    };

    const auto ApplySpline = [](USplineComponent& Spline, const FSplineSnapshot& Snapshot)
    {
        Spline.SetRelativeTransform(Snapshot.RelativeTransform);
        Spline.ClearSplinePoints(false);
        Spline.AddPoints(Snapshot.Points, false);
        Spline.SetClosedLoop(Snapshot.bClosedLoop, false);
        Spline.UpdateSpline();
    };

    const FSplineSnapshot SourceSnapshot = CaptureSpline(*SourceSpline);
    const FSplineSnapshot TargetSnapshot = CaptureSpline(*TargetSpline);

    ApplySpline(*SourceSpline, TargetSnapshot);
    ApplySpline(*TargetSpline, SourceSnapshot);

    ApplyEditorVisualStyle();
    RegenerateLinks();
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

bool AOmniSplineNavLink::AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks, TArray<FOmniJumpLinkSpec>& InOutJumpLinks, TArray<FOmniMantleLinkSpec>& InOutMantleLinks, TArray<FOmniCustomLinkSpec>& InOutCustomLinks)
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

    const double MergeDistance = static_cast<double>(FMath::Max(0.0f, EndpointMergeDistance));
    const double MergeDistanceSq = FMath::Square(MergeDistance);
    const double MergeCellSize = FMath::Max(1.0, MergeDistance);
    const auto ToMergeCell = [MergeCellSize](const FVector& Location)
    {
        return FIntVector(
            FMath::FloorToInt(Location.X / MergeCellSize),
            FMath::FloorToInt(Location.Y / MergeCellSize),
            FMath::FloorToInt(Location.Z / MergeCellSize));
    };

    if (MergeDistanceSq > 0.0)
    {
        const FIntVector SourceCell = ToMergeCell(Pair.SourceWorld);
        for (int32 Z = -1; Z <= 1; ++Z)
        {
            for (int32 Y = -1; Y <= 1; ++Y)
            {
                for (int32 X = -1; X <= 1; ++X)
                {
                    const FIntVector NeighborCell = SourceCell + FIntVector(X, Y, Z);
                    const TArray<int32>* PairIndices = PreviewPairMergeBuckets.Find(NeighborCell);
                    if (!PairIndices)
                    {
                        continue;
                    }

                    for (const int32 ExistingIndex : *PairIndices)
                    {
                        if (!PreviewGeneratedPairs.IsValidIndex(ExistingIndex))
                        {
                            continue;
                        }

                        const FProjectedPair& ExistingPair = PreviewGeneratedPairs[ExistingIndex];
                        if (FVector::DistSquared(ExistingPair.SourceWorld, Pair.SourceWorld) <= MergeDistanceSq
                            && FVector::DistSquared(ExistingPair.TargetWorld, Pair.TargetWorld) <= MergeDistanceSq)
                        {
                            ++MergedDuplicateCount;
                            return false;
                        }
                    }
                }
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
        if (MergeDistanceSq > 0.0)
        {
            PreviewPairMergeBuckets.FindOrAdd(ToMergeCell(PreviewPair.SourceWorld)).Add(PreviewGeneratedPairs.Num() - 1);
        }
        return true;
    };

    auto RecordRejectedJump = [&](ENavLinkDirection::Type Direction, int32 FailureCode, bool bTooFast, const FVector& BlockingLocation, const FString& BlockingObject)
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

        FProjectedPair PreviewPair = Pair;
        PreviewPair.NativeDirection = Direction;
        PreviewPair.bUsesJumpTraversal = true;
        PreviewPair.bUsesMantleTraversal = false;
        PreviewPair.bUsesCustomTraversal = false;
        PreviewPair.ValidationFailureCode = static_cast<uint8>(FailureCode);
        PreviewPair.bValidationSpeedFailure = bTooFast;
        PreviewPair.bHasBlockingLocation = (FailureCode == 2 || FailureCode == 3 || FailureCode == 5) && (!BlockingLocation.IsNearlyZero() || !BlockingObject.IsEmpty());
        PreviewPair.BlockingLocation = BlockingLocation;
        PreviewPair.BlockingObject = BlockingObject;
        PreviewRejectedJumpPairs.Add(PreviewPair);
    };

    auto ValidateJumpDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.TargetWorld : Pair.SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.SourceWorld : Pair.TargetWorld;
        int32 FailureCode = 0;
        bool bTooFast = false;
        FVector BlockingLocation = FVector::ZeroVector;
        FString BlockingObject;
        if (IsJumpDirectionValid(StartWorld, EndWorld, FailureCode, bTooFast, &BlockingLocation, &BlockingObject))
        {
            return true;
        }

        RecordRejectedJump(Direction, FailureCode, bTooFast, BlockingLocation, BlockingObject);
        return false;
    };

    auto RecordRejectedMantle = [&](ENavLinkDirection::Type Direction, int32 FailureCode, const FVector& BlockingLocation, const FString& BlockingObject)
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

        FProjectedPair PreviewPair = Pair;
        PreviewPair.NativeDirection = Direction;
        PreviewPair.bUsesJumpTraversal = false;
        PreviewPair.bUsesMantleTraversal = true;
        PreviewPair.bUsesCustomTraversal = false;
        PreviewPair.ValidationFailureCode = static_cast<uint8>(FailureCode);
        PreviewPair.bValidationSpeedFailure = false;
        PreviewPair.bHasBlockingLocation = (FailureCode == 2 || FailureCode == 3 || FailureCode == 5) && (!BlockingLocation.IsNearlyZero() || !BlockingObject.IsEmpty());
        PreviewPair.BlockingLocation = BlockingLocation;
        PreviewPair.BlockingObject = BlockingObject;
        PreviewRejectedMantlePairs.Add(PreviewPair);
    };

    auto ValidateMantleDirection = [&](ENavLinkDirection::Type Direction) -> bool
    {
        const FVector StartWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.TargetWorld : Pair.SourceWorld;
        const FVector EndWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.SourceWorld : Pair.TargetWorld;
        int32 FailureCode = 0;
        FVector BlockingLocation = FVector::ZeroVector;
        FString BlockingObject;
        if (IsMantleDirectionValid(StartWorld, EndWorld, FailureCode, &BlockingLocation, &BlockingObject))
        {
            return true;
        }

        RecordRejectedMantle(Direction, FailureCode, BlockingLocation, BlockingObject);
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
                const FVector MantleStartWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.TargetWorld : Pair.SourceWorld;
                const FVector MantleEndWorld = Direction == ENavLinkDirection::RightToLeft ? Pair.SourceWorld : Pair.TargetWorld;
                int32 FailureCode = 0;
                FVector BlockingLocation = FVector::ZeroVector;
                FString BlockingObject;
                IsMantleDirectionValid(MantleStartWorld, MantleEndWorld, FailureCode, &BlockingLocation, &BlockingObject);
                RecordRejectedMantle(Direction, FailureCode, BlockingLocation, BlockingObject);
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
        if (bAdded && UsesTraversalAction(EOmniTraversalAction::Automatic) && PreviewGeneratedPairs.Num() > 0)
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

            PreviewGeneratedPairs.Last().bAutomaticMantleFallback = bAutomaticMantleFallback;
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

bool AOmniSplineNavLink::IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode, bool& bOutTooFast, FVector* OutBlockingLocation, FString* OutBlockingObject) const
{
    OutFailureCode = 0;
    bOutTooFast = false;

    FOmniJumpValidationSettings Settings;
    Settings.bValidatePath = bValidateJumpPath;
    Settings.ApexHeight = JumpApexHeight;
    Settings.AgentRadius = JumpValidationAgentRadius;
    Settings.AgentHalfHeight = JumpValidationAgentHalfHeight;
    Settings.MaxHorizontalSpeed = MaximumJumpHorizontalSpeed;
    Settings.ArcSegments = JumpValidationSegments;

    const EOmniJumpValidationResult Result = OmniTraversal::ValidateJumpPath(GetWorld(), this, StartWorld, EndWorld, Settings, OutBlockingLocation, OutBlockingObject);
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

bool AOmniSplineNavLink::IsMantleDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode, FVector* OutBlockingLocation, FString* OutBlockingObject) const
{
    OutFailureCode = 0;

    FOmniMantleValidationSettings Settings;
    Settings.bValidatePath = bValidateMantlePath;
    Settings.ArcHeight = MantleArcHeight;
    Settings.AgentRadius = JumpValidationAgentRadius;
    Settings.AgentHalfHeight = JumpValidationAgentHalfHeight;
    Settings.PathSegments = MantleValidationSegments;

    const EOmniMantleValidationResult Result = OmniTraversal::ValidateMantlePath(GetWorld(), this, StartWorld, EndWorld, Settings, OutBlockingLocation, OutBlockingObject);
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

bool AOmniSplineNavLink::UsesTraversalAction(EOmniTraversalAction Action) const
{
    if (!bUseDirectionalTraversalActions)
    {
        return TraversalAction == Action;
    }

    return SourceToTargetTraversalAction == Action
        || TargetToSourceTraversalAction == Action;
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
        if ((RejectedJumpDepartureSupportCount + RejectedJumpDepartureClearanceCount + RejectedJumpPathCount + RejectedJumpLandingSupportCount + RejectedJumpLandingClearanceCount + RejectedJumpSpeedCount + RejectedJumpInvalidCount) > 0)
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
            Status += FString::Printf(TEXT(" %d bidirectional jump pair%s reduced to one-way by validation."), JumpRestrictedToOneWayCount, JumpRestrictedToOneWayCount == 1 ? TEXT(" was") : TEXT("s were"));
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
    MantleLinkCount = 0;
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
    MergedDuplicateCount = 0;
    bSampleCapReached = false;
    bMatchedGridFallbackToNearest = false;
    PreviewSourceSamples.Reset();
    PreviewTargetSamples.Reset();
    PreviewGeneratedPairs.Reset();
    PreviewPairMergeBuckets.Reset();
    PreviewRejectedJumpPairs.Reset();
    PreviewRejectedMantlePairs.Reset();
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

    auto GetValidationLabel = [&](uint8 FailureCode, bool bSpeedFailure, const FString& BlockingObject) -> FString
    {
        FString Reason = GetValidationReason(FailureCode, bSpeedFailure);
        if ((FailureCode == 2 || FailureCode == 3 || FailureCode == 5) && !BlockingObject.IsEmpty())
        {
            Reason += FString::Printf(TEXT(" [%s]"), *BlockingObject);
        }
        return Reason;
    };

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

        if (Pair.bAutomaticMantleFallback)
        {
            OmniPreview::DrawFallbackMarker(World, Pair.SourceWorld, Pair.TargetWorld, PreviewOffset);
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
            OmniPreview::DrawRejectedReasonLabel(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                GetValidationLabel(Pair.ValidationFailureCode, Pair.bValidationSpeedFailure, Pair.BlockingObject),
                PreviewOffset);
            OmniPreview::DrawRejectedFailureMarker(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                Pair.ValidationFailureCode,
                Pair.bValidationSpeedFailure,
                false,
                JumpApexHeight,
                PreviewOffset,
                Pair.bHasBlockingLocation,
                Pair.BlockingLocation);
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
            OmniPreview::DrawRejectedReasonLabel(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                GetValidationLabel(Pair.ValidationFailureCode, false, Pair.BlockingObject),
                PreviewOffset);
            OmniPreview::DrawRejectedFailureMarker(
                World,
                Pair.SourceWorld,
                Pair.TargetWorld,
                Pair.NativeDirection,
                Pair.ValidationFailureCode,
                false,
                true,
                MantleArcHeight,
                PreviewOffset,
                Pair.bHasBlockingLocation,
                Pair.BlockingLocation);
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
void AOmniSplineNavLink::QueueEditorRegeneration()
{
    bEditorRegenerationPending = true;
}

void AOmniSplineNavLink::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    UnbindNavigationGenerationDelegate();
    BindNavigationGenerationDelegate();
    ApplyEditorVisualStyle();

    const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
    const FName DirectionalActionsName = GET_MEMBER_NAME_CHECKED(AOmniSplineNavLink, bUseDirectionalTraversalActions);
    const FName AutoRegenerateName = GET_MEMBER_NAME_CHECKED(AOmniSplineNavLink, bAutoRegenerate);

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
            QueueEditorRegeneration();
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
        QueueEditorRegeneration();
    }
}

void AOmniSplineNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    ApplyEditorVisualStyle();
    RegenerateLinks();
}
#endif
