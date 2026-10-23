#pragma once

#include "CoreMinimal.h"

class UWorld;

namespace OmniEditorDiagnostics
{
    /** Selects each unique actor named by diagnostic blocker strings of the form Actor or Actor.Component. */
    int32 SelectBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects);

    /** Selects the diagnostic blockers and focuses the active Level Editor viewport on the resolved actor set. */
    int32 FocusBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects);

    /** Frames one or more world-space diagnostic failure locations in the active Level Editor viewport. */
    int32 FocusFailureLocations(const TArray<FVector>& FailureLocations);
}
