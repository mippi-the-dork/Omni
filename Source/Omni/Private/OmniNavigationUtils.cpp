#include "OmniNavigationUtils.h"

#include "NavLinkComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"

namespace
{
    int32 CountSelectedAgents(const FNavAgentSelector& Selector)
    {
        int32 Count = 0;
        for (int32 AgentIndex = 0; AgentIndex < FNavAgentSelector::AgentsMaxCount; ++AgentIndex)
        {
            if (Selector.Contains(AgentIndex))
            {
                ++Count;
            }
        }
        return Count;
    }

    int32 CountCompatibleAgents(const FNavAgentSelector& A, const FNavAgentSelector& B)
    {
        int32 Count = 0;
        for (int32 AgentIndex = 0; AgentIndex < FNavAgentSelector::AgentsMaxCount; ++AgentIndex)
        {
            if (A.Contains(AgentIndex) && B.Contains(AgentIndex))
            {
                ++Count;
            }
        }
        return Count;
    }
}

void OmniNavigation::CanonicalizeNativeSimpleLink(
    FVector& InOutLeft,
    FVector& InOutRight,
    ENavLinkDirection::Type& InOutDirection)
{
    if (InOutDirection == ENavLinkDirection::RightToLeft)
    {
        Swap(InOutLeft, InOutRight);
        InOutDirection = ENavLinkDirection::LeftToRight;
    }
}

void OmniNavigation::RefreshNativeLinkComponentNavigation(UNavLinkComponent& LinkComponent)
{
    if (!LinkComponent.IsRegistered() || !LinkComponent.GetWorld())
    {
        return;
    }

    // Omni mutates UNavLinkComponent::Links after the component may already have
    // completed its native registration lifecycle. A component that registered
    // while Links was empty can otherwise remain absent from the navigation octree
    // after valid links are generated later. Re-register through Unreal's public
    // runtime component API so empty -> valid, valid -> valid, and valid -> empty
    // transitions all rebuild native navigation state consistently in editor and
    // packaged game targets.
    LinkComponent.ReregisterComponent();
}

void OmniNavigation::AppendSupportedAgentCompatibilityWarning(
    UObject* WorldContextObject,
    const FNavAgentSelector& SupportedAgents,
    FString& InOutStatus)
{
    const int32 SelectedAgentCount = CountSelectedAgents(SupportedAgents);
    if (SelectedAgentCount == 0)
    {
        InOutStatus += TEXT(" Warning: Supported Agents has no agents selected, so no navigation agent can use these links.");
        return;
    }

    UNavigationSystemV1* NavigationSystem = WorldContextObject
        ? UNavigationSystemV1::GetCurrent(WorldContextObject)
        : nullptr;
    if (!NavigationSystem)
    {
        return;
    }

    const FNavAgentSelector WorldSupportedAgents = NavigationSystem->GetSupportedAgentsMask();
    if (CountCompatibleAgents(SupportedAgents, WorldSupportedAgents) == 0)
    {
        InOutStatus += FString::Printf(
            TEXT(" Warning: Supported Agents selects %d agent%s, but none overlap this world's Navigation System supported-agent mask."),
            SelectedAgentCount,
            SelectedAgentCount == 1 ? TEXT("") : TEXT("s"));
    }
}


bool OmniNavigation::ShouldRegenerateForNavigationData(
    UObject* WorldContextObject,
    const FNavAgentSelector& SupportedAgents,
    const ANavigationData* NavData)
{
    if (!WorldContextObject || !NavData)
    {
        // Callers already validate these in the normal path. Remaining conservative
        // here avoids accidentally suppressing a rebuild if this helper is reused.
        return true;
    }

    UNavigationSystemV1* NavigationSystem = UNavigationSystemV1::GetCurrent(WorldContextObject);
    if (!NavigationSystem)
    {
        return true;
    }

    const int32 AgentIndex = NavigationSystem->GetSupportedAgentIndex(NavData);
    if (AgentIndex == INDEX_NONE)
    {
        // Abstract/custom/unregistered NavData may not map to a configured supported
        // agent. Preserve the historical rebuild behavior for unknown data.
        return true;
    }

    return AgentIndex >= 0
        && AgentIndex < FNavAgentSelector::AgentsMaxCount
        && SupportedAgents.Contains(AgentIndex);
}
