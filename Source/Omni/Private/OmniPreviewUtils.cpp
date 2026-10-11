#include "OmniPreviewUtils.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "OmniTraversalUtils.h"

namespace
{
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

    FColor RejectedColor()
    {
        return FColor(255, 70, 70);
    }

    FColor DirectionColor(ENavLinkDirection::Type Direction)
    {
        if (Direction == ENavLinkDirection::LeftToRight)
        {
            return FColor(255, 220, 80);
        }
        if (Direction == ENavLinkDirection::RightToLeft)
        {
            return FColor(220, 100, 255);
        }
        return FColor(110, 255, 130);
    }

    void DrawSamplePoint(UWorld* World, const FVector& WorldPoint, const FColor& Color, float PointSize, const FVector& Offset)
    {
        if (World)
        {
            DrawDebugPoint(World, WorldPoint + Offset, PointSize, Color, false, -1.0f, 0);
        }
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

        const FColor Color = OverrideColor ? *OverrideColor : DirectionColor(Direction);
        const FVector Source = SourceWorld + Offset;
        const FVector Target = TargetWorld + Offset;

        if (!bUsesJumpTraversal)
        {
            DrawDebugLine(World, Source, Target, Color, false, -1.0f, 0, LineThickness);
            return;
        }

        if (Direction == ENavLinkDirection::RightToLeft)
        {
            DrawBallisticArc(World, Target, Source, Color, JumpApexHeight, JumpArcSegments, LineThickness);
            return;
        }

        DrawBallisticArc(World, Source, Target, Color, JumpApexHeight, JumpArcSegments, LineThickness);
        if (Direction == ENavLinkDirection::BothWays)
        {
            DrawBallisticArc(World, Target, Source, Color, JumpApexHeight, JumpArcSegments, LineThickness);
        }
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
        const FColor Color = DirectionColor(Direction);
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
            return;
        }

        DrawDashed(Source, Target);
        if (Direction == ENavLinkDirection::BothWays)
        {
            const FVector SideOffset = FVector::CrossProduct((Target - Source).GetSafeNormal(), FVector::UpVector).GetSafeNormal() * 4.0f;
            DrawDashed(Target + SideOffset, Source + SideOffset);
        }
    }

}
