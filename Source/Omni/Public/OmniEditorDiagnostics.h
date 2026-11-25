#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UWorld;

/**
 * Runtime-safe bridge for optional editor diagnostics.
 *
 * Omni runtime actors can expose CallInEditor actions without linking the Runtime
 * module against UnrealEd. OmniEditor registers the handlers while the editor
 * module is loaded; packaged/runtime targets simply receive zero/no-op results.
 */
namespace OmniEditorDiagnostics
{
    using FBlockingActorsHandler = TFunction<int32(UWorld*, const TArray<FString>&)>;
    using FFailureLocationsHandler = TFunction<int32(const TArray<FVector>&)>;

    OMNI_API void RegisterHandlers(
        FBlockingActorsHandler InSelectBlockingActors,
        FBlockingActorsHandler InFocusBlockingActors,
        FFailureLocationsHandler InFocusFailureLocations);

    OMNI_API void UnregisterHandlers();

    /** Selects each unique actor named by diagnostic blocker strings of the form Actor or Actor.Component. */
    OMNI_API int32 SelectBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects);

    /** Selects the diagnostic blockers and focuses the active Level Editor viewport on the resolved actor set. */
    OMNI_API int32 FocusBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects);

    /** Frames one or more world-space diagnostic failure locations in the active Level Editor viewport. */
    OMNI_API int32 FocusFailureLocations(const TArray<FVector>& FailureLocations);
}
