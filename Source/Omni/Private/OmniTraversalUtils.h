#pragma once

#include "CoreMinimal.h"
#include "OmniTypes.h"

class AActor;
class UWorld;

enum class EOmniGroundTraversalResult : uint8
{
    Walkable,
    MissingSupport,
    StepTooHigh,
    SlopeTooSteep,
    Blocked,
    Invalid
};

struct FOmniAutomaticTraversalSettings
{
    float RiseThreshold = 10.0f;
    float NativeDropMaxDistance = 200.0f;
    float GroundProbeDepth = 75.0f;
    float GroundProbeSpacing = 75.0f;
    bool bValidateNativePath = true;
    float MaxStepHeight = 45.0f;
    float MaxGroundSlopeDegrees = 44.0f;
    float AgentRadius = 34.0f;
    float AgentHalfHeight = 88.0f;
};

struct FOmniJumpValidationSettings
{
    bool bValidatePath = true;
    float ApexHeight = 150.0f;
    float AgentRadius = 34.0f;
    float AgentHalfHeight = 88.0f;
    float MaxHorizontalSpeed = 1600.0f;
    int32 ArcSegments = 16;
};

enum class EOmniJumpValidationResult : uint8
{
    Valid,
    Blocked,
    TooFast,
    Invalid
};


/** One directional piece of the traversal plan for a Source/Target pair. */
struct FOmniTraversalLeg
{
    ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
    bool bUseJump = false;
    bool bUseCustom = false;
    EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
};

/**
 * Shared traversal classification result used by Point, Volume, and Spline.
 * A bidirectional pair normally produces one BothWays leg. Automatic can produce
 * two one-way legs when the two directions require different traversal actions.
 */
struct FOmniAutomaticDirectionDiagnostic
{
    bool bEvaluated = false;
    bool bUseJump = false;
    EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
};

struct FOmniTraversalPlan
{
    FOmniTraversalLeg Legs[2];
    int32 LegCount = 0;
    bool bAutomaticSplit = false;

    /** Per-direction Automatic results retained even when both directions collapse into one BothWays leg. */
    FOmniAutomaticDirectionDiagnostic SourceToTargetAutomatic;
    FOmniAutomaticDirectionDiagnostic TargetToSourceAutomatic;
};

namespace OmniTraversal
{
    /**
     * Resolves Native / Jump / Automatic / Custom into one or two directional traversal legs.
     * This is the canonical classifier shared by every Omni authoring shape.
     */
    FOmniTraversalPlan BuildTraversalPlan(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type PairDirection,
        EOmniTraversalAction TraversalAction,
        const FOmniAutomaticTraversalSettings& AutomaticSettings);

    /** Returns true when Automatic traversal should use Omni's Jump smart-link execution. */
    bool ShouldUseJump(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings,
        EOmniGroundTraversalResult* OutGroundResult = nullptr);

    /** Tests whether ordinary collision support exists beneath the straight crossing. */
    bool HasContinuousGroundSupport(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        float ProbeDepth,
        float ProbeSpacing);

    /**
     * Evaluates whether the straight Source-to-Target crossing looks physically walkable.
     * In addition to continuous support, optional validation checks step height, ground
     * slope, and upright capsule clearance at each probe sample.
     */
    EOmniGroundTraversalResult EvaluateGroundTraversal(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings);

    /**
     * Calculates a ballistic launch velocity that reaches End from Start while
     * passing above the higher endpoint by ApexHeight.
     */
    bool CalculateBallisticLaunch(
        const FVector& Start,
        const FVector& End,
        float ApexHeight,
        float GravityMagnitude,
        FVector& OutLaunchVelocity,
        float& OutTotalTime);

    /**
     * Validates a generated jump direction using a standard world-Z ballistic arc.
     * Endpoints are treated as feet locations; AgentHalfHeight lifts the test arc
     * to approximate an upright Character capsule center.
     */
    EOmniJumpValidationResult ValidateJumpPath(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartFeetWorld,
        const FVector& EndFeetWorld,
        const FOmniJumpValidationSettings& Settings);
}
