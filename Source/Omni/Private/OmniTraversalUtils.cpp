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
        return BuildDirectionalTraversalPlan(
            World,
            IgnoreActor,
            SourceWorld,
            TargetWorld,
            PairDirection,
            TraversalAction,
            false,
            TraversalAction,
            TraversalAction,
            AutomaticSettings);
    }

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
        const FOmniAutomaticTraversalSettings& AutomaticSettings)
    {
        FOmniTraversalPlan Plan;

        auto AddLeg = [&](ENavLinkDirection::Type Direction, bool bUseJump, bool bUseMantle, bool bUseCustom, EOmniGroundTraversalResult GroundResult)
        {
            if (Plan.LegCount >= UE_ARRAY_COUNT(Plan.Legs))
            {
                return;
            }

            FOmniTraversalLeg& Leg = Plan.Legs[Plan.LegCount++];
            Leg.Direction = Direction;
            Leg.bUseJump = bUseJump;
            Leg.bUseMantle = bUseMantle;
            Leg.bUseCustom = bUseCustom;
            Leg.GroundResult = GroundResult;
        };

        struct FResolvedAction
        {
            bool bUseJump = false;
            bool bUseMantle = false;
            bool bUseCustom = false;
            bool bMantleValidationFallback = false;
            bool bMantleLedgeFallback = false;
            EOmniGroundTraversalResult GroundResult = EOmniGroundTraversalResult::Invalid;
        };

        auto ResolveAction = [&](EOmniTraversalAction Action, const FVector& Start, const FVector& End, FOmniAutomaticDirectionDiagnostic* AutomaticDiagnostic)
        {
            FResolvedAction Resolved;

            switch (Action)
            {
            case EOmniTraversalAction::Jump:
                Resolved.bUseJump = true;
                break;

            case EOmniTraversalAction::Mantle:
                Resolved.bUseMantle = true;
                break;

            case EOmniTraversalAction::Custom:
                Resolved.bUseCustom = true;
                break;

            case EOmniTraversalAction::Automatic:
                ResolveAutomaticTraversal(
                    World,
                    IgnoreActor,
                    Start,
                    End,
                    AutomaticSettings,
                    Resolved.bUseJump,
                    Resolved.bUseMantle,
                    &Resolved.GroundResult,
                    &Resolved.bMantleValidationFallback,
                    &Resolved.bMantleLedgeFallback);

                if (AutomaticDiagnostic)
                {
                    AutomaticDiagnostic->bEvaluated = true;
                    AutomaticDiagnostic->bUseJump = Resolved.bUseJump;
                    AutomaticDiagnostic->bUseMantle = Resolved.bUseMantle;
                    AutomaticDiagnostic->bMantleValidationFallback = Resolved.bMantleValidationFallback;
                    AutomaticDiagnostic->bMantleLedgeFallback = Resolved.bMantleLedgeFallback;
                    AutomaticDiagnostic->GroundResult = Resolved.GroundResult;
                }
                break;

            case EOmniTraversalAction::NativeMovement:
            default:
                break;
            }

            return Resolved;
        };

        auto ActionForDirection = [&](ENavLinkDirection::Type Direction)
        {
            if (!bUseDirectionalActions)
            {
                return DefaultTraversalAction;
            }

            return Direction == ENavLinkDirection::RightToLeft
                ? TargetToSourceAction
                : SourceToTargetAction;
        };

        if (PairDirection != ENavLinkDirection::BothWays)
        {
            const bool bReverse = PairDirection == ENavLinkDirection::RightToLeft;
            const EOmniTraversalAction Action = ActionForDirection(PairDirection);
            FOmniAutomaticDirectionDiagnostic* Diagnostic = nullptr;

            if (Action == EOmniTraversalAction::Automatic)
            {
                Diagnostic = bReverse
                    ? &Plan.TargetToSourceAutomatic
                    : &Plan.SourceToTargetAutomatic;
            }

            const FResolvedAction Resolved = bReverse
                ? ResolveAction(Action, TargetWorld, SourceWorld, Diagnostic)
                : ResolveAction(Action, SourceWorld, TargetWorld, Diagnostic);

            AddLeg(PairDirection, Resolved.bUseJump, Resolved.bUseMantle, Resolved.bUseCustom, Resolved.GroundResult);
            return Plan;
        }

        const EOmniTraversalAction ForwardAction = bUseDirectionalActions
            ? SourceToTargetAction
            : DefaultTraversalAction;
        const EOmniTraversalAction ReverseAction = bUseDirectionalActions
            ? TargetToSourceAction
            : DefaultTraversalAction;

        FOmniAutomaticDirectionDiagnostic* ForwardDiagnostic = ForwardAction == EOmniTraversalAction::Automatic
            ? &Plan.SourceToTargetAutomatic
            : nullptr;
        FOmniAutomaticDirectionDiagnostic* ReverseDiagnostic = ReverseAction == EOmniTraversalAction::Automatic
            ? &Plan.TargetToSourceAutomatic
            : nullptr;

        const FResolvedAction Forward = ResolveAction(
            ForwardAction,
            SourceWorld,
            TargetWorld,
            ForwardDiagnostic);
        const FResolvedAction Reverse = ResolveAction(
            ReverseAction,
            TargetWorld,
            SourceWorld,
            ReverseDiagnostic);

        const bool bSameTraversalType =
            Forward.bUseJump == Reverse.bUseJump
            && Forward.bUseMantle == Reverse.bUseMantle
            && Forward.bUseCustom == Reverse.bUseCustom;

        if (bSameTraversalType)
        {
            AddLeg(
                ENavLinkDirection::BothWays,
                Forward.bUseJump,
                Forward.bUseMantle,
                Forward.bUseCustom,
                Forward.GroundResult);
            return Plan;
        }

        AddLeg(
            ENavLinkDirection::LeftToRight,
            Forward.bUseJump,
            Forward.bUseMantle,
            Forward.bUseCustom,
            Forward.GroundResult);
        AddLeg(
            ENavLinkDirection::RightToLeft,
            Reverse.bUseJump,
            Reverse.bUseMantle,
            Reverse.bUseCustom,
            Reverse.GroundResult);

        Plan.bAutomaticSplit =
            ForwardAction == EOmniTraversalAction::Automatic
            || ReverseAction == EOmniTraversalAction::Automatic;
        Plan.bDirectionalSplit =
            bUseDirectionalActions
            && ForwardAction != ReverseAction;

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

    bool HasMantleLedgeGeometry(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings)
    {
        if (!World || StartWorld.ContainsNaN() || EndWorld.ContainsNaN())
        {
            return false;
        }

        const double Rise = EndWorld.Z - StartWorld.Z;
        if (Rise <= KINDA_SMALL_NUMBER)
        {
            return false;
        }

        FVector HorizontalDelta = EndWorld - StartWorld;
        HorizontalDelta.Z = 0.0;
        const double HorizontalDistance = HorizontalDelta.Size();
        if (HorizontalDistance <= 1.0)
        {
            return false;
        }

        const FVector TravelDirection = HorizontalDelta / HorizontalDistance;
        const float SafeRadius = FMath::Max(1.0f, Settings.AgentRadius);
        const float SafeHalfHeight = FMath::Max(SafeRadius, Settings.AgentHalfHeight);
        const float SafeMaxSlopeDegrees = FMath::Clamp(Settings.MaxGroundSlopeDegrees, 0.0f, 89.0f);
        const float TopHeightTolerance = FMath::Clamp(Settings.MaxStepHeight, 15.0f, 50.0f);
        const float TopProbeLift = FMath::Max(25.0f, SafeRadius);
        const float TopInset = SafeRadius + 5.0f;

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(OmniAutomaticMantleLedgeProbe), false);
        if (IgnoreActor)
        {
            QueryParams.AddIgnoredActor(IgnoreActor);
        }

        const FCollisionShape AgentShape = FCollisionShape::MakeCapsule(SafeRadius, SafeHalfHeight);
        constexpr float ProbeFractions[] = { 0.25f, 0.5f, 0.75f };

        for (const float Fraction : ProbeFractions)
        {
            const double ProbeZ = FMath::Lerp(StartWorld.Z, EndWorld.Z, static_cast<double>(Fraction));
            const FVector TraceStart(StartWorld.X, StartWorld.Y, ProbeZ);
            const FVector TraceEnd(EndWorld.X, EndWorld.Y, ProbeZ);

            FHitResult FaceHit;
            if (!World->LineTraceSingleByChannel(
                    FaceHit,
                    TraceStart,
                    TraceEnd,
                    ECC_WorldStatic,
                    QueryParams))
            {
                continue;
            }

            FVector HorizontalNormal(FaceHit.ImpactNormal.X, FaceHit.ImpactNormal.Y, 0.0);
            if (!HorizontalNormal.Normalize())
            {
                continue;
            }

            // A useful mantle face must substantially oppose the approach. This
            // rejects horizontal tops/undersides and glancing incidental geometry.
            if (FVector::DotProduct(HorizontalNormal, -TravelDirection) < 0.25f)
            {
                continue;
            }

            // A face alone is not enough. Probe just beyond it toward the elevated
            // destination and require a usable top near the destination height.
            // The radius-sized inset asks whether a full agent can actually get
            // onto the ledge rather than merely touching a thin wall or lip.
            const FVector TopProbeXY = FaceHit.ImpactPoint + (TravelDirection * TopInset);
            const FVector TopTraceStart(TopProbeXY.X, TopProbeXY.Y, EndWorld.Z + TopProbeLift);
            const FVector TopTraceEnd(TopProbeXY.X, TopProbeXY.Y, EndWorld.Z - TopHeightTolerance);

            FHitResult TopHit;
            if (!World->LineTraceSingleByChannel(
                    TopHit,
                    TopTraceStart,
                    TopTraceEnd,
                    ECC_WorldStatic,
                    QueryParams))
            {
                continue;
            }

            if (FMath::Abs(TopHit.ImpactPoint.Z - EndWorld.Z) > TopHeightTolerance + KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float UpDot = FMath::Clamp(TopHit.ImpactNormal.GetSafeNormal().Z, -1.0f, 1.0f);
            const float SurfaceSlopeDegrees = FMath::RadiansToDegrees(FMath::Acos(UpDot));
            if (SurfaceSlopeDegrees > SafeMaxSlopeDegrees + KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float SafeUpDotForClearance = FMath::Max(0.1f, UpDot);
            const float SlopeLift = SafeRadius * ((1.0f / SafeUpDotForClearance) - 1.0f);
            const FVector CapsuleCenter = TopHit.ImpactPoint + FVector(0.0, 0.0, SafeHalfHeight + SlopeLift + 2.0f);
            if (World->OverlapBlockingTestByChannel(
                    CapsuleCenter,
                    FQuat::Identity,
                    ECC_WorldStatic,
                    AgentShape,
                    QueryParams))
            {
                continue;
            }

            return true;
        }

        return false;
    }

    void ResolveAutomaticTraversal(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings,
        bool& bOutUseJump,
        bool& bOutUseMantle,
        EOmniGroundTraversalResult* OutGroundResult,
        bool* bOutMantleValidationFallback,
        bool* bOutMantleLedgeFallback)
    {
        bOutUseJump = false;
        bOutUseMantle = false;
        if (bOutMantleValidationFallback)
        {
            *bOutMantleValidationFallback = false;
        }
        if (bOutMantleLedgeFallback)
        {
            *bOutMantleLedgeFallback = false;
        }

        const float RiseThreshold = FMath::Max(0.0f, Settings.RiseThreshold);
        const float DeltaZ = EndWorld.Z - StartWorld.Z;
        const double HorizontalDistance = FVector::Dist2D(StartWorld, EndWorld);

        auto TryAutomaticMantle = [&]()
        {
            if (Settings.bRequireMantleLedge && !HasMantleLedgeGeometry(World, IgnoreActor, StartWorld, EndWorld, Settings))
            {
                bOutUseJump = true;
                if (bOutMantleLedgeFallback)
                {
                    *bOutMantleLedgeFallback = true;
                }
                return;
            }

            FOmniMantleValidationSettings MantleSettings;
            MantleSettings.bValidatePath = Settings.bValidateMantlePath;
            MantleSettings.ArcHeight = Settings.MantleArcHeight;
            MantleSettings.AgentRadius = Settings.AgentRadius;
            MantleSettings.AgentHalfHeight = Settings.AgentHalfHeight;
            MantleSettings.PathSegments = Settings.MantleValidationSegments;

            if (ValidateMantlePath(World, IgnoreActor, StartWorld, EndWorld, MantleSettings) == EOmniMantleValidationResult::Valid)
            {
                bOutUseMantle = true;
                return;
            }

            bOutUseJump = true;
            if (bOutMantleValidationFallback)
            {
                *bOutMantleValidationFallback = true;
            }
        };

        if (HorizontalDistance <= 1.0)
        {
            const bool bNeedsSpecialTraversal = DeltaZ > RiseThreshold;
            const bool bMantleGeometry =
                bNeedsSpecialTraversal
                && Settings.bAllowAutomaticMantle
                && DeltaZ <= FMath::Max(0.0f, Settings.MantleMaxHeight) + KINDA_SMALL_NUMBER;

            if (bMantleGeometry)
            {
                TryAutomaticMantle();
            }
            else
            {
                bOutUseJump = bNeedsSpecialTraversal;
            }

            if (OutGroundResult)
            {
                *OutGroundResult = bNeedsSpecialTraversal
                    ? EOmniGroundTraversalResult::StepTooHigh
                    : EOmniGroundTraversalResult::Walkable;
            }
            return;
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
            return;
        }

        if (GroundResult == EOmniGroundTraversalResult::MissingSupport
            && DeltaZ < -RiseThreshold
            && HorizontalDistance <= static_cast<double>(FMath::Max(0.0f, Settings.NativeDropMaxDistance)))
        {
            return;
        }

        const bool bMantleGeometry =
            Settings.bAllowAutomaticMantle
            && DeltaZ > RiseThreshold
            && DeltaZ <= FMath::Max(0.0f, Settings.MantleMaxHeight) + KINDA_SMALL_NUMBER
            && HorizontalDistance <= static_cast<double>(FMath::Max(0.0f, Settings.MantleMaxReach)) + KINDA_SMALL_NUMBER
            && (GroundResult == EOmniGroundTraversalResult::MissingSupport
                || GroundResult == EOmniGroundTraversalResult::StepTooHigh);

        if (bMantleGeometry)
        {
            TryAutomaticMantle();
            return;
        }

        bOutUseJump = true;
    }

    bool ShouldUseJump(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartWorld,
        const FVector& EndWorld,
        const FOmniAutomaticTraversalSettings& Settings,
        EOmniGroundTraversalResult* OutGroundResult)
    {
        bool bUseJump = false;
        bool bUseMantle = false;
        ResolveAutomaticTraversal(
            World,
            IgnoreActor,
            StartWorld,
            EndWorld,
            Settings,
            bUseJump,
            bUseMantle,
            OutGroundResult);
        return bUseJump;
    }

    FVector CalculateMantlePathPoint(
        const FVector& Start,
        const FVector& End,
        float Alpha,
        float ArcHeight)
    {
        const float A = FMath::Clamp(Alpha, 0.0f, 1.0f);
        const float SafeArcHeight = FMath::Max(0.0f, ArcHeight);
        const float TopZ = FMath::Max(Start.Z, End.Z) + SafeArcHeight;

        auto Smooth = [](float T)
        {
            const float C = FMath::Clamp(T, 0.0f, 1.0f);
            return C * C * (3.0f - 2.0f * C);
        };

        const FVector LiftEnd(
            FMath::Lerp(Start.X, End.X, 0.2f),
            FMath::Lerp(Start.Y, End.Y, 0.2f),
            TopZ);
        const FVector TraverseEnd(End.X, End.Y, TopZ);

        if (A < 0.35f)
        {
            return FMath::Lerp(Start, LiftEnd, Smooth(A / 0.35f));
        }

        if (A < 0.8f)
        {
            return FMath::Lerp(LiftEnd, TraverseEnd, Smooth((A - 0.35f) / 0.45f));
        }

        return FMath::Lerp(TraverseEnd, End, Smooth((A - 0.8f) / 0.2f));
    }

    EOmniMantleValidationResult ValidateMantlePath(
        UWorld* World,
        const AActor* IgnoreActor,
        const FVector& StartFeetWorld,
        const FVector& EndFeetWorld,
        const FOmniMantleValidationSettings& Settings)
    {
        if (!Settings.bValidatePath)
        {
            return EOmniMantleValidationResult::Valid;
        }

        if (!World || StartFeetWorld.ContainsNaN() || EndFeetWorld.ContainsNaN())
        {
            return EOmniMantleValidationResult::Invalid;
        }

        const float SafeRadius = FMath::Max(1.0f, Settings.AgentRadius);
        const float SafeHalfHeight = FMath::Max(SafeRadius, Settings.AgentHalfHeight);
        const int32 SafeSegments = FMath::Clamp(Settings.PathSegments, 6, 64);
        const FVector CenterOffset(0.0, 0.0, SafeHalfHeight + 2.0f);
        const FVector StartCenter = StartFeetWorld + CenterOffset;
        const FVector EndCenter = EndFeetWorld + CenterOffset;

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(OmniMantlePathValidation), false);
        if (IgnoreActor)
        {
            QueryParams.AddIgnoredActor(IgnoreActor);
        }

        const FCollisionShape AgentShape = FCollisionShape::MakeCapsule(SafeRadius, SafeHalfHeight);
        FVector PreviousPoint = StartCenter;

        for (int32 Segment = 1; Segment <= SafeSegments; ++Segment)
        {
            const float Alpha = static_cast<float>(Segment) / static_cast<float>(SafeSegments);
            const FVector CurrentPoint = CalculateMantlePathPoint(StartCenter, EndCenter, Alpha, Settings.ArcHeight);

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
                return EOmniMantleValidationResult::Blocked;
            }

            PreviousPoint = CurrentPoint;
        }

        return EOmniMantleValidationResult::Valid;
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
