#include "OmniMigrationUtils.h"

namespace OmniMigration
{
    EOmniPointLinkDirection ToPointDirection(const ENavLinkDirection::Type NativeDirection)
    {
        switch (NativeDirection)
        {
        case ENavLinkDirection::LeftToRight:
            return EOmniPointLinkDirection::SourceToTarget;
        case ENavLinkDirection::RightToLeft:
            return EOmniPointLinkDirection::TargetToSource;
        case ENavLinkDirection::BothWays:
        default:
            return EOmniPointLinkDirection::BothWays;
        }
    }

    EOmniSplineLinkDirection ToSplineDirection(const ENavLinkDirection::Type NativeDirection)
    {
        switch (NativeDirection)
        {
        case ENavLinkDirection::LeftToRight:
            return EOmniSplineLinkDirection::SourceToTarget;
        case ENavLinkDirection::RightToLeft:
            return EOmniSplineLinkDirection::TargetToSource;
        case ENavLinkDirection::BothWays:
        default:
            return EOmniSplineLinkDirection::BothWays;
        }
    }

    float ResolveProjectionHeight(const FNavigationLinkBase& Link)
    {
        return FMath::Max(0.0f, FMath::Max(Link.LeftProjectHeight, Link.MaxFallDownLength));
    }

    float ResolveSnapRadius(const float NativeSnapRadius)
    {
        return FMath::Max(1.0f, NativeSnapRadius);
    }

    float ResolveSegmentSpacing(const float NativeSnapRadius)
    {
        return FMath::Clamp(FMath::Max(10.0f, NativeSnapRadius), 10.0f, 500.0f);
    }

    int32 ResolveSegmentSampleCount(const float LongestSegmentLength, const float Spacing)
    {
        const float SafeSpacing = FMath::Max(1.0f, Spacing);
        return FMath::Max(2, FMath::CeilToInt(FMath::Max(0.0f, LongestSegmentLength) / SafeSpacing) + 1);
    }
}
