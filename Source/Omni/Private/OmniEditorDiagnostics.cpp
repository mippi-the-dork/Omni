#include "OmniEditorDiagnostics.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#endif

namespace
{
#if WITH_EDITOR
    void ResolveBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects, TArray<AActor*>& OutActors)
    {
        OutActors.Reset();
        if (!World || BlockingObjects.IsEmpty())
        {
            return;
        }

        TSet<FName> RequestedActorNames;
        for (const FString& BlockingObject : BlockingObjects)
        {
            FString ActorName;
            FString ComponentName;
            if (!BlockingObject.Split(TEXT("."), &ActorName, &ComponentName))
            {
                ActorName = BlockingObject;
            }

            ActorName.TrimStartAndEndInline();
            if (!ActorName.IsEmpty())
            {
                RequestedActorNames.Add(FName(*ActorName));
            }
        }

        if (RequestedActorNames.IsEmpty())
        {
            return;
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (IsValid(Actor) && RequestedActorNames.Contains(Actor->GetFName()))
            {
                OutActors.Add(Actor);
            }
        }
    }

    void SelectActors(const TArray<AActor*>& Actors)
    {
        if (!GEditor || Actors.IsEmpty())
        {
            return;
        }

        GEditor->SelectNone(false, true, false);
        for (AActor* Actor : Actors)
        {
            GEditor->SelectActor(Actor, true, false, true);
        }
        GEditor->NoteSelectionChange();
    }
#endif
}

namespace OmniEditorDiagnostics
{
    int32 SelectBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects)
    {
#if WITH_EDITOR
        if (!GEditor)
        {
            return 0;
        }

        TArray<AActor*> Actors;
        ResolveBlockingActors(World, BlockingObjects, Actors);
        if (Actors.IsEmpty())
        {
            return 0;
        }

        SelectActors(Actors);
        return Actors.Num();
#else
        return 0;
#endif
    }

    int32 FocusBlockingActors(UWorld* World, const TArray<FString>& BlockingObjects)
    {
#if WITH_EDITOR
        if (!GEditor)
        {
            return 0;
        }

        TArray<AActor*> Actors;
        ResolveBlockingActors(World, BlockingObjects, Actors);
        if (Actors.IsEmpty())
        {
            return 0;
        }

        SelectActors(Actors);
        GEditor->MoveViewportCamerasToActor(Actors, true);
        return Actors.Num();
#else
        return 0;
#endif
    }


    int32 FocusFailureLocations(const TArray<FVector>& FailureLocations)
    {
#if WITH_EDITOR
        if (!GEditor || FailureLocations.IsEmpty())
        {
            return 0;
        }

        FBox FocusBox(ForceInit);
        for (const FVector& Location : FailureLocations)
        {
            FocusBox += Location;
        }

        if (!FocusBox.IsValid)
        {
            return 0;
        }

        // Give single-point and tightly clustered failures enough context to be useful.
        const FVector MinimumExtent(75.0f);
        const FVector CurrentExtent = FocusBox.GetExtent();
        FocusBox = FBox::BuildAABB(FocusBox.GetCenter(), FVector(
            FMath::Max(CurrentExtent.X, MinimumExtent.X),
            FMath::Max(CurrentExtent.Y, MinimumExtent.Y),
            FMath::Max(CurrentExtent.Z, MinimumExtent.Z)));

        GEditor->MoveViewportCamerasToBox(FocusBox, true, 0.0f);
        return FailureLocations.Num();
#else
        return 0;
#endif
    }
}
