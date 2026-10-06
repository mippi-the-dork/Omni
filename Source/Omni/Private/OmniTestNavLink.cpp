#include "OmniTestNavLink.h"

#include "AI/Navigation/NavAgentSelector.h"
#include "AI/NavigationSystemBase.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "NavAreas/NavArea_Default.h"
#include "NavLinkComponent.h"

AOmniTestNavLink::AOmniTestNavLink()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    SourceBox = CreateDefaultSubobject<UBoxComponent>(TEXT("SourceBox"));
    SourceBox->SetupAttachment(SceneRoot);
    SourceBox->SetRelativeLocation(FVector(-300.0, 0.0, 0.0));
    SourceBox->SetBoxExtent(FVector(50.0, 250.0, 50.0));
    SourceBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SourceBox->SetGenerateOverlapEvents(false);
    SourceBox->SetCanEverAffectNavigation(false);

    TargetBox = CreateDefaultSubobject<UBoxComponent>(TEXT("TargetBox"));
    TargetBox->SetupAttachment(SceneRoot);
    TargetBox->SetRelativeLocation(FVector(300.0, 0.0, 0.0));
    TargetBox->SetBoxExtent(FVector(50.0, 250.0, 50.0));
    TargetBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TargetBox->SetGenerateOverlapEvents(false);
    TargetBox->SetCanEverAffectNavigation(false);

    GeneratedLinks = CreateDefaultSubobject<UNavLinkComponent>(TEXT("GeneratedLinks"));
    GeneratedLinks->SetupAttachment(SceneRoot);

    AreaClass = UNavArea_Default::StaticClass();
}

void AOmniTestNavLink::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RebuildGeneratedLinks();
}

void AOmniTestNavLink::RebuildGeneratedLinks()
{
    if (!SourceBox || !TargetBox || !GeneratedLinks)
    {
        GeneratedLinkCount = 0;
        return;
    }

    const int32 SafeLinkCount = FMath::Clamp(LinkCount, 1, 128);
    const FVector SourceExtent = SourceBox->GetUnscaledBoxExtent();
    const FVector TargetExtent = TargetBox->GetUnscaledBoxExtent();

    TArray<FNavigationLink> NewLinks;
    NewLinks.Reserve(SafeLinkCount);

    const FTransform SourceTransform = SourceBox->GetComponentTransform();
    const FTransform TargetTransform = TargetBox->GetComponentTransform();
    const FTransform LinkTransform = GeneratedLinks->GetComponentTransform();

    UClass* ResolvedAreaClass = AreaClass.Get();
    if (!ResolvedAreaClass)
    {
        ResolvedAreaClass = UNavArea_Default::StaticClass();
    }

    for (int32 Index = 0; Index < SafeLinkCount; ++Index)
    {
        const double Alpha = SafeLinkCount == 1
            ? 0.5
            : static_cast<double>(Index) / static_cast<double>(SafeLinkCount - 1);

        const double SourceY = FMath::Lerp(-SourceExtent.Y, SourceExtent.Y, Alpha);
        const double TargetY = FMath::Lerp(-TargetExtent.Y, TargetExtent.Y, Alpha);

        const FVector SourceWorld = SourceTransform.TransformPosition(FVector(0.0, SourceY, 0.0));
        const FVector TargetWorld = TargetTransform.TransformPosition(FVector(0.0, TargetY, 0.0));

        const FVector SourceLinkLocal = LinkTransform.InverseTransformPosition(SourceWorld);
        const FVector TargetLinkLocal = LinkTransform.InverseTransformPosition(TargetWorld);

        FNavigationLink& Link = NewLinks.Emplace_GetRef(SourceLinkLocal, TargetLinkLocal);
        Link.Direction = Direction;
        Link.SnapRadius = FMath::Max(1.0f, SnapRadius);
        Link.LeftProjectHeight = FMath::Max(0.0f, ProjectionHeight);
        Link.MaxFallDownLength = FMath::Max(0.0f, ProjectionHeight);
        Link.bUseSnapHeight = false;
        Link.bSnapToCheapestArea = false;
        Link.SupportedAgents = FNavAgentSelector(FNavAgentSelector::AllAgentsMask);
        Link.SetAreaClass(ResolvedAreaClass);
    }

    GeneratedLinks->Links = MoveTemp(NewLinks);
    GeneratedLinkCount = GeneratedLinks->Links.Num();

    GeneratedLinks->MarkRenderStateDirty();
    NotifyNavigationSystem();
}

void AOmniTestNavLink::NotifyNavigationSystem()
{
    if (!GeneratedLinks || !GeneratedLinks->IsRegistered() || !GeneratedLinks->GetWorld())
    {
        return;
    }

    FNavigationSystem::UpdateComponentData(*GeneratedLinks);
}

#if WITH_EDITOR
void AOmniTestNavLink::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    RebuildGeneratedLinks();
}

void AOmniTestNavLink::PostEditMove(bool bFinished)
{
    Super::PostEditMove(bFinished);

    if (bFinished)
    {
        RebuildGeneratedLinks();
    }
}

void AOmniTestNavLink::PostEditUndo()
{
    Super::PostEditUndo();
    RebuildGeneratedLinks();
}
#endif
