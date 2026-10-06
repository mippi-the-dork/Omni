#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "GameFramework/Actor.h"
#include "OmniVolumeNavLink.generated.h"

class ANavigationData;
class UBoxComponent;
class UNavAreaBase;
class UNavLinkComponent;
class USceneComponent;

/**
 * Designer-authored volume-to-volume navigation link.
 *
 * Omni samples the horizontal footprint of SourceBox and TargetBox as 2D grids,
 * projects candidates to the current NavMesh, pairs each valid sample with the
 * nearest valid sample in the opposite volume, and publishes the surviving
 * results as ordinary FNavigationLink entries through a native UNavLinkComponent.
 */
UCLASS(Blueprintable)
class OMNI_API AOmniVolumeNavLink : public AActor
{
    GENERATED_BODY()

public:
    AOmniVolumeNavLink();

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void PostUnregisterAllComponents() override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditUndo() override;
#endif

    /** Rebuild generated native links from the current box transforms and NavMesh. */
    UFUNCTION(CallInEditor, Category="Omni|Generation", meta=(DisplayName="Regenerate Links"))
    void RegenerateLinks();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UBoxComponent> SourceBox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UBoxComponent> TargetBox;

    /** Native Unreal component containing Omni's generated FNavigationLink array. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UNavLinkComponent> GeneratedLinks;

    /** Enables or disables all generated links for this Omni actor. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation")
    bool bEnabled = true;

    /** Approximate world-space distance between candidate grid points in each volume. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation", meta=(ClampMin="10.0", UIMin="25.0", UIMax="500.0", Units="cm"))
    float LinkSpacing = 100.0f;

    /** Maximum world-space distance allowed between projected source and target endpoints. Zero disables this limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation", meta=(ClampMin="0.0", UIMin="0.0", UIMax="5000.0", Units="cm"))
    float MaximumLinkDistance = 2000.0f;

    /** Extent used by ProjectPointToNavigation around every candidate endpoint. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Projection", meta=(ClampMin="0.0"))
    FVector ProjectionExtent = FVector(75.0f, 75.0f, 200.0f);

    /** Projected points must remain within their authoring volume, with this extra tolerance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Projection", meta=(ClampMin="0.0", Units="cm"))
    float ProjectionContainmentTolerance = 25.0f;

    /** Reject projections that move farther than this from the raw candidate. Zero disables this limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Projection", meta=(ClampMin="0.0", Units="cm"))
    float MaximumProjectionDistance = 300.0f;

    /** Native Recast snap radius assigned to each generated link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Navigation", meta=(ClampMin="1.0", Units="cm"))
    float SnapRadius = 30.0f;

    /** Native downward endpoint projection used when Recast attaches generated links to polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Navigation", meta=(ClampMin="0.0", Units="cm"))
    float NativeProjectionHeight = 100.0f;

    /** Traversal direction applied to every generated native link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Navigation")
    TEnumAsByte<ENavLinkDirection::Type> Direction = ENavLinkDirection::BothWays;

    /** Navigation area class applied to every generated native link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Navigation")
    TSubclassOf<UNavAreaBase> AreaClass;

    /** Nav agents allowed to use generated links. */
    UPROPERTY(EditAnywhere, Category="Omni|Navigation")
    FNavAgentSelector SupportedAgents;

    /** Candidates whose endpoints are nearly identical to an existing link are merged. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Optimization", meta=(ClampMin="0.0", UIMin="0.0", UIMax="250.0", Units="cm"))
    float EndpointMergeDistance = 25.0f;

    /** Hard safety cap for projected grid samples gathered from each volume before pairing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Optimization", meta=(ClampMin="1", ClampMax="4096", UIMin="16", UIMax="1024"))
    int32 MaximumGridSamplesPerVolume = 512;

    /** Hard safety cap for generated native links. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Optimization", meta=(ClampMin="1", ClampMax="1024", UIMin="1", UIMax="512"))
    int32 MaximumGeneratedLinks = 256;

    /** Regenerate after Unreal reports that navigation generation has completed. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation")
    bool bRegenerateAfterNavigationBuild = true;

    /** Current number of generated native links. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 GeneratedLinkCount = 0;

    /** Raw grid candidates generated inside Source Box before NavMesh projection. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 SourceGridCandidateCount = 0;

    /** Raw grid candidates generated inside Target Box before NavMesh projection. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 TargetGridCandidateCount = 0;

    /** Source grid candidates that successfully projected to NavMesh inside Source Box. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 ValidSourceSampleCount = 0;

    /** Target grid candidates that successfully projected to NavMesh inside Target Box. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 ValidTargetSampleCount = 0;

    /** Number of projected source/target pairs evaluated in the most recent generation pass. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 CandidateCount = 0;

    /** Source grid candidates rejected because they could not project to NavMesh. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 RejectedSourceProjectionCount = 0;

    /** Target grid candidates rejected because they could not project to NavMesh. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 RejectedTargetProjectionCount = 0;

    /** Grid candidates rejected because the projected point left its authoring volume. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 RejectedOutsideVolumeCount = 0;

    /** Candidate pairs rejected by MaximumLinkDistance. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 RejectedDistanceCount = 0;

    /** Candidate pairs merged with an existing link. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 MergedDuplicateCount = 0;

private:
    struct FProjectedGridSample
    {
        FVector ProjectedWorld = FVector::ZeroVector;
    };

    struct FProjectedPair
    {
        FVector SourceWorld = FVector::ZeroVector;
        FVector TargetWorld = FVector::ZeroVector;
    };

    bool BuildProjectedGrid(const UBoxComponent& Box, bool bSourceVolume, TArray<FProjectedGridSample>& OutSamples);
    bool ProjectCandidate(const FVector& CandidateWorld, const UBoxComponent& OwnerBox, FVector& OutProjectedWorld, bool& bOutOutsideVolume) const;
    bool IsProjectedPointInsideBox(const FVector& WorldPoint, const UBoxComponent& Box) const;
    void BuildNearestPairs(const TArray<FProjectedGridSample>& SourceSamples, const TArray<FProjectedGridSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const;
    bool AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks);
    bool AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const;
    static int32 CalculateAxisSampleCount(double WorldLength, double Spacing);
    static int32 FindNearestSampleIndex(const FVector& Point, const TArray<FProjectedGridSample>& Samples);
    void ResetDebugCounters();
    void NotifyNavigationSystem();
    void BindNavigationGenerationDelegate();
    void UnbindNavigationGenerationDelegate();

    UFUNCTION()
    void HandleNavigationGenerationFinished(ANavigationData* NavData);

    bool bIsRegenerating = false;
};
