#include "OmniPreviewUtils.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "OmniTraversalUtils.h"

namespace
{
    void DrawArrowhead(
        UWorld* World,
        const FVector& Start,
        const FVector& End,
        const FColor& Color,
        float Thickness)
    {
        if (!World)
        {
            return;
        }

        const FVector Direction = (End - Start).GetSafeNormal();
        if (Direction.IsNearlyZero())
        {
            return;
        }

        FVector Side = FVector::CrossProduct(Direction, FVector::UpVector).GetSafeNormal();
        if (Side.IsNearlyZero())
        {
            Side = FVector::CrossProduct(Direction, FVector::RightVector).GetSafeNormal();
        }

        constexpr float ArrowLength = 14.0f;
        constexpr float ArrowWidth = 7.0f;
        const FVector Base = End - Direction * ArrowLength;
        DrawDebugLine(World, End, Base + Side * ArrowWidth, Color, false, -1.0f, 0, Thickness);
        DrawDebugLine(World, End, Base - Side * ArrowWidth, Color, false, -1.0f, 0, Thickness);
    }

    void DrawDirectionalArrowheads(
        UWorld* World,
        const FVector& Source,
        const FVector& Target,
        ENavLinkDirection::Type Direction,
        const FColor& Color,
        float Thickness)
    {
        if (Direction == ENavLinkDirection::RightToLeft)
        {
            DrawArrowhead(World, Target, Source, Color, Thickness);
            return;
        }

        DrawArrowhead(World, Source, Target, Color, Thickness);
        if (Direction == ENavLinkDirection::BothWays)
        {
            DrawArrowhead(World, Target, Source, Color, Thickness);
        }
    }

    void DrawBallisticArc(
        UWorld* World,
        const FVector& Start,
        const FVector& End,
        const FColor& Color,
        float ApexHeight,
        int32 SegmentCount,
        float Thickness)
    {
        if (!World)
        {
            return;
        }

        FVector LaunchVelocity = FVector::ZeroVector;
        float TotalTime = 0.0f;
        constexpr float StandardGravityMagnitude = 980.0f;
        if (!OmniTraversal::CalculateBallisticLaunch(Start, End, ApexHeight, StandardGravityMagnitude, LaunchVelocity, TotalTime))
        {
            DrawDebugLine(World, Start, End, Color, false, -1.0f, 0, Thickness);
            return;
        }

        const int32 SafeSegments = FMath::Clamp(SegmentCount, 4, 64);
        FVector Previous = Start;
        for (int32 Segment = 1; Segment <= SafeSegments; ++Segment)
        {
            const float Alpha = static_cast<float>(Segment) / static_cast<float>(SafeSegments);
            const float Time = TotalTime * Alpha;
            FVector Point = Start + LaunchVelocity * Time;
            Point.Z -= 0.5f * StandardGravityMagnitude * Time * Time;
            DrawDebugLine(World, Previous, Point, Color, false, -1.0f, 0, Thickness);
            Previous = Point;
        }
    }
}

namespace OmniPreview
{
    FColor SourceColor()
    {
        return FColor(70, 200, 255);
    }

    FColor TargetColor()
    {
        return FColor(255, 160, 60);
    }

    FColor NativeColor()
    {
        return FColor(110, 255, 130);
    }

    FColor JumpColor()
    {
        return FColor(255, 220, 80);
    }

    FColor MantleColor()
    {
        return FColor(80, 200, 255);
    }

    FColor CustomColor()
    {
        return FColor(220, 100, 255);
    }

    FColor FallbackColor()
    {
        return FColor(255, 145, 40);
    }

    FColor RejectedColor()
    {
        return FColor(255, 70, 70);
    }

    void DrawSamplePoint(UWorld* World, const FVector& WorldPoint, const FColor& Color, float PointSize, const FVector& Offset)
    {
        if (World)
        {
            DrawDebugPoint(World, WorldPoint + Offset, PointSize, Color, false, -1.0f, 0);
        }
    }

    void DrawFallbackMarker(UWorld* World, const FVector& SourceWorld, const FVector& TargetWorld, const FVector& Offset)
    {
        if (!World)
        {
            return;
        }

        const FVector Center = FMath::Lerp(SourceWorld, TargetWorld, 0.5f) + Offset + FVector(0.0, 0.0, 6.0);
        DrawDebugPoint(World, Center, 12.0f, FallbackColor(), false, -1.0f, 0);
    }

    void DrawRejectedReasonLabel(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        const FString& Reason,
        const FVector& Offset)
    {
        if (!World || Reason.IsEmpty())
        {
            return;
        }

        const FVector DirectionVector = (TargetWorld - SourceWorld).GetSafeNormal();
        FVector Side = FVector::CrossProduct(DirectionVector, FVector::UpVector).GetSafeNormal();
        if (Side.IsNearlyZero())
        {
            Side = FVector::RightVector;
        }

        const float SideSign = Direction == ENavLinkDirection::RightToLeft ? -1.0f : 1.0f;
        const FVector LabelLocation = FMath::Lerp(SourceWorld, TargetWorld, 0.5f) + Offset + Side * (18.0f * SideSign) + FVector(0.0f, 0.0f, 18.0f);
        const TCHAR* DirectionPrefix = Direction == ENavLinkDirection::RightToLeft ? TEXT("T->S: ") : TEXT("S->T: ");
        DrawDebugString(World, LabelLocation, FString(DirectionPrefix) + Reason, nullptr, RejectedColor(), 0.0f, false, 1.0f);
    }

    FVector GetRejectedFailureLocation(
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        uint8 FailureCode,
        bool bSpeedFailure,
        bool bMantleTraversal,
        float ArcHeight,
        bool bHasExactFailureLocation,
        const FVector& ExactFailureLocation)
    {
        const bool bReverse = Direction == ENavLinkDirection::RightToLeft;
        const FVector Start = bReverse ? TargetWorld : SourceWorld;
        const FVector End = bReverse ? SourceWorld : TargetWorld;

        FVector MarkerLocation = FMath::Lerp(Start, End, 0.5f);
        if (!bSpeedFailure)
        {
            switch (FailureCode)
            {
            case 1: // Departure support
                MarkerLocation = Start;
                break;

            case 2: // Departure clearance
                MarkerLocation = bHasExactFailureLocation ? ExactFailureLocation : Start;
                break;

            case 4: // Landing support
                MarkerLocation = End;
                break;

            case 5: // Landing clearance
                MarkerLocation = bHasExactFailureLocation ? ExactFailureLocation : End;
                break;

            case 3: // Travel path
                if (bHasExactFailureLocation)
                {
                    MarkerLocation = ExactFailureLocation;
                }
                else if (bMantleTraversal)
                {
                    MarkerLocation = OmniTraversal::CalculateMantlePathPoint(Start, End, 0.5f, ArcHeight);
                }
                else
                {
                    FVector LaunchVelocity = FVector::ZeroVector;
                    float TotalTime = 0.0f;
                    constexpr float StandardGravityMagnitude = 980.0f;
                    if (OmniTraversal::CalculateBallisticLaunch(Start, End, ArcHeight, StandardGravityMagnitude, LaunchVelocity, TotalTime))
                    {
                        const float Time = TotalTime * 0.5f;
                        MarkerLocation = Start + LaunchVelocity * Time;
                        MarkerLocation.Z -= 0.5f * StandardGravityMagnitude * Time * Time;
                    }
                }
                break;

            default:
                break;
            }
        }

        return MarkerLocation;
    }

    void DrawRejectedFailureMarker(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        uint8 FailureCode,
        bool bSpeedFailure,
        bool bMantleTraversal,
        float ArcHeight,
        const FVector& Offset,
        bool bHasExactFailureLocation,
        const FVector& ExactFailureLocation)
    {
        if (!World)
        {
            return;
        }

        const FVector MarkerLocation = GetRejectedFailureLocation(
            SourceWorld,
            TargetWorld,
            Direction,
            FailureCode,
            bSpeedFailure,
            bMantleTraversal,
            ArcHeight,
            bHasExactFailureLocation,
            ExactFailureLocation) + Offset;

        const FColor Color = RejectedColor();
        DrawDebugSphere(World, MarkerLocation, 8.0f, 8, Color, false, -1.0f, 0, 1.5f);
        DrawDebugPoint(World, MarkerLocation, 10.0f, Color, false, -1.0f, 0);
    }

    void DrawTraversal(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        bool bUsesJumpTraversal,
        float JumpApexHeight,
        int32 JumpArcSegments,
        float LineThickness,
        const FVector& Offset,
        const FColor* OverrideColor)
    {
        if (!World)
        {
            return;
        }

        const FColor Color = OverrideColor ? *OverrideColor : (bUsesJumpTraversal ? JumpColor() : NativeColor());
        const FVector Source = SourceWorld + Offset;
        const FVector Target = TargetWorld + Offset;

        if (!bUsesJumpTraversal)
        {
            DrawDebugLine(World, Source, Target, Color, false, -1.0f, 0, LineThickness);
            DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
            return;
        }

        if (Direction == ENavLinkDirection::RightToLeft)
        {
            DrawBallisticArc(World, Target, Source, Color, JumpApexHeight, JumpArcSegments, LineThickness);
            DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
            return;
        }

        DrawBallisticArc(World, Source, Target, Color, JumpApexHeight, JumpArcSegments, LineThickness);
        if (Direction == ENavLinkDirection::BothWays)
        {
            DrawBallisticArc(World, Target, Source, Color, JumpApexHeight, JumpArcSegments, LineThickness);
        }
        DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
    }
    void DrawMantleTraversal(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        float ArcHeight,
        int32 PathSegments,
        float LineThickness,
        const FVector& Offset,
        const FColor* OverrideColor)
    {
        if (!World)
        {
            return;
        }

        const FVector Source = SourceWorld + Offset;
        const FVector Target = TargetWorld + Offset;
        const FColor Color = OverrideColor ? *OverrideColor : MantleColor();
        const int32 SegmentCount = FMath::Clamp(PathSegments, 6, 64);

        auto DrawArc = [&](const FVector& Start, const FVector& End, const FVector& SideOffset)
        {
            FVector Previous = Start + SideOffset;
            for (int32 Segment = 1; Segment <= SegmentCount; ++Segment)
            {
                const float Alpha = static_cast<float>(Segment) / static_cast<float>(SegmentCount);
                FVector Point = OmniTraversal::CalculateMantlePathPoint(Start, End, Alpha, ArcHeight);
                Point += SideOffset;
                DrawDebugLine(World, Previous, Point, Color, false, -1.0f, 0, LineThickness);
                Previous = Point;
            }
        };

        if (Direction == ENavLinkDirection::RightToLeft)
        {
            DrawArc(Target, Source, FVector::ZeroVector);
            DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
            return;
        }

        DrawArc(Source, Target, FVector::ZeroVector);
        if (Direction == ENavLinkDirection::BothWays)
        {
            const FVector SideOffset = FVector::CrossProduct((Target - Source).GetSafeNormal(), FVector::UpVector).GetSafeNormal() * 4.0f;
            DrawArc(Target, Source, SideOffset);
        }
        DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
    }

    void DrawCustomTraversal(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        float LineThickness,
        const FVector& Offset)
    {
        if (!World)
        {
            return;
        }

        const FVector Source = SourceWorld + Offset;
        const FVector Target = TargetWorld + Offset;
        const FColor Color = CustomColor();
        constexpr int32 DashSegments = 16;

        auto DrawDashed = [&](const FVector& Start, const FVector& End)
        {
            for (int32 Segment = 0; Segment < DashSegments; Segment += 2)
            {
                const float A0 = static_cast<float>(Segment) / static_cast<float>(DashSegments);
                const float A1 = static_cast<float>(Segment + 1) / static_cast<float>(DashSegments);
                DrawDebugLine(World, FMath::Lerp(Start, End, A0), FMath::Lerp(Start, End, A1), Color, false, -1.0f, 0, LineThickness);
            }
        };

        if (Direction == ENavLinkDirection::RightToLeft)
        {
            DrawDashed(Target, Source);
            DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
            return;
        }

        DrawDashed(Source, Target);
        if (Direction == ENavLinkDirection::BothWays)
        {
            const FVector SideOffset = FVector::CrossProduct((Target - Source).GetSafeNormal(), FVector::UpVector).GetSafeNormal() * 4.0f;
            DrawDashed(Target + SideOffset, Source + SideOffset);
        }
        DrawDirectionalArrowheads(World, Source, Target, Direction, Color, LineThickness);
    }

}
