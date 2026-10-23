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
    bool bAllowAutomaticMantle = true;
    bool bRequireMantleLedge = true;
    float MantleMaxHeight = 150.0f;
    float MantleMaxReach = 120.0f;
    bool bValidateMantlePath = true;
    float MantleArcHeight = 35.0f;
    int32 MantleValidationSegments = 12;
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
    DepartureSupport,
    DepartureClearance,
    PathBlocked,
    LandingSupport,
    LandingClearance,
    TooFast,
    Invalid
};

struct FOmniMantleValidationSettings
{
    bool bValidatePath = true;
    float ArcHeight = 35.0f;
    float AgentRadius = 34.0f;
    float AgentHalfHeight = 88.0f;
    int32 PathSegments = 12;
};

enum class EOmniMantleValidationResult : uint8
{
    Valid,
    DepartureSupport,
    DepartureClearance,
    PathBlocked,
    LandingSupport,
    LandingClearance,
    Invalid
};


/** One directional piece of the traversal plan for a Source/Target pair. */
struct FOmniTraversalLeg
{
    ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
    bool bUseJump = false;
    bool bUseMantle = false;
    bool bUseCustom = false;
    EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
};

/**
 * Shared traversal classification result used by Point, Volume, and Spline.
 * A bidirectional pair normally produces one BothWays leg. Automatic can produce
 * two one-way legs when the two directions require different traversal actions, including Mantle.
 */
struct FOmniAutomaticDirectionDiagnostic
{
    bool bEvaluated = false;
    bool bUseJump = false;
    bool bUseMantle = false;
    bool bMantleValidationFallback = false;
    bool bMantleLedgeFallback = false;
    EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
};

struct FOmniTraversalPlan
{
    FOmniTraversalLeg Legs[2];
    int32 LegCount = 0;
    bool bAutomaticSplit = false;
    bool bDirectionalSplit = false;

    /** Per-direction Automatic results retained even when both directions collapse into one BothWays leg. */
    FOmniAutomaticDirectionDiagnostic SourceToTargetAutomatic;
    FOmniAutomaticDirectionDiagnostic TargetToSourceAutomatic;
};

namespace OmniTraversal
{
    /**
     * Resolves Native / Jump / Mantle / Automatic / Custom into one or two directional traversal legs.
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

    /**
     * Resolves traversal with optional per-direction action overrides.
     * When overrides are enabled, Source to Target and Target to Source may independently
     * use Native Movement, Jump, Mantle, Automatic, or Custom. Compatible results collapse back
     * into one bidirectional link; incompatible results become two one-way legs.
     */
    FOmniTraversalPlan BuildDirectionalTraversalPlan(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type PairDirection,
        EOmniTraversalAction DefaultTraversalAction,
        bool bUseDirectionalActions,
        EOmniTraversalAction SourceToTargetAction,
        EOmniTraversalAction TargetToSourceAction,
        const FOmniAutomaticTraversalSettings& AutomaticSettings);

    /** Resolves Automatic traversal into Native, Mantle, or Jump for one direction. */
    void ResolveAutomaticTraversal(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings,
        bool& bOutUseJump,
        bool& bOutUseMantle,
        EOmniGroundTraversalResult* OutGroundResult = nullptr,
        bool* bOutMantleValidationFallback = nullptr,
        bool* bOutMantleLedgeFallback = nullptr);

    /**
     * Returns true when an upward Automatic Mantle candidate has usable ledge
     * geometry: a WorldStatic face opposing travel plus a walkable, capsule-clear
     * top immediately beyond that face near the destination height.
     * This is intentionally an Automatic-only classification check.
     */
    bool HasMantleLedgeGeometry(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings);

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

    /** Returns the controlled three-stage path used by built-in Mantle traversal and its editor preview. */
    FVector CalculateMantlePathPoint(
        const FVector& Start,
        const FVector& End,
        float Alpha,
        float ArcHeight);

    /**
     * Validates a built-in Mantle direction by sweeping an upright capsule along
     * the same three-stage path used by preview and runtime movement. Endpoints
     * are treated as feet locations and lifted to capsule-center height.
     */
    EOmniMantleValidationResult ValidateMantlePath(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartFeetWorld,
        const FVector& EndFeetWorld,
        const FOmniMantleValidationSettings& Settings,
        FVector* OutBlockingLocation = nullptr,
        FString* OutBlockingObject = nullptr);

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
        const FOmniJumpValidationSettings& Settings,
        FVector* OutBlockingLocation = nullptr,
        FString* OutBlockingObject = nullptr);
}
