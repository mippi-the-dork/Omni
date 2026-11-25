#include "OmniEditorDiagnostics.h"

namespace
{
    OmniEditorDiagnostics::FBlockingActorsHandler SelectBlockingActorsHandler;
    OmniEditorDiagnostics::FBlockingActorsHandler FocusBlockingActorsHandler;
    OmniEditorDiagnostics::FFailureLocationsHandler FocusFailureLocationsHandler;
}

namespace OmniEditorDiagnostics
{
    void RegisterHandlers(
        FBlockingActorsHandler InSelectBlockingActors,
        FBlockingActorsHandler InFocusBlockingActors,
        FFailureLocationsHandler InFocusFailureLocations)
    {
        SelectBlockingActorsHandler = MoveTemp(InSelectBlockingActors);
        FocusBlockingActorsHandler = MoveTemp(InFocusBlockingActors);
        FocusFailureLocationsHandler = MoveTemp(InFocusFailureLocations);
    }

    void UnregisterHandlers()
    {
        SelectBlockingActorsHandler = nullptr;
        FocusBlockingActorsHandler = nullptr;
        FocusFailureLocationsHandler = nullptr;
    }

    int32 SelectBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects)
    {
        return SelectBlockingActorsHandler ? SelectBlockingActorsHandler(World, BlockingObjects) : 0;
    }

    int32 FocusBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects)
    {
        return FocusBlockingActorsHandler ? FocusBlockingActorsHandler(World, BlockingObjects) : 0;
    }

    int32 FocusFailureLocations(const TArray<FVector>& FailureLocations)
    {
        return FocusFailureLocationsHandler ? FocusFailureLocationsHandler(FailureLocations) : 0;
    }
}
