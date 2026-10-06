#include "OmniVolumeNavLink.h"

#include "AI/NavigationSystemBase.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavLinkComponent.h"

namespace OmniVolumeNavLinkPrivate
{
    constexpr float LinkComparisonTolerance = 0.1f;
}

AOmniVolumeNavLink::AOmniVolumeNavLink()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    SourceBox = CreateDefaultSubobject<UBoxComponent>(TEXT("SourceBox"));
    SourceBox->SetupAttachment(SceneRoot);
    SourceBox->SetRelativeLocation(FVector(-300.0, 0.0, 0.0));
    SourceBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));
    SourceBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SourceBox->SetGenerateOverlapEvents(false);
    SourceBox->SetCanEverAffectNavigation(false);

    TargetBox = CreateDefaultSubobject<UBoxComponent>(TEXT("TargetBox"));
    TargetBox->SetupAttachment(SceneRoot);
    TargetBox->SetRelativeLocation(FVector(300.0, 0.0, 0.0));
    TargetBox->SetBoxExtent(FVector(150.0, 300.0, 75.0));
    TargetBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TargetBox->SetGenerateOverlapEvents(false);
    TargetBox->SetCanEverAffectNavigation(false);

    GeneratedLinks = CreateDefaultSubobject<UNavLinkComponent>(TEXT("GeneratedLinks"));
    GeneratedLinks->SetupAttachment(SceneRoot);
    GeneratedLinks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    GeneratedLinks->SetGenerateOverlapEvents(false);

    AreaClass = UNavArea_Default::StaticClass();
    SupportedAgents = FNavAgentSelector(FNavAgentSelector::AllAgentsMask);
}

void AOmniVolumeNavLink::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RegenerateLinks();
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

void AOmniVolumeNavLink::RegenerateLinks()
{
    if (bIsRegenerating || !GeneratedLinks || !SourceBox || !TargetBox)
    {
        return;
    }

    TGuardValue<bool> RegenerationGuard(bIsRegenerating, true);
    ResetDebugCounters();

    TArray<FNavigationLink> NewLinks;

    if (bEnabled)
    {
        const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
        NewLinks.Reserve(FMath::Min(SafeMaximumLinks, 256));

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
            BuildNearestPairs(SourceSamples, TargetSamples, CandidatePairs);

            TBitArray<> AttemptedPairs(false, CandidatePairs.Num());
            const int32 SpreadAttemptCount = FMath::Min(SafeMaximumLinks, CandidatePairs.Num());
            for (int32 AttemptIndex = 0; AttemptIndex < SpreadAttemptCount && NewLinks.Num() < SafeMaximumLinks; ++AttemptIndex)
            {
                const int32 PairIndex = SpreadAttemptCount <= 1
                    ? CandidatePairs.Num() / 2
                    : FMath::RoundToInt(
                        static_cast<double>(AttemptIndex) * static_cast<double>(CandidatePairs.Num() - 1)
                        / static_cast<double>(SpreadAttemptCount - 1));

                AttemptedPairs[PairIndex] = true;
                AddProjectedPair(CandidatePairs[PairIndex], NewLinks);
            }

            // If distance rejection or duplicate merging left room under the cap, walk the
            // untried proposals once to fill any remaining capacity.
            for (int32 PairIndex = 0; PairIndex < CandidatePairs.Num() && NewLinks.Num() < SafeMaximumLinks; ++PairIndex)
            {
                if (!AttemptedPairs[PairIndex])
                {
                    AddProjectedPair(CandidatePairs[PairIndex], NewLinks);
                }
            }
        }
    }

    GeneratedLinkCount = NewLinks.Num();

    if (AreLinkArraysEquivalent(GeneratedLinks->Links, NewLinks))
    {
        return;
    }

    GeneratedLinks->Links = MoveTemp(NewLinks);
    GeneratedLinks->UpdateBounds();
    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

bool AOmniVolumeNavLink::BuildProjectedGrid(const UBoxComponent& Box, bool bSourceVolume, TArray<FProjectedGridSample>& OutSamples)
{
    OutSamples.Reset();

    const FVector Extent = Box.GetUnscaledBoxExtent();
    const FTransform BoxTransform = Box.GetComponentTransform();
    const FVector Scale = BoxTransform.GetScale3D();

    const double WorldLengthX = 2.0 * Extent.X * FMath::Abs(Scale.X);
    const double WorldLengthY = 2.0 * Extent.Y * FMath::Abs(Scale.Y);
    const double SafeSpacing = FMath::Max(10.0f, LinkSpacing);

    int32 SampleCountX = CalculateAxisSampleCount(WorldLengthX, SafeSpacing);
    int32 SampleCountY = CalculateAxisSampleCount(WorldLengthY, SafeSpacing);

    const int32 SafeMaximumSamples = FMath::Clamp(MaximumGridSamplesPerVolume, 1, 4096);
    const int64 InitialSampleCount = static_cast<int64>(SampleCountX) * static_cast<int64>(SampleCountY);

    if (InitialSampleCount > SafeMaximumSamples)
    {
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
            const int32 TargetIndex = FindNearestSampleIndex(SourceSample.ProjectedWorld, TargetSamples);
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
            const int32 SourceIndex = FindNearestSampleIndex(TargetSample.ProjectedWorld, SourceSamples);
            if (SourceIndex != INDEX_NONE)
            {
                FProjectedPair& Pair = OutPairs.Emplace_GetRef();
                Pair.SourceWorld = SourceSamples[SourceIndex].ProjectedWorld;
                Pair.TargetWorld = TargetSample.ProjectedWorld;
            }
        }
    }
}

bool AOmniVolumeNavLink::AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks)
{
    ++CandidateCount;

    const int32 SafeMaximumLinks = FMath::Clamp(MaximumGeneratedLinks, 1, 1024);
    if (InOutLinks.Num() >= SafeMaximumLinks)
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

    const double MergeDistanceSq = FMath::Square(static_cast<double>(FMath::Max(0.0f, EndpointMergeDistance)));
    const FTransform LinkTransform = GeneratedLinks->GetComponentTransform();
    const FVector SourceLocal = LinkTransform.InverseTransformPosition(Pair.SourceWorld);
    const FVector TargetLocal = LinkTransform.InverseTransformPosition(Pair.TargetWorld);

    if (MergeDistanceSq > 0.0)
    {
        for (const FNavigationLink& ExistingLink : InOutLinks)
        {
            if (FVector::DistSquared(ExistingLink.Left, SourceLocal) <= MergeDistanceSq
                && FVector::DistSquared(ExistingLink.Right, TargetLocal) <= MergeDistanceSq)
            {
                ++MergedDuplicateCount;
                return false;
            }
        }
    }

    UClass* ResolvedAreaClass = AreaClass.Get();
    if (!ResolvedAreaClass)
    {
        ResolvedAreaClass = UNavArea_Default::StaticClass();
    }

    FNavigationLink& NewLink = InOutLinks.Emplace_GetRef(SourceLocal, TargetLocal);
    NewLink.Direction = Direction;
    NewLink.SnapRadius = FMath::Max(1.0f, SnapRadius);
    NewLink.LeftProjectHeight = FMath::Max(0.0f, NativeProjectionHeight);
    NewLink.MaxFallDownLength = FMath::Max(0.0f, NativeProjectionHeight);
    NewLink.bUseSnapHeight = false;
    NewLink.bSnapToCheapestArea = false;
    NewLink.SupportedAgents = SupportedAgents;
    NewLink.SetAreaClass(ResolvedAreaClass);

    return true;
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

int32 AOmniVolumeNavLink::FindNearestSampleIndex(const FVector& Point, const TArray<FProjectedGridSample>& Samples)
{
    if (Samples.IsEmpty())
    {
        return INDEX_NONE;
    }

    int32 BestIndex = 0;
    double BestDistanceSq = FVector::DistSquared(Point, Samples[0].ProjectedWorld);

    for (int32 Index = 1; Index < Samples.Num(); ++Index)
    {
        const double DistanceSq = FVector::DistSquared(Point, Samples[Index].ProjectedWorld);
        if (DistanceSq < BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestIndex = Index;
        }
    }

    return BestIndex;
}

void AOmniVolumeNavLink::ResetDebugCounters()
{
    GeneratedLinkCount = 0;
    SourceGridCandidateCount = 0;
    TargetGridCandidateCount = 0;
    ValidSourceSampleCount = 0;
    ValidTargetSampleCount = 0;
    CandidateCount = 0;
    RejectedSourceProjectionCount = 0;
    RejectedTargetProjectionCount = 0;
    RejectedOutsideVolumeCount = 0;
    RejectedDistanceCount = 0;
    MergedDuplicateCount = 0;
}

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
    RegenerateLinks();
}

void AOmniVolumeNavLink::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);

    if (bFinished)
    {
        RegenerateLinks();
    }
}

void AOmniVolumeNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    RegenerateLinks();
}
#endif
