#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "GameFramework/Actor.h"
#include "OmniTypes.h"
#include "OmniVolumeNavLink.generated.h"

class ANavigationData;
class UBoxComponent;
class UNavAreaBase;
class UNavLinkComponent;
class UOmniJumpTraversalComponent;
class UOmniMantleTraversalComponent;
class UOmniCustomTraversalComponent;
class USceneComponent;

/** User-facing traversal direction. Source is always the Source Box, Target is always the Target Box. */
UENUM(BlueprintType)
enum class EOmniVolumeLinkDirection : uint8
{
    BothWays UMETA(DisplayName="Both Ways"),
    SourceToTarget UMETA(DisplayName="Source to Target"),
    TargetToSource UMETA(DisplayName="Target to Source")
};

/**
 * Designer-authored volume-to-volume navigation link.
 *
 * Normal workflow:
 * 1. Place the actor.
 * 2. Move/resize Source Box over one navigable region.
 * 3. Move/resize Target Box over the region it should connect to.
 * 4. Choose Direction, Pairing Mode, and Coverage Density.
 *
 * Omni projects a horizontal grid inside each volume to NavMesh, pairs valid
 * samples between the two regions, then publishes either ordinary native point
 * links or built-in Jump, Mantle, or Custom smart links according to Traversal Action.
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
    virtual void Tick(float DeltaSeconds) override;

#if WITH_EDITOR
    virtual bool ShouldTickIfViewportsOnly() const override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditUndo() override;
#endif

    /** Force an immediate refresh from the current Source/Target volumes. */
    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Regenerate Links"))
    void RegenerateLinks();

    /** Swap the physical Source and Target volume placements. Useful when authoring one-way traversal. */
    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Swap Source and Target"))
    void SwapSourceAndTarget();

    /** Copy Source Box size/scale to Target Box while preserving the Target position and rotation. */
    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Match Target Size to Source"))
    void MatchTargetSizeToSource();

    /** Restore the two authoring volumes to Omni's default placement and size. */
    UFUNCTION(CallInEditor, Category="Omni|Actions", meta=(DisplayName="Reset Volumes"))
    void ResetVolumes();

    /** Master switch for this Omni connection. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayPriority="1"))
    bool bEnabled = true;

    /** Traversal direction using the clearly named Source and Target volumes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Direction", DisplayPriority="2"))
    EOmniVolumeLinkDirection LinkDirection = EOmniVolumeLinkDirection::BothWays;

    /**
     * Chooses how valid Source and Target grid samples are paired.
     * Nearest is the general-purpose default. Matched Grid attempts one-to-one normalized grid correspondence.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Pairing Mode", DisplayPriority="3"))
    EOmniPairingMode PairingMode = EOmniPairingMode::Nearest;

    /**
     * Controls how densely Omni samples each volume.
     * Sparse = 200 cm, Balanced = 100 cm, Dense = 50 cm.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayPriority="4"))
    EOmniCoverageDensity CoverageDensity = EOmniCoverageDensity::Balanced;

    /** Used only when Coverage Density is Custom. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Custom Spacing", ClampMin="10.0", UIMin="25.0", UIMax="500.0", Units="cm", EditCondition="CoverageDensity == EOmniCoverageDensity::Custom", EditConditionHides, DisplayPriority="5"))
    float LinkSpacing = 100.0f;

    /** Longest Source-to-Target crossing Omni may create. Zero disables this limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Setup", meta=(DisplayName="Max Crossing Distance", ClampMin="0.0", UIMin="0.0", UIMax="5000.0", Units="cm", DisplayPriority="6"))
    float MaximumLinkDistance = 2000.0f;

    /** Native Movement uses ordinary links. Jump and Mantle use built-in smart-link traversal. Automatic chooses Native, Mantle, or Jump per direction. Custom hands smart-link traversal to Blueprint or C++. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Use Directional Actions", DisplayPriority="1"))
    bool bUseDirectionalTraversalActions = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Action", EditCondition="!bUseDirectionalTraversalActions", EditConditionHides, DisplayPriority="2"))
    EOmniTraversalAction TraversalAction = EOmniTraversalAction::NativeMovement;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Source to Target Action", EditCondition="bUseDirectionalTraversalActions && LinkDirection != EOmniVolumeLinkDirection::TargetToSource", EditConditionHides, DisplayPriority="2"))
    EOmniTraversalAction SourceToTargetTraversalAction = EOmniTraversalAction::NativeMovement;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Target to Source Action", EditCondition="bUseDirectionalTraversalActions && LinkDirection != EOmniVolumeLinkDirection::SourceToTarget", EditConditionHides, DisplayPriority="3"))
    EOmniTraversalAction TargetToSourceTraversalAction = EOmniTraversalAction::NativeMovement;

    /** Enable directional vertical filtering for generated crossings. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Use Height Limits", DisplayPriority="4"))
    bool bUseHeightLimits = false;

    /** Maximum upward movement allowed in a traversal direction. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Rise", ClampMin="0.0", UIMin="0.0", UIMax="1000.0", Units="cm", EditCondition="bUseHeightLimits", EditConditionHides, DisplayPriority="5"))
    float MaximumRise = 100.0f;

    /** Maximum downward movement allowed in a traversal direction. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Drop", ClampMin="0.0", UIMin="0.0", UIMax="2000.0", Units="cm", EditCondition="bUseHeightLimits", EditConditionHides, DisplayPriority="6"))
    float MaximumDrop = 300.0f;

    /** Extra height above the higher endpoint used to shape Omni's ballistic jump. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Apex Height", ClampMin="1.0", UIMin="25.0", UIMax="500.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="7"))
    float JumpApexHeight = 150.0f;

    /** Reject Jump directions whose predicted Character capsule arc is blocked or exceeds the configured speed limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Validate Jump Path", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="8"))
    bool bValidateJumpPath = true;

    /** Maximum predicted horizontal launch speed. Zero disables the speed limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Max Jump Horizontal Speed", ClampMin="0.0", UIMin="0.0", UIMax="3000.0", Units="cm/s", EditCondition="bValidateJumpPath && ((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float MaximumJumpHorizontalSpeed = 1600.0f;

    /** Capsule radius used by Automatic native-path, Jump, and Mantle clearance validation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Agent Radius", ClampMin="1.0", UIMin="10.0", UIMax="100.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Automatic || TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Mantle)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle)))", EditConditionHides, AdvancedDisplay))
    float JumpValidationAgentRadius = 34.0f;

    /** Capsule half height used by Automatic native-path, Jump, and Mantle clearance validation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Traversal Agent Half Height", ClampMin="1.0", UIMin="20.0", UIMax="200.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Automatic || TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Mantle)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle)))", EditConditionHides, AdvancedDisplay))
    float JumpValidationAgentHalfHeight = 88.0f;

    /** Number of capsule sweeps used to validate each ballistic arc. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Validation Segments", ClampMin="4", ClampMax="64", UIMin="8", UIMax="32", EditCondition="bValidateJumpPath && ((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    int32 JumpValidationSegments = 16;

    /** Navigation area applied to Jump smart links. Omni Jump is higher cost than normal walkable navigation by default. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Area Class", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    TSubclassOf<UNavAreaBase> JumpAreaClass;

    /** How close the Character must get to the destination before normal path following resumes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Landing Tolerance", ClampMin="1.0", UIMin="25.0", UIMax="250.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float JumpLandingTolerance = 75.0f;

    /** Safety timeout that prevents a failed jump from holding path following forever. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Jump Timeout", ClampMin="0.25", UIMin="0.5", UIMax="10.0", Units="s", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Jump || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Jump || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Jump || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float JumpTraversalTimeout = 3.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Duration", ClampMin="0.05", UIMin="0.15", UIMax="2.0", Units="s", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="9"))
    float MantleDuration = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Arc Height", ClampMin="0.0", UIMin="0.0", UIMax="150.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="10"))
    float MantleArcHeight = 35.0f;

    /** Rejects built-in Mantle directions whose swept traversal capsule is blocked by WorldStatic geometry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Validate Mantle Path", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="11"))
    bool bValidateMantlePath = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Validation Segments", ClampMin="6", ClampMax="64", UIMin="8", UIMax="32", EditCondition="bValidateMantlePath && ((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    int32 MantleValidationSegments = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Landing Tolerance", ClampMin="1.0", UIMin="5.0", UIMax="100.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float MantleLandingTolerance = 20.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Timeout", ClampMin="0.25", UIMin="0.5", UIMax="10.0", Units="s", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float MantleTraversalTimeout = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Mantle Area Class", EditCondition="((!bUseDirectionalTraversalActions && (TraversalAction == EOmniTraversalAction::Mantle || TraversalAction == EOmniTraversalAction::Automatic)) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Mantle || SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Mantle || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    TSubclassOf<UNavAreaBase> MantleAreaClass;

    /** Safety timeout for designer-driven Custom traversal requests. If it expires, Omni resumes path following to avoid a permanent stall. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Custom Traversal Timeout", ClampMin="0.25", UIMin="0.5", UIMax="30.0", Units="s", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Custom) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Custom || TargetToSourceTraversalAction == EOmniTraversalAction::Custom)))", EditConditionHides, DisplayPriority="9"))
    float CustomTraversalTimeout = 5.0f;

    /** Identifier included with non-directional Custom traversal requests so project logic can route the request without inspecting geometry. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Custom Traversal ID", EditCondition="!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Custom", EditConditionHides, DisplayPriority="10"))
    FName CustomTraversalId = NAME_None;

    /** Identifier reported when a directional Custom request travels from Source toward Target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Source to Target Custom ID", EditCondition="bUseDirectionalTraversalActions && LinkDirection != EOmniVolumeLinkDirection::TargetToSource && SourceToTargetTraversalAction == EOmniTraversalAction::Custom", EditConditionHides, DisplayPriority="10"))
    FName SourceToTargetCustomTraversalId = NAME_None;

    /** Identifier reported when a directional Custom request travels from Target toward Source. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Target to Source Custom ID", EditCondition="bUseDirectionalTraversalActions && LinkDirection != EOmniVolumeLinkDirection::SourceToTarget && TargetToSourceTraversalAction == EOmniTraversalAction::Custom", EditConditionHides, DisplayPriority="11"))
    FName TargetToSourceCustomTraversalId = NAME_None;

    /** Validate whether continuously supported ground is actually walkable before Automatic keeps a Native link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Validate Native Path", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="10"))
    bool bValidateAutomaticNativePath = true;

    /** Largest single ground-height change Automatic considers ordinary walkable movement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Max Step Height", ClampMin="0.0", UIMin="0.0", UIMax="100.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic))) && bValidateAutomaticNativePath", EditConditionHides, DisplayPriority="11"))
    float AutomaticMaxStepHeight = 45.0f;

    /** Steepest supported ground angle Automatic considers ordinary walkable movement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Max Ground Slope", ClampMin="0.0", ClampMax="89.0", UIMin="0.0", UIMax="60.0", Units="deg", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic))) && bValidateAutomaticNativePath", EditConditionHides, DisplayPriority="12"))
    float AutomaticMaxGroundSlopeDegrees = 44.0f;

    /** In Automatic mode, upward crossings above this threshold require special traversal. Short rises may become Mantle; others become Jump. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Rise Threshold", ClampMin="0.0", UIMin="0.0", UIMax="100.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="13"))
    float AutomaticRiseThreshold = 10.0f;

    /** In Automatic mode, unsupported downward crossings no longer than this may remain Native drops. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Native Drop Distance", ClampMin="0.0", UIMin="0.0", UIMax="1000.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="14"))
    float AutomaticNativeDropMaxDistance = 200.0f;

    /** Allow Automatic to classify short upward ledges as built-in Mantle instead of Jump. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Use Mantle", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, DisplayPriority="15"))
    bool bAutomaticUseMantle = true;

    /** Require Automatic Mantle candidates to have a facing ledge plus a usable, capsule-clear top near the destination height. Explicit Mantle is unaffected. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Require Ledge Geometry", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic))) && bAutomaticUseMantle", EditConditionHides, DisplayPriority="16"))
    bool bAutomaticRequireMantleLedge = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Mantle Max Height", ClampMin="0.0", UIMin="50.0", UIMax="250.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic))) && bAutomaticUseMantle", EditConditionHides, DisplayPriority="17"))
    float AutomaticMantleMaxHeight = 150.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Mantle Max Reach", ClampMin="0.0", UIMin="25.0", UIMax="300.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic))) && bAutomaticUseMantle", EditConditionHides, DisplayPriority="18"))
    float AutomaticMantleMaxReach = 120.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Ground Probe Depth", ClampMin="1.0", UIMin="25.0", UIMax="300.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float AutomaticGroundProbeDepth = 75.0f;

    /** Spacing between physical ground-support probes in Automatic mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Traversal", meta=(DisplayName="Auto Ground Probe Spacing", ClampMin="10.0", UIMin="25.0", UIMax="250.0", Units="cm", EditCondition="((!bUseDirectionalTraversalActions && TraversalAction == EOmniTraversalAction::Automatic) || (bUseDirectionalTraversalActions && (SourceToTargetTraversalAction == EOmniTraversalAction::Automatic || TargetToSourceTraversalAction == EOmniTraversalAction::Automatic)))", EditConditionHides, AdvancedDisplay))
    float AutomaticGroundProbeSpacing = 75.0f;

    /** Draw projected samples and the actual generated pairings while this actor is selected. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Visualization", meta=(DisplayName="Show Preview"))
    bool bShowPreview = true;


    /** Controls how much selected-actor preview information is drawn. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Visualization", meta=(DisplayName="Preview Detail", EditCondition="bShowPreview", EditConditionHides))
    EOmniPreviewDetail PreviewDetail = EOmniPreviewDetail::SamplesAndLinks;

    /** Legacy 0.11-0.13 setting retained so existing placed actors deserialize cleanly. */
    UPROPERTY()
    bool bShowRejectedJumps = true;

    /** Plain-English result of the most recent generation pass. */
    UPROPERTY(VisibleAnywhere, Transient, Category="Omni|Status", meta=(DisplayName="Status"))
    FText GenerationStatus;

    /** Current total number of navigation links generated by this actor. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Generated Links"))
    int32 GeneratedLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Native Links"))
    int32 NativeLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Jump Links"))
    int32 JumpLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Mantle Links"))
    int32 MantleLinkCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Custom Links"))
    int32 CustomLinkCount = 0;

    /** Valid NavMesh samples found inside Source Box. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Source Nav Samples"))
    int32 ValidSourceSampleCount = 0;

    /** Valid NavMesh samples found inside Target Box. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Status", meta=(DisplayName="Target Nav Samples"))
    int32 ValidTargetSampleCount = 0;

    // ---------------------------------------------------------------------
    // Advanced authoring. Defaults should be appropriate for most actors.
    // ---------------------------------------------------------------------

    /** Automatically regenerate after editor property/component changes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    bool bAutoRegenerate = true;

    /** Re-evaluate this actor after Unreal finishes rebuilding navigation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    bool bRegenerateAfterNavigationBuild = true;

    /** Extent used by ProjectPointToNavigation around each candidate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", AdvancedDisplay))
    FVector ProjectionExtent = FVector(75.0f, 75.0f, 200.0f);

    /** Projected points may exceed the exact authoring box by this small tolerance. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float ProjectionContainmentTolerance = 25.0f;

    /** Reject projections that move farther than this from the raw grid point. Zero disables this limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float MaximumProjectionDistance = 300.0f;

    /** Native Recast snap radius assigned to each generated link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="1.0", Units="cm", AdvancedDisplay))
    float SnapRadius = 30.0f;

    /** Native downward endpoint projection used when Recast attaches links to polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", Units="cm", AdvancedDisplay))
    float NativeProjectionHeight = 100.0f;

    /** Navigation area class applied to generated links. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TSubclassOf<UNavAreaBase> AreaClass;

    /** Nav agents allowed to use generated links. */
    UPROPERTY(EditAnywhere, Category="Omni|Advanced", meta=(AdvancedDisplay))
    FNavAgentSelector SupportedAgents;

    /** Candidate endpoint pairs inside this distance are merged. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="0.0", UIMin="0.0", UIMax="250.0", Units="cm", AdvancedDisplay))
    float EndpointMergeDistance = 25.0f;

    /** Safety cap for projected grid samples gathered from each volume. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="1", ClampMax="4096", UIMin="16", UIMax="1024", AdvancedDisplay))
    int32 MaximumGridSamplesPerVolume = 512;

    /** Safety cap for native links generated by one Omni actor. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Advanced", meta=(ClampMin="1", ClampMax="1024", UIMin="1", UIMax="512", AdvancedDisplay))
    int32 MaximumGeneratedLinks = 256;

    // ---------------------------------------------------------------------
    // Diagnostics. Useful when Status says coverage could not be generated.
    // ---------------------------------------------------------------------

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 SourceGridCandidateCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 TargetGridCandidateCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 CandidateCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 RejectedSourceProjectionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 RejectedTargetProjectionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 RejectedOutsideVolumeCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 RejectedDistanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Height Limited To One Way"))
    int32 HeightRestrictedLinkCount = 0;

    /** Both Ways pairs that Automatic traversal split because each direction resolved to a different traversal action. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Automatic Split Pairs"))
    int32 AutomaticSplitPairCount = 0;

    /** Both Ways pairs split because Source-to-Target and Target-to-Source use different explicit traversal actions. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Directional Split Pairs"))
    int32 DirectionalSplitPairCount = 0;

    /** Automatic directions promoted to Jump because the supported ground changed height too abruptly. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Step Promotions"))
    int32 AutomaticStepPromotionCount = 0;

    /** Automatic directions promoted to Jump because supported ground exceeded Auto Max Ground Slope. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Slope Promotions"))
    int32 AutomaticSlopePromotionCount = 0;

    /** Automatic directions promoted to Jump because the standing capsule did not have clear space. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Clearance Promotions"))
    int32 AutomaticBlockedPromotionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Mantle Promotions"))
    int32 AutomaticMantlePromotionCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Mantle Validation Fallbacks"))
    int32 AutomaticMantleValidationFallbackCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Auto Ledge Geometry Fallbacks"))
    int32 AutomaticMantleLedgeFallbackCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Departure Support"))
    int32 RejectedJumpDepartureSupportCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Departure Clearance"))
    int32 RejectedJumpDepartureClearanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Path"))
    int32 RejectedJumpPathCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Landing Support"))
    int32 RejectedJumpLandingSupportCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Landing Clearance"))
    int32 RejectedJumpLandingClearanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Speed"))
    int32 RejectedJumpSpeedCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Jump Invalid"))
    int32 RejectedJumpInvalidCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Jump Limited To One Way"))
    int32 JumpRestrictedToOneWayCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Departure Support"))
    int32 RejectedMantleDepartureSupportCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Departure Clearance"))
    int32 RejectedMantleDepartureClearanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Path"))
    int32 RejectedMantlePathCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Landing Support"))
    int32 RejectedMantleLandingSupportCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Landing Clearance"))
    int32 RejectedMantleLandingClearanceCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Rejected Mantle Invalid"))
    int32 RejectedMantleInvalidCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Mantle Limited To One Way"))
    int32 MantleRestrictedToOneWayCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay))
    int32 MergedDuplicateCount = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Diagnostics", meta=(AdvancedDisplay, DisplayName="Grid Sample Cap Reached"))
    bool bGridSampleCapReached = false;

    // Components remain accessible when needed, but are not part of the normal setup workflow.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UBoxComponent> SourceBox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UBoxComponent> TargetBox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UNavLinkComponent> GeneratedLinks;

    UPROPERTY(VisibleAnywhere, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UOmniJumpTraversalComponent> JumpTraversal;

    UPROPERTY(VisibleAnywhere, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UOmniMantleTraversalComponent> MantleTraversal;

    /** Runtime smart-link dispatcher used when Traversal Action is Custom. Bind OnTraversalRequested in Blueprint or C++. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Advanced", meta=(AdvancedDisplay))
    TObjectPtr<UOmniCustomTraversalComponent> CustomTraversal;

private:
    struct FProjectedGridSample
    {
        FVector ProjectedWorld = FVector::ZeroVector;
        FVector2D GridUV = FVector2D::ZeroVector;
    };

    struct FProjectedPair
    {
        FVector SourceWorld = FVector::ZeroVector;
        FVector TargetWorld = FVector::ZeroVector;
        ENavLinkDirection::Type NativeDirection = ENavLinkDirection::BothWays;
        bool bUsesJumpTraversal = false;
        bool bUsesMantleTraversal = false;
        bool bUsesCustomTraversal = false;
        bool bAutomaticMantleFallback = false;
        uint8 ValidationFailureCode = 0;
        bool bValidationSpeedFailure = false;
    };

    bool BuildProjectedGrid(const UBoxComponent& Box, bool bSourceVolume, TArray<FProjectedGridSample>& OutSamples);
    bool ProjectCandidate(const FVector& CandidateWorld, const UBoxComponent& OwnerBox, FVector& OutProjectedWorld, bool& bOutOutsideVolume) const;
    bool IsProjectedPointInsideBox(const FVector& WorldPoint, const UBoxComponent& Box) const;
    void BuildNearestPairs(const TArray<FProjectedGridSample>& SourceSamples, const TArray<FProjectedGridSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const;
    void BuildMatchedGridPairs(const TArray<FProjectedGridSample>& SourceSamples, const TArray<FProjectedGridSample>& TargetSamples, TArray<FProjectedPair>& OutPairs) const;
    bool AddProjectedPair(const FProjectedPair& Pair, TArray<FNavigationLink>& InOutLinks, TArray<FOmniJumpLinkSpec>& InOutJumpLinks, TArray<FOmniMantleLinkSpec>& InOutMantleLinks, TArray<FOmniCustomLinkSpec>& InOutCustomLinks);
    bool AreLinkArraysEquivalent(const TArray<FNavigationLink>& A, const TArray<FNavigationLink>& B) const;
    static int32 CalculateAxisSampleCount(double WorldLength, double Spacing);
    int32 FindNearestUsableSampleIndex(const FVector& Point, bool bPointIsSource, const TArray<FProjectedGridSample>& Samples) const;
    int32 FindNearestUnusedSampleByUV(const FProjectedGridSample& DriverSample, bool bSourceDrives, const TArray<FProjectedGridSample>& Samples, const TBitArray<>& UsedSamples) const;
    bool IsPairUsable(const FVector& SourceWorld, const FVector& TargetWorld) const;
    bool IsTraversalAllowedByHeight(const FVector& StartWorld, const FVector& EndWorld) const;
    bool ResolvePairDirection(const FVector& SourceWorld, const FVector& TargetWorld, ENavLinkDirection::Type& OutDirection, bool& bOutHeightRestricted) const;
    bool IsJumpDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode, bool& bOutTooFast) const;
    bool IsMantleDirectionValid(const FVector& StartWorld, const FVector& EndWorld, int32& OutFailureCode) const;
    float GetEffectiveLinkSpacing() const;
    ENavLinkDirection::Type GetNativeDirection() const;
    bool UsesTraversalAction(EOmniTraversalAction Action) const;
    void UpdateGenerationStatus();
    void ApplyEditorVisualStyle();
    void ResetDebugCounters();
    void NotifyNavigationSystem();
    void BindNavigationGenerationDelegate();
    void UnbindNavigationGenerationDelegate();

#if WITH_EDITOR
    void DrawEditorPreview() const;
#endif

    UFUNCTION()
    void HandleNavigationGenerationFinished(ANavigationData* NavData);

    TArray<FVector> PreviewSourceSamples;
    TArray<FVector> PreviewTargetSamples;
    TArray<FProjectedPair> PreviewGeneratedPairs;
    TArray<FProjectedPair> PreviewRejectedJumpPairs;
    TArray<FProjectedPair> PreviewRejectedMantlePairs;

    bool bIsRegenerating = false;
};
