#include "OmniTraversalUtils.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace OmniTraversal
{
    FOmniTraversalPlan BuildTraversalPlan(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type PairDirection,
        EOmniTraversalAction TraversalAction,
        const FOmniAutomaticTraversalSettings& AutomaticSettings)
    {
        FOmniTraversalPlan Plan;

        auto AddLeg = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseCustom, EOmniGroundTraversalResult GroundResult)
        {
            if (Plan.LegCount >= UE_ARRAY_COUNT(Plan.Legs))
            {
                return;
            }
            FOmniTraversalLeg& Leg = Plan.Legs[Plan.LegCount++];
            Leg.Direction = Direction;
            Leg.bUseJump = bUseJump;
            Leg.bUseCustom = bUseCustom;
            Leg.GroundResult = GroundResult;
        };

        if (TraversalAction == EOmniTraversalAction::Jump)
        {
            AddLeg(PairDirection, true, false, EOmniGroundTraversalResult::Invalid);
            return Plan;
        }

        if (TraversalAction == EOmniTraversalAction::Custom)
        {
            AddLeg(PairDirection, false, true, EOmniGroundTraversalResult::Invalid);
            return Plan;
        }

        if (TraversalAction == EOmniTraversalAction::NativeMovement)
        {
            AddLeg(PairDirection, false, false, EOmniGroundTraversalResult::Invalid);
            return Plan;
        }

        auto Classify = [&](const FVector& Start, const FVector& End, EOmniGroundTraversalResult& OutGroundResult)
        {
            return ShouldUseJump(World, IgnoreActor, Start, End, AutomaticSettings, &OutGroundResult);
        };

        if (PairDirection == ENavLinkDirection::BothWays)
        {
            EOmniGroundTraversalResult ForwardGround = EOmniGroundTraversalResult::Invalid;
            EOmniGroundTraversalResult ReverseGround = EOmniGroundTraversalResult::Invalid;
            const bool bForwardJump = Classify(SourceWorld, TargetWorld, ForwardGround);
            const bool bReverseJump = Classify(TargetWorld, SourceWorld, ReverseGround);
            Plan.SourceToTargetAutomatic = { true, bForwardJump, ForwardGround };
            Plan.TargetToSourceAutomatic = { true, bReverseJump, ReverseGround };

            if (bForwardJump == bReverseJump)
            {
                // Keep the forward result for diagnostics. If both directions are
                // equivalent, callers do not need two separately emitted links.
                AddLeg(ENavLinkDirection::BothWays, bForwardJump, false, ForwardGround);
                return Plan;
            }

            AddLeg(ENavLinkDirection::LeftToRight, bForwardJump, false, ForwardGround);
            AddLeg(ENavLinkDirection::RightToLeft, bReverseJump, false, ReverseGround);
            Plan.bAutomaticSplit = true;
            return Plan;
        }

        EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
        const bool bUseJump = PairDirection == ENavLinkDirection::RightToLeft
            ? Classify(TargetWorld, SourceWorld, GroundResult)
            : Classify(SourceWorld, TargetWorld, GroundResult);
        if (PairDirection == ENavLinkDirection::RightToLeft)
        {
            Plan.TargetToSourceAutomatic = { true, bUseJump, GroundResult };
        }
        else
        {
            Plan.SourceToTargetAutomatic = { true, bUseJump, GroundResult };
        }
        AddLeg(PairDirection, bUseJump, false, GroundResult);
        return Plan;
    }

    EOmniGroundTraversalResult EvaluateGroundTraversal(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings)
    {
        if (!World)
        {
            return EOmniGroundTraversalResult::Invalid;
        }

        const float SafeProbeDepth = FMath::Max(1.0f, Settings.GroundProbeDepth);
        const float SafeProbeSpacing = FMath::Max(10.0f, Settings.GroundProbeSpacing);
        const float SafeMaxStepHeight = FMath::Max(0.0f, Settings.MaxStepHeight);
        const float SafeMaxSlopeDegrees = FMath::Clamp(Settings.MaxGroundSlopeDegrees, 0.0f, 89.0f);
        const float SafeRadius = FMath::Max(1.0f, Settings.AgentRadius);
        const float SafeHalfHeight = FMath::Max(SafeRadius, Settings.AgentHalfHeight);
        const double HorizontalDistance = FVector::Dist2D(StartWorld, EndWorld);
        const int32 SegmentCount = FMath::Clamp(FMath::CeilToInt(HorizontalDistance / static_cast<double>(SafeProbeSpacing)), 2, 64);
        const float TraceLift = FMath::Max(25.0f, SafeMaxStepHeight + 10.0f);

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(OmniAutomaticTraversalGroundProbe), false);
        if (IgnoreActor)
        {
            QueryParams.AddIgnoredActor(IgnoreActor);
        }

        const FCollisionShape AgentShape = FCollisionShape::MakeCapsule(SafeRadius, SafeHalfHeight);
        FVector PreviousGround = FVector::ZeroVector;
        float PreviousSurfaceSlopeDegrees = 0.0f;
        bool bHasPreviousGround = false;

        for (int32 Segment = 0; Segment <= SegmentCount; ++Segment)
        {
            const float Alpha = static_cast<float>(Segment) / static_cast<float>(SegmentCount);
            const FVector ExpectedPoint = FMath::Lerp(StartWorld, EndWorld, Alpha);
            const FVector TraceStart = ExpectedPoint + FVector(0.0, 0.0, TraceLift);
            const FVector TraceEnd = ExpectedPoint - FVector(0.0, 0.0, SafeProbeDepth);

            FHitResult GroundHit;
            if (!World->LineTraceSingleByChannel(GroundHit, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams))
            {
                return EOmniGroundTraversalResult::MissingSupport;
            }

            const FVector GroundPoint = GroundHit.ImpactPoint;

            float SurfaceSlopeDegrees = 0.0f;
            if (Settings.bValidateNativePath)
            {
                const float UpDot = FMath::Clamp(GroundHit.ImpactNormal.GetSafeNormal().Z, -1.0f, 1.0f);
                SurfaceSlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(UpDot));
                if (SurfaceSlopeDegrees > SafeMaxSlopeDegrees + KINDA_SMALL_NUMBER)
                {
                    return EOmniGroundTraversalResult::SlopeTooSteep;
                }

                if (bHasPreviousGround)
                {
                    // Treat a large height discontinuity between two essentially flat
                    // support samples as a step. Sloped surfaces are governed by the
                    // surface-normal slope test above instead, so a valid ramp is not
                    // mistaken for an oversized stair simply because probes are spaced apart.
                    constexpr float FlatSurfaceToleranceDegrees = 5.0f;
                    const bool bPreviousFlat = PreviousSurfaceSlopeDegrees <= FlatSurfaceToleranceDegrees;
                    const bool bCurrentFlat = SurfaceSlopeDegrees <= FlatSurfaceToleranceDegrees;
                    const double StepDelta = FMath::Abs(static_cast<double>(GroundPoint.Z - PreviousGround.Z));
                    if (bPreviousFlat
                        && bCurrentFlat
                        && StepDelta > static_cast<double>(SafeMaxStepHeight) + KINDA_SMALL_NUMBER)
                    {
                        return EOmniGroundTraversalResult::StepTooHigh;
                    }
                }

                // A vertical capsule needs a little more world-Z lift on a slope than
                // it does on flat ground so its lower hemisphere is tangent to the
                // support plane instead of falsely overlapping it.
                const float SafeUpDotForClearance = FMath::Max(0.1f, UpDot);
                const float SlopeLift = SafeRadius * ((1.0f / SafeUpDotForClearance) - 1.0f);
                const FVector CapsuleCenter = GroundPoint + FVector(0.0, 0.0, SafeHalfHeight + SlopeLift + 2.0f);
                if (World->OverlapBlockingTestByChannel(
                        CapsuleCenter,
                        FQuat::Identity,
                        ECC_WorldStatic,
                        AgentShape,
                        QueryParams))
                {
                    return EOmniGroundTraversalResult::Blocked;
                }
            }

            PreviousGround = GroundPoint;
            PreviousSurfaceSlopeDegrees = SurfaceSlopeDegrees;
            bHasPreviousGround = true;
        }

        return EOmniGroundTraversalResult::Walkable;
    }

    bool HasContinuousGroundSupport(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        float ProbeDepth,
        float ProbeSpacing)
    {
        FOmniAutomaticTraversalSettings Settings;
        Settings.GroundProbeDepth = ProbeDepth;
        Settings.GroundProbeSpacing = ProbeSpacing;
        Settings.bValidateNativePath = false;
        return EvaluateGroundTraversal(World, IgnoreActor, StartWorld, EndWorld, Settings) == EOmniGroundTraversalResult::Walkable;
    }

    bool ShouldUseJump(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings,
        EOmniGroundTraversalResult* OutGroundResult)
    {
        const float RiseThreshold = FMath::Max(0.0f, Settings.RiseThreshold);
        const float DeltaZ = EndWorld.Z - StartWorld.Z;
        const double HorizontalDistance = FVector::Dist2D(StartWorld, EndWorld);

        if (HorizontalDistance <= 1.0)
        {
            const bool bUseJump = DeltaZ > RiseThreshold;
            if (OutGroundResult)
            {
                *OutGroundResult = bUseJump ? EOmniGroundTraversalResult::StepTooHigh : EOmniGroundTraversalResult::Walkable;
            }
            return bUseJump;
        }

        const EOmniGroundTraversalResult GroundResult = EvaluateGroundTraversal(
            World,
            IgnoreActor,
            StartWorld,
            EndWorld,
            Settings);

        if (OutGroundResult)
        {
            *OutGroundResult = GroundResult;
        }

        if (GroundResult == EOmniGroundTraversalResult::Walkable)
        {
            return false;
        }

        // Preserve the deliberate native short-drop behavior only when the native
        // path failed because support disappeared. A steep surface, oversized step,
        // or blocked capsule clearance should not be mislabeled as a harmless drop.
        if (GroundResult == EOmniGroundTraversalResult::MissingSupport
            && DeltaZ < -RiseThreshold
            && HorizontalDistance <= static_cast<double>(FMath::Max(0.0f, Settings.NativeDropMaxDistance)))
        {
            return false;
        }

        return true;
    }

    bool CalculateBallisticLaunch(
        const FVector& Start,
        const FVector& End,
        float ApexHeight,
        float GravityMagnitude,
        FVector& OutLaunchVelocity,
        float& OutTotalTime)
    {
        const double SafeGravity = FMath::Max(1.0, static_cast<double>(FMath::Abs(GravityMagnitude)));
        const double SafeApexHeight = FMath::Max(1.0, static_cast<double>(ApexHeight));
        const double ApexZ = FMath::Max(static_cast<double>(Start.Z), static_cast<double>(End.Z)) + SafeApexHeight;
        const double RiseToApex = FMath::Max(1.0, ApexZ - static_cast<double>(Start.Z));
        const double FallFromApex = FMath::Max(1.0, ApexZ - static_cast<double>(End.Z));

        const double VerticalSpeed = FMath::Sqrt(2.0 * SafeGravity * RiseToApex);
        const double TimeUp = VerticalSpeed / SafeGravity;
        const double TimeDown = FMath::Sqrt((2.0 * FallFromApex) / SafeGravity);
        const double TotalTime = FMath::Max(0.05, TimeUp + TimeDown);

        FVector HorizontalDelta = End - Start;
        HorizontalDelta.Z = 0.0;
        const FVector HorizontalVelocity = HorizontalDelta / TotalTime;

        OutLaunchVelocity = FVector(HorizontalVelocity.X, HorizontalVelocity.Y, VerticalSpeed);
        OutTotalTime = static_cast<float>(TotalTime);
        return !OutLaunchVelocity.ContainsNaN() && FMath::IsFinite(OutTotalTime);
    }

    EOmniJumpValidationResult ValidateJumpPath(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartFeetWorld,
        const FVector& EndFeetWorld,
        const FOmniJumpValidationSettings& Settings)
    {
        if (!Settings.bValidatePath)
        {
            return EOmniJumpValidationResult::Valid;
        }

        if (!World)
        {
            return EOmniJumpValidationResult::Invalid;
        }

        const float SafeRadius = FMath::Max(1.0f, Settings.AgentRadius);
        const float SafeHalfHeight = FMath::Max(SafeRadius, Settings.AgentHalfHeight);
        const int32 SafeSegments = FMath::Clamp(Settings.ArcSegments, 4, 64);
        const FVector CenterOffset(0.0, 0.0, SafeHalfHeight + 2.0f);
        const FVector StartCenter = StartFeetWorld + CenterOffset;
        const FVector EndCenter = EndFeetWorld + CenterOffset;

        FVector LaunchVelocity = FVector::ZeroVector;
        float TotalTime = 0.0f;
        constexpr float StandardGravityMagnitude = 980.0f;
        if (!CalculateBallisticLaunch(StartCenter, EndCenter, Settings.ApexHeight, StandardGravityMagnitude, LaunchVelocity, TotalTime))
        {
            return EOmniJumpValidationResult::Invalid;
        }

        if (Settings.MaxHorizontalSpeed > 0.0f)
        {
            const double HorizontalSpeed = FVector2D(LaunchVelocity.X, LaunchVelocity.Y).Size();
            if (HorizontalSpeed > static_cast<double>(Settings.MaxHorizontalSpeed))
            {
                return EOmniJumpValidationResult::TooFast;
            }
        }

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(OmniJumpArcValidation), false);
        if (IgnoreActor)
        {
            QueryParams.AddIgnoredActor(IgnoreActor);
        }

        const FCollisionShape AgentShape = FCollisionShape::MakeCapsule(SafeRadius, SafeHalfHeight);
        FVector PreviousPoint = StartCenter;

        for (int32 Segment = 1; Segment <= SafeSegments; ++Segment)
        {
            const float Alpha = static_cast<float>(Segment) / static_cast<float>(SafeSegments);
            const float Time = TotalTime * Alpha;
            FVector CurrentPoint = StartCenter + LaunchVelocity * Time;
            CurrentPoint.Z -= 0.5f * StandardGravityMagnitude * Time * Time;

            FHitResult Hit;
            if (World->SweepSingleByChannel(
                    Hit,
                    PreviousPoint,
                    CurrentPoint,
                    FQuat::Identity,
                    ECC_WorldStatic,
                    AgentShape,
                    QueryParams))
            {
                return EOmniJumpValidationResult::Blocked;
            }

            PreviousPoint = CurrentPoint;
        }

        return EOmniJumpValidationResult::Valid;
    }
}
