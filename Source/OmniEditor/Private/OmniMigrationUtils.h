#pragma once

#include "AI/Navigation/NavLinkDefinition.h"
#include "OmniPointNavLink.h"
#include "OmniSplineNavLink.h"

namespace OmniMigration
{
    EOmniPointLinkDirection ToPointDirection(ENavLinkDirection::Type NativeDirection);
    EOmniSplineLinkDirection ToSplineDirection(ENavLinkDirection::Type NativeDirection);
    float ResolveProjectionHeight(const FNavigationLinkBase& Link);
    float ResolveSnapRadius(float NativeSnapRadius);
    float ResolveSegmentSpacing(float NativeSnapRadius);
    int32 ResolveSegmentSampleCount(float LongestSegmentLength, float Spacing);
}
