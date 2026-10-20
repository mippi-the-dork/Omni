#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavLinkDefinition.h"

class UWorld;

namespace OmniPreview
{
    FColor SourceColor();
    FColor TargetColor();
    FColor RejectedColor();
    FColor DirectionColor(ENavLinkDirection::Type Direction);

    void DrawSamplePoint(UWorld* World, const FVector& WorldPoint, const FColor& Color, float PointSize, const FVector& Offset);

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
