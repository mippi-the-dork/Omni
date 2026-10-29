#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavAgentSelector.h"

class ANavigationData;

namespace OmniNavigation
{
    /**
     * Appends a concise warning when an Omni actor's Supported Agents mask cannot
     * be used by any configured navigation agent in the current world.
     *
     * The authored selector is never modified. This is diagnostic-only so native
     * Unreal agent-mask semantics remain authoritative.
     */
    void AppendSupportedAgentCompatibilityWarning(
        UObject* WorldContextObject,
        const FNavAgentSelector& SupportedAgents,
        FString& InOutStatus);

    /**
     * Returns true when a completed NavData build is relevant to this authored
     * Supported Agents mask. Recognized NavData instances are filtered by the
     * Navigation System's native supported-agent index. Unknown/unmapped NavData
     * remains conservative and returns true so Omni never risks leaving stale links.
     */
    bool ShouldRegenerateForNavigationData(
        UObject* WorldContextObject,
        const FNavAgentSelector& SupportedAgents,
        const ANavigationData* NavData);
}
