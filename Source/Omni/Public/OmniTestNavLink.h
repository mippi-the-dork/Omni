#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "GameFramework/Actor.h"
#include "OmniTestNavLink.generated.h"

class UBoxComponent;
class UNavAreaBase;
class UNavLinkComponent;
class USceneComponent;

/**
 * Phase 0 Omni proof actor.
 *
 * The designer edits two boxes. Omni generates several ordinary FNavigationLink
 * connections between corresponding positions across those boxes and supplies
 * them through a native UNavLinkComponent.
 */
UCLASS(Blueprintable)
class OMNI_API AOmniTestNavLink : public AActor
{
    GENERATED_BODY()

public:
    AOmniTestNavLink();

    virtual void OnConstruction(const FTransform& Transform) override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void PostEditUndo() override;
#endif

    /** Rebuild the native links from the current Source and Target box transforms. */
    UFUNCTION(CallInEditor, Category="Omni|Generation", meta=(DisplayName="Rebuild Generated Links"))
    void RebuildGeneratedLinks();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UBoxComponent> SourceBox;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UBoxComponent> TargetBox;

    /** Native Unreal component that owns the generated FNavigationLink array. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Omni|Components")
    TObjectPtr<UNavLinkComponent> GeneratedLinks;

    /** Number of evenly distributed links generated across the boxes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation", meta=(ClampMin="1", ClampMax="128", UIMin="1", UIMax="32"))
    int32 LinkCount = 5;

    /** Traversal direction applied to every generated link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation")
    TEnumAsByte<ENavLinkDirection::Type> Direction = ENavLinkDirection::BothWays;

    /** Navigation area used by every generated link. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation")
    TSubclassOf<UNavAreaBase> AreaClass;

    /** Horizontal snap radius used by Recast when attaching link endpoints to polygons. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation", meta=(ClampMin="1.0", UIMin="1.0"))
    float SnapRadius = 30.0f;

    /** Downward projection distance applied to both generated link endpoints. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Omni|Generation", meta=(ClampMin="0.0", UIMin="0.0"))
    float ProjectionHeight = 100.0f;

    /** Current number of generated native links. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="Omni|Debug")
    int32 GeneratedLinkCount = 0;

private:
    void NotifyNavigationSystem();
};
