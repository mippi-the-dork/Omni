#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavLinkDefinition.h"

class UWorld;

namespace OmniPreview
{
    FColor SourceColor();
    FColor TargetColor();
    FColor NativeColor();
    FColor JumpColor();
    FColor MantleColor();
    FColor CustomColor();
    FColor FallbackColor();
    FColor RejectedColor();

    void DrawSamplePoint(UWorld* World, const FVector& WorldPoint, const FColor& Color, float PointSize, const FVector& Offset);
    void DrawFallbackMarker(UWorld* World, const FVector& SourceWorld, const FVector& TargetWorld, const FVector& Offset);

    /** Draws a short editor-only reason label beside one rejected directional traversal. */
    void DrawRejectedReasonLabel(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        const FString& Reason,
        const FVector& Offset);

    /** Draws a compact marker at the diagnostic stage location for one rejected traversal. */
    void DrawRejectedFailureMarker(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        uint8 FailureCode,
        bool bSpeedFailure,
        bool bMantleTraversal,
        float ArcHeight,
        const FVector& Offset);

    /**
     * Draws one generated Omni traversal using the same directional convention for
     * Point, Volume, and Spline authoring actors. BothWays Jump links draw both
     * ballistic directions because the two arcs can differ when endpoint heights differ.
     */
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
        const FColor* OverrideColor = nullptr);

    /** Draws built-in Mantle traversal as a compact eased arc. */
    void DrawMantleTraversal(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        float ArcHeight,
        int32 PathSegments,
        float LineThickness,
        const FVector& Offset,
        const FColor* OverrideColor = nullptr);

    /** Draws designer-defined Custom traversal as a dashed directional connection. */
    void DrawCustomTraversal(
        UWorld* World,
        const FVector& SourceWorld,
        const FVector& TargetWorld,
        ENavLinkDirection::Type Direction,
        float LineThickness,
        const FVector& Offset);
}
