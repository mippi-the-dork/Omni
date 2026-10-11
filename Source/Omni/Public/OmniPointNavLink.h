#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "GameFramework/Actor.h"
#include "OmniTypes.h"
#include "OmniPointNavLink.generated.h"

class ANavigationData;
class UNavAreaBase;
class UNavLinkComponent;
class UOmniJumpTraversalComponent;
class UOmniCustomTraversalComponent;
class USceneComponent;
class USphereComponent;

/** User-facing traversal direction. Source is always Source Point, Target is always Target Point. */
UENUM(BlueprintType)
enum class EOmniPointLinkDirection : uint8
{
    BothWays UMETA(DisplayName="Both Ways"),
    SourceToTarget UMETA(DisplayName="Source to Target"),
    TargetToSource UMETA(DisplayName="Target to Source")
};

/**
 * Enhanced point-to-point navigation link using Omni's traversal stack.
 *
 * This is the direct replacement for the common one-link authoring case. Designers
 * position one Source Point and one Target Point, while Omni handles NavMesh
 * projection, directional height limits, Automatic traversal classification,
 * native path validation, Jump path validation, path cost, runtime Jump execution,
 * diagnostics, and selected-actor preview.
 */
UCLASS(Blueprintable)
class OMNI_API AOmniPointNavLink : public AActor
{
    GENERATED_BODY()

public:
    AOmniPointNavLink();

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void PostUnregisterAllComponents() override;
    virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditUndo() override;
#endif

    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Regenerate Link"))
    void RegenerateLink();

    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Swap Source and Target"))
    void SwapSourceAndTarget();

    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Reset Points"))
    void ResetPoints();

    // ---------------------------------------------------------------------
    // Normal setup
    // ---------------------------------------------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayPriority="1"))
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Direction", DisplayPriority="2"))
    EOmniPointLinkDirection LinkDirection = EOmniPointLinkDirection::BothWays;

    /** Longest Source-to-Target crossing Omni may create. Zero disables this limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Max Crossing Distance", ClampMin="0.0", UIMin="0.0", UIMax="5000.0", Units="cm", DisplayPriority="3"))
    float MaximumLinkDistance = 2000.0f;

    // ---------------------------------------------------------------------
    // Traversal
    // ---------------------------------------------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Action", DisplayPriority="1"))
    EOmniTraversalAction TraversalAction = EOmniTraversalAction::NativeMovement;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Use Height Limits", DisplayPriority="2"))
    bool bUseHeightLimits = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Rise", ClampMin="0.0", UIMin="0.0", UIMax="1000.0", Units="cm", EditCondition="bUseHeightLimits", EditConditionHides, DisplayPriority="3"))
    float MaximumRise = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Drop", ClampMin="0.0", UIMin="0.0", UIMax="2000.0", Units="cm", EditCondition="bUseHeightLimits", EditConditionHides, DisplayPriority="4"))
    float MaximumDrop = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Apex Height", ClampMin="1.0", UIMin="25.0", UIMax="500.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, DisplayPriority="5"))
    float JumpApexHeight = 150.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Validate Jump Path", EditCondition="TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, DisplayPriority="6"))
    bool bValidateJumpPath = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Jump Horizontal Speed", ClampMin="0.0", UIMin="0.0", UIMax="3000.0", Units="cm/s", EditCondition="bValidateJumpPath && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)", EditConditionHides, AdvancedDisplay))
    float MaximumJumpHorizontalSpeed = 1600.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Agent Radius", ClampMin="1.0", UIMin="10.0", UIMax="100.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic || (bValidateJumpPath && TraversalAction == EOmniTraversalAction::Jump)", EditConditionHides, AdvancedDisplay))
    float TraversalAgentRadius = 34.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Agent Half Height", ClampMin="1.0", UIMin="20.0", UIMax="200.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic || (bValidateJumpPath && TraversalAction == EOmniTraversalAction::Jump)", EditConditionHides, AdvancedDisplay))
    float TraversalAgentHalfHeight = 88.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Validation Segments", ClampMin="4", ClampMax="64", UIMin="8", UIMax="32", EditCondition="bValidateJumpPath && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)", EditConditionHides, AdvancedDisplay))
    int32 JumpValidationSegments = 16;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Area Class", EditCondition="TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, AdvancedDisplay))
    TSubclassOf<UNavAreaBase> JumpAreaClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Landing Tolerance", ClampMin="1.0", UIMin="25.0", UIMax="250.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, AdvancedDisplay))
    float JumpLandingTolerance = 75.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Timeout", ClampMin="0.25", UIMin="0.5", UIMax="10.0", Units="s", EditCondition="TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, AdvancedDisplay))
    float JumpTraversalTimeout = 3.0f;

    /** Safety timeout for designer-driven Custom traversal requests. If it expires, Omni resumes path following to avoid a permanent stall. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Custom Traversal Timeout", ClampMin="0.25", UIMin="0.5", UIMax="30.0", Units="s", EditCondition="TraversalAction == EOmniTraversalAction::Custom", EditConditionHides, DisplayPriority="5"))
    float CustomTraversalTimeout = 5.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Validate Native Path", EditCondition="TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, DisplayPriority="6"))
    bool bValidateAutomaticNativePath = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Max Step Height", ClampMin="0.0", UIMin="0.0", UIMax="100.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic && bValidateAutomaticNativePath", EditConditionHides, DisplayPriority="7"))
    float AutomaticMaxStepHeight = 45.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Max Ground Slope", ClampMin="0.0", ClampMax="89.0", UIMin="0.0", UIMax="60.0", Units="deg", EditCondition="TraversalAction == EOmniTraversalAction::Automatic && bValidateAutomaticNativePath", EditConditionHides, DisplayPriority="8"))
    float AutomaticMaxGroundSlopeDegrees = 44.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Rise Threshold", ClampMin="0.0", UIMin="0.0", UIMax="100.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, DisplayPriority="9"))
    float AutomaticRiseThreshold = 10.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Native Drop Distance", ClampMin="0.0", UIMin="0.0", UIMax="1000.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, DisplayPriority="10"))
    float AutomaticNativeDropMaxDistance = 200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Ground Probe Depth", ClampMin="1.0", UIMin="25.0", UIMax="300.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, AdvancedDisplay))
    float AutomaticGroundProbeDepth = 75.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Ground Probe Spacing", ClampMin="10.0", UIMin="25.0", UIMax="250.0", Units="cm", EditCondition="TraversalAction == EOmniTraversalAction::Automatic", EditConditionHides, AdvancedDisplay))
    float AutomaticGroundProbeSpacing = 75.0f;

    // ---------------------------------------------------------------------
    // Visualization and status
    // ---------------------------------------------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Visualization", meta=(DisplayName="Show Preview"))
    bool bShowPreview = true;


    /** Controls how much selected-actor preview information is drawn. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Visualization", meta=(DisplayName="Preview Detail", EditCondition="bShowPreview", EditConditionHides))
    EOmniPreviewDetail PreviewDetail = EOmniPreviewDetail::SamplesAndLinks;

    /** Legacy 0.11-0.13 setting retained so existing placed actors deserialize cleanly. */
    UPROPERTY()
    bool bShowRejectedJumps = true;

    UPROPERTY(VisibleAnywhere, Transient, Category="Omni|Status", meta=(DisplayName="Status"))
    FText GenerationStatus;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Generated Links"))
    int32 GeneratedLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Native Links"))
    int32 NativeLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Jump Links"))
    int32 JumpLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Custom Links"))
    int32 CustomLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Source On NavMesh"))
    bool bSourceOnNavMesh = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Target On NavMesh"))
    bool bTargetOnNavMesh = false;

    // ---------------------------------------------------------------------
    // Advanced
    // ---------------------------------------------------------------------

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    bool bAutoRegenerate = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    bool bRegenerateAfterNavigationBuild = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", AdvancedDisplay))
    FVector ProjectionExtent = FVector(75.0f, 75.0f, 200.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float MaximumProjectionDistance = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="1.0", Units="cm", AdvancedDisplay))
    float SnapRadius = 30.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float NativeProjectionHeight = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TSubclassOf<UNavAreaBase> AreaClass;

    UPROPERTY(EditAnywhere, Category="Omni|Advanced", meta=(AdvancedDisplay))
    FNavAgentSelector SupportedAgents;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay, DisplayName="Source Point"))
    TObjectPtr<USphereComponent> SourcePoint;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay, DisplayName="Target Point"))
    TObjectPtr<USphereComponent> TargetPoint;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UNavLinkComponent> GeneratedLinks;

    UPROPERTY(VisibleAnywhere, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UOmniJumpTraversalComponent> JumpTraversal;

    /** Runtime smart-link dispatcher used when Traversal Action is Custom. Bind OnTraversalRequested in Blueprint or C++. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UOmniCustomTraversalComponent> CustomTraversal;

    // ---------------------------------------------------------------------
    // Diagnostics
    // ---------------------------------------------------------------------

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Source Projection"))
    int32 RejectedSourceProjectionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Target Projection"))
    int32 RejectedTargetProjectionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Crossing Distance"))
    int32 RejectedDistanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Height Limited To One Way"))
    int32 HeightRestrictedLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Automatic Split Pair"))
    int32 AutomaticSplitPairCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Step Promotions"))
    int32 AutomaticStepPromotionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Slope Promotions"))
    int32 AutomaticSlopePromotionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Clearance Promotions"))
    int32 AutomaticBlockedPromotionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Arc"))
    int32 RejectedJumpArcCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Speed"))
    int32 RejectedJumpSpeedCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Jump Limited To One Way"))
    int32 JumpRestrictedToOneWayCount = 0;

private:
    struct FPreviewLink
    {
        FVector SourceWorld = FVector::ZeroVector;
        FVector TargetWorld = FVector::ZeroVector;
        ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
        bool bUsesJumpTraversal = false;
        bool bUsesCustomTraversal = false;
    };

    bool ProjectEndpoint(const FVector& CandidateWorld, bool bSource, FVector& OutProjectedWorld);
    bool GenerateProjectedConnection(const FVector& SourceWorld, const FVector& TargetWorld, TArray<FNavigationLink>& OutNativeLinks, TArray<FOmniJumpLinkSpec>& OutJumpLinks, TArray<FOmniCustomLinkSpec>& OutCustomLinks);
    bool IsTraversalAllowedByHeight(const FVector& StartWorld, const FVector& EndWorld) const;
    bool ResolvePairDirection(const FVector& SourceWorld, const FVector& TargetWorld, ENavLinkDirection::Type& OutDirection, bool& bOutHeightRestricted) const;
    bool IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, bool& bOutBlocked, bool& bOutTooFast) const;
    bool AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const;
    ENavLinkDirection::Type GetNativeDirection() const;
    void UpdateGenerationStatus();
    void ApplyEditorVisualStyle();
    void ResetDebugState();
    void NotifyNavigationSystem();
    void BindNavigationGenerationDelegate();
    void UnbindNavigationGenerationDelegate();

#if WITH_EDITOR
    void DrawEditorPreview() const;
#endif

    UFUNCTION()
    void HandleNavigationGenerationFinished(ANavigationData* NavData);

    FVector PreviewProjectedSource = FVector::ZeroVector;
    FVector PreviewProjectedTarget = FVector::ZeroVector;
    bool bHasPreviewProjectedSource = false;
    bool bHasPreviewProjectedTarget = false;
    TArray<FPreviewLink> PreviewGeneratedLinks;
    TArray<FPreviewLink> PreviewRejectedJumpLinks;

    bool bIsRegenerating = false;
};
