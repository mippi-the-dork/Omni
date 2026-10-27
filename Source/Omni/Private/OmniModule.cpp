#include "OmniModule.h"

#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "AI/Navigation/NavLinkDefinition.h"
#include "Components/SphereComponent.h"
#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Selection.h"
#include "NavAreas/NavArea_Default.h"
#include "Navigation/NavLinkProxy.h"
#include "NavLinkCustomComponent.h"
#include "NavAreas/NavArea_Null.h"
#include "OmniPointNavLink.h"
#include "OmniSplineNavLink.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"
#endif

#define LOCTEXT_NAMESPACE "FOmniModule"

IMPLEMENT_MODULE(FOmniModule, Omni)

void FOmniModule::StartupModule()
{
#if WITH_EDITOR
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FOmniModule::RegisterEditorMenus));
#endif
}

void FOmniModule::ShutdownModule()
{
#if WITH_EDITOR
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
#endif
}

#if WITH_EDITOR
DEFINE_LOG_CATEGORY_STATIC(LogOmniMigration, Log, All);

namespace
{
EOmniPointLinkDirection ToOmniDirection(const ENavLinkDirection::Type NativeDirection)
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

EOmniSplineLinkDirection ToOmniSplineDirection(const ENavLinkDirection::Type NativeDirection)
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

bool HasConvertiblePointLinks(const ANavLinkProxy* Proxy)
{
    return IsValid(Proxy) && Proxy->PointLinks.Num() > 0;
}

bool HasConvertibleSegmentLinks(const ANavLinkProxy* Proxy)
{
    return IsValid(Proxy) && Proxy->SegmentLinks.Num() > 0;
}

bool HasConvertibleSmartLink(ANavLinkProxy* Proxy)
{
    return IsValid(Proxy) && Proxy->bSmartLinkIsRelevant && IsValid(Proxy->GetSmartLinkComp());
}

template <typename TLinkType>
bool HasCustomFlags(const TLinkType& Link)
{
    return Link.bCustomFlag0 || Link.bCustomFlag1 || Link.bCustomFlag2 || Link.bCustomFlag3
        || Link.bCustomFlag4 || Link.bCustomFlag5 || Link.bCustomFlag6 || Link.bCustomFlag7;
}

void ReportUntranslatedPointLinkSettings(
    const ANavLinkProxy& Proxy,
    const int32 LinkIndex,
    const FNavigationLink& Link,
    int32& InOutWarningCount)
{
    const FString ProxyLabel = Proxy.GetActorLabel();
    const TCHAR* ProxyName = *ProxyLabel;

    if (Link.bUseSnapHeight)
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s PointLinks[%d] uses Snap Height (%.2f). Omni Point does not currently expose native Snap Height, so this setting was not transferred."),
            ProxyName,
            LinkIndex,
            Link.SnapHeight);
    }

    if (Link.bSnapToCheapestArea)
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s PointLinks[%d] uses Snap to Cheapest Area. Omni-generated native links currently keep this disabled, so this setting was not transferred."),
            ProxyName,
            LinkIndex);
    }

    if (HasCustomFlags(Link))
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s PointLinks[%d] uses one or more native custom link flags. Omni Point does not currently author custom FNavigationLink flags, so they were not transferred."),
            ProxyName,
            LinkIndex);
    }

    if (!FMath::IsNearlyEqual(Link.LeftProjectHeight, Link.MaxFallDownLength))
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s PointLinks[%d] uses asymmetric projection heights (Left %.2f, Right %.2f). Omni currently uses one Native Projection Height for both endpoints; the converted actor uses the larger value %.2f."),
            ProxyName,
            LinkIndex,
            Link.LeftProjectHeight,
            Link.MaxFallDownLength,
            FMath::Max(Link.LeftProjectHeight, Link.MaxFallDownLength));
    }
}

void ReportUntranslatedSegmentLinkSettings(
    const ANavLinkProxy& Proxy,
    const int32 LinkIndex,
    const FNavigationSegmentLink& Link,
    int32& InOutWarningCount)
{
    const TCHAR* ProxyName = *Proxy.GetActorLabel();

    if (Link.bUseSnapHeight)
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s SegmentLinks[%d] uses Snap Height (%.2f). Omni Spline does not currently expose native Snap Height, so this setting was not transferred."),
            ProxyName,
            LinkIndex,
            Link.SnapHeight);
    }

    if (Link.bSnapToCheapestArea)
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s SegmentLinks[%d] uses Snap to Cheapest Area. Omni-generated native links currently keep this disabled, so this setting was not transferred."),
            ProxyName,
            LinkIndex);
    }

    if (HasCustomFlags(Link))
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s SegmentLinks[%d] uses one or more native custom link flags. Omni Spline does not currently author custom FNavigationLink flags, so they were not transferred."),
            ProxyName,
            LinkIndex);
    }

    if (!FMath::IsNearlyEqual(Link.LeftProjectHeight, Link.MaxFallDownLength))
    {
        ++InOutWarningCount;
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s SegmentLinks[%d] uses asymmetric projection heights (Left %.2f, Right %.2f). Omni currently uses one Native Projection Height for generated links; the converted actor uses the larger value %.2f."),
            ProxyName,
            LinkIndex,
            Link.LeftProjectHeight,
            Link.MaxFallDownLength,
            FMath::Max(Link.LeftProjectHeight, Link.MaxFallDownLength));
    }
}

void ConfigureLinearSpline(USplineComponent& Spline, const FVector& Start, const FVector& End)
{
    Spline.SetRelativeTransform(FTransform::Identity);
    Spline.ClearSplinePoints(false);
    Spline.AddSplinePoint(Start, ESplineCoordinateSpace::Local, false);
    Spline.AddSplinePoint(End, ESplineCoordinateSpace::Local, false);
    Spline.SetSplinePointType(0, ESplinePointType::Linear, false);
    Spline.SetSplinePointType(1, ESplinePointType::Linear, false);
    Spline.SetClosedLoop(false, false);
    Spline.UpdateSpline();
}
}

void FOmniModule::RegisterEditorMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    UToolMenu* ActorContextMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.ActorContextMenu"));
    if (!ActorContextMenu)
    {
        return;
    }

    FToolMenuSection& OmniSection = ActorContextMenu->FindOrAddSection(
        TEXT("OmniNavLinkMigration"),
        LOCTEXT("OmniNavLinkMigrationSection", "Omni"));

    OmniSection.AddDynamicEntry(
        TEXT("OmniConvertSelectedNavLinkProxiesDynamic"),
        FNewToolMenuSectionDelegate::CreateRaw(this, &FOmniModule::BuildNavLinkMigrationMenu));
}

void FOmniModule::BuildNavLinkMigrationMenu(FToolMenuSection& Section)
{
    const bool bCanConvertPointLinks = CanConvertSelectedNavLinkProxies();
    const bool bCanConvertSegmentLinks = CanConvertSelectedSegmentLinks();
    const bool bCanConvertSmartLinks = CanConvertSelectedSmartLinks();
    if (!bCanConvertPointLinks && !bCanConvertSegmentLinks && !bCanConvertSmartLinks)
    {
        return;
    }

    if (bCanConvertPointLinks)
    {
        Section.AddMenuEntry(
            TEXT("OmniConvertSelectedNavLinkProxies"),
            LOCTEXT("ConvertSelectedNavLinkProxiesLabel", "Convert NavLinkProxy Point Links to Omni"),
            LOCTEXT(
                "ConvertSelectedNavLinkProxiesTooltip",
                "Creates one non-destructive Omni Point actor for each simple Point Link on the selected NavLinkProxy actors. The original NavLinkProxy actors are left unchanged."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FOmniModule::ConvertSelectedNavLinkProxies)));
    }

    if (bCanConvertSegmentLinks)
    {
        Section.AddMenuEntry(
            TEXT("OmniConvertSelectedSegmentLinks"),
            LOCTEXT("ConvertSelectedSegmentLinksLabel", "Convert NavLinkProxy Segment Links to Omni"),
            LOCTEXT(
                "ConvertSelectedSegmentLinksTooltip",
                "Creates one non-destructive Omni Spline actor for each Segment Link on the selected NavLinkProxy actors. The two native segments become matched Source/Target splines; the original NavLinkProxy actors are left unchanged."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FOmniModule::ConvertSelectedSegmentLinks)));
    }

    if (bCanConvertSmartLinks)
    {
        Section.AddMenuEntry(
            TEXT("OmniConvertSelectedSmartLinks"),
            LOCTEXT("ConvertSelectedSmartLinksLabel", "Convert NavLinkProxy Smart Links to Omni"),
            LOCTEXT(
                "ConvertSelectedSmartLinksTooltip",
                "Creates one non-destructive Omni Point Custom traversal actor for each relevant Smart Link on the selected NavLinkProxy actors. Smart Link geometry/state is preserved where possible; project traversal callbacks must be rebound to Omni."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FOmniModule::ConvertSelectedSmartLinks)));
    }
}

bool FOmniModule::CanConvertSelectedNavLinkProxies() const
{
    if (!GEditor)
    {
        return false;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return false;
    }

    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (HasConvertiblePointLinks(Cast<ANavLinkProxy>(*It)))
        {
            return true;
        }
    }

    return false;
}

bool FOmniModule::CanConvertSelectedSegmentLinks() const
{
    if (!GEditor)
    {
        return false;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return false;
    }

    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (HasConvertibleSegmentLinks(Cast<ANavLinkProxy>(*It)))
        {
            return true;
        }
    }

    return false;
}

bool FOmniModule::CanConvertSelectedSmartLinks() const
{
    if (!GEditor)
    {
        return false;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return false;
    }

    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (HasConvertibleSmartLink(Cast<ANavLinkProxy>(*It)))
        {
            return true;
        }
    }

    return false;
}

void FOmniModule::ConvertSelectedNavLinkProxies()
{
    if (!GEditor)
    {
        return;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return;
    }

    TArray<ANavLinkProxy*> SelectedProxies;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (ANavLinkProxy* Proxy = Cast<ANavLinkProxy>(*It))
        {
            if (HasConvertiblePointLinks(Proxy))
            {
                SelectedProxies.Add(Proxy);
            }
        }
    }

    if (SelectedProxies.IsEmpty())
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("ConvertNavLinkProxiesTransaction", "Convert NavLinkProxy Point Links to Omni"));

    TArray<AOmniPointNavLink*> CreatedOmniActors;
    int32 ConvertedPointLinkCount = 0;
    int32 SkippedSegmentLinkCount = 0;
    int32 SkippedSmartLinkProxyCount = 0;
    int32 UntranslatedSettingWarningCount = 0;

    for (ANavLinkProxy* Proxy : SelectedProxies)
    {
        if (!IsValid(Proxy))
        {
            continue;
        }

        if (!Proxy->GetLevel())
        {
            continue;
        }

        SkippedSegmentLinkCount += Proxy->SegmentLinks.Num();
        if (Proxy->bSmartLinkIsRelevant)
        {
            ++SkippedSmartLinkProxyCount;
        }

        const FString SourceLabel = Proxy->GetActorLabel();

        for (int32 LinkIndex = 0; LinkIndex < Proxy->PointLinks.Num(); ++LinkIndex)
        {
            FNavigationLink NativeLink = Proxy->PointLinks[LinkIndex];
            ReportUntranslatedPointLinkSettings(*Proxy, LinkIndex, NativeLink, UntranslatedSettingWarningCount);

            AOmniPointNavLink* OmniActor = Cast<AOmniPointNavLink>(GEditor->AddActor(
                Proxy->GetLevel(),
                AOmniPointNavLink::StaticClass(),
                Proxy->GetActorTransform(),
                true,
                RF_Transactional,
                false));

            if (!OmniActor || !OmniActor->SourcePoint || !OmniActor->TargetPoint)
            {
                continue;
            }

            OmniActor->Modify();
            OmniActor->SourcePoint->Modify();
            OmniActor->TargetPoint->Modify();

            // FNavigationLink Left/Right are authored in NavLinkProxy local space. Keeping
            // the source actor transform makes these a direct local-space transfer.
            OmniActor->SourcePoint->SetRelativeLocation(NativeLink.Left);
            OmniActor->TargetPoint->SetRelativeLocation(NativeLink.Right);
            OmniActor->LinkDirection = ToOmniDirection(NativeLink.Direction);
            OmniActor->TraversalAction = EOmniTraversalAction::NativeMovement;
            OmniActor->bUseDirectionalTraversalActions = false;
            OmniActor->MaximumLinkDistance = 0.0f;
            OmniActor->SupportedAgents = NativeLink.SupportedAgents;
            OmniActor->AreaClass = NativeLink.GetAreaClass()
                ? NativeLink.GetAreaClass()
                : UNavArea_Default::StaticClass();
            OmniActor->SnapRadius = FMath::Max(1.0f, NativeLink.SnapRadius);
            OmniActor->NativeProjectionHeight = FMath::Max(
                0.0f,
                FMath::Max(NativeLink.LeftProjectHeight, NativeLink.MaxFallDownLength));

            const FString DesiredLabel = Proxy->PointLinks.Num() == 1
                ? FString::Printf(TEXT("%s_Omni"), *SourceLabel)
                : FString::Printf(TEXT("%s_Omni_%d"), *SourceLabel, LinkIndex + 1);
            OmniActor->SetActorLabel(DesiredLabel);
            OmniActor->SetFolderPath(Proxy->GetFolderPath());

            OmniActor->RegenerateLink();
            CreatedOmniActors.Add(OmniActor);
            ++ConvertedPointLinkCount;
        }
    }

    if (!CreatedOmniActors.IsEmpty())
    {
        GEditor->SelectNone(false, true, false);
        for (AOmniPointNavLink* OmniActor : CreatedOmniActors)
        {
            if (IsValid(OmniActor))
            {
                GEditor->SelectActor(OmniActor, true, false, true);
            }
        }
        GEditor->NoteSelectionChange();
    }

    UE_LOG(
        LogOmniMigration,
        Display,
        TEXT("Omni migration: created %d Omni Point actor(s) from selected NavLinkProxy PointLinks. Original NavLinkProxy actors were left unchanged."),
        ConvertedPointLinkCount);

    if (SkippedSegmentLinkCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: skipped %d NavLinkProxy SegmentLink(s) in the Point Link command. Use Omni > Convert NavLinkProxy Segment Links to Omni. The originals were preserved."),
            SkippedSegmentLinkCount);
    }

    if (SkippedSmartLinkProxyCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %d selected NavLinkProxy actor(s) also had Smart Link relevance enabled. This Point Link command does not convert Smart Links; use Omni > Convert NavLinkProxy Smart Links to Omni. The originals were preserved."),
            SkippedSmartLinkProxyCount);
    }

    if (UntranslatedSettingWarningCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %d source PointLink setting(s) could not be represented exactly. See the Omni migration warnings above. Original NavLinkProxy actors were preserved for comparison."),
            UntranslatedSettingWarningCount);
    }
}

void FOmniModule::ConvertSelectedSegmentLinks()
{
    if (!GEditor)
    {
        return;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return;
    }

    TArray<ANavLinkProxy*> SelectedProxies;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (ANavLinkProxy* Proxy = Cast<ANavLinkProxy>(*It))
        {
            if (HasConvertibleSegmentLinks(Proxy))
            {
                SelectedProxies.Add(Proxy);
            }
        }
    }

    if (SelectedProxies.IsEmpty())
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("ConvertNavLinkProxySegmentLinksTransaction", "Convert NavLinkProxy Segment Links to Omni"));

    TArray<AOmniSplineNavLink*> CreatedOmniActors;
    int32 ConvertedSegmentLinkCount = 0;
    int32 DegenerateSegmentLinkCount = 0;
    int32 SamplingCapWarningCount = 0;
    int32 UntranslatedSettingWarningCount = 0;

    for (ANavLinkProxy* Proxy : SelectedProxies)
    {
        if (!IsValid(Proxy) || !Proxy->GetLevel())
        {
            continue;
        }

        const FString SourceLabel = Proxy->GetActorLabel();
        const FTransform ProxyTransform = Proxy->GetActorTransform();

        for (int32 LinkIndex = 0; LinkIndex < Proxy->SegmentLinks.Num(); ++LinkIndex)
        {
            const FNavigationSegmentLink NativeLink = Proxy->SegmentLinks[LinkIndex];
            ReportUntranslatedSegmentLinkSettings(*Proxy, LinkIndex, NativeLink, UntranslatedSettingWarningCount);

            const float SourceLengthWorld = FVector::Distance(
                ProxyTransform.TransformPosition(NativeLink.LeftStart),
                ProxyTransform.TransformPosition(NativeLink.LeftEnd));
            const float TargetLengthWorld = FVector::Distance(
                ProxyTransform.TransformPosition(NativeLink.RightStart),
                ProxyTransform.TransformPosition(NativeLink.RightEnd));

            if (SourceLengthWorld <= KINDA_SMALL_NUMBER || TargetLengthWorld <= KINDA_SMALL_NUMBER)
            {
                ++DegenerateSegmentLinkCount;
                UE_LOG(
                    LogOmniMigration,
                    Warning,
                    TEXT("Omni migration: %s SegmentLinks[%d] has a degenerate Left or Right segment and was not converted. The original NavLinkProxy was preserved."),
                    *SourceLabel,
                    LinkIndex);
                continue;
            }

            AOmniSplineNavLink* OmniActor = Cast<AOmniSplineNavLink>(GEditor->AddActor(
                Proxy->GetLevel(),
                AOmniSplineNavLink::StaticClass(),
                ProxyTransform,
                true,
                RF_Transactional,
                false));

            if (!OmniActor || !OmniActor->SourceSpline || !OmniActor->TargetSpline)
            {
                continue;
            }

            OmniActor->Modify();
            OmniActor->SourceSpline->Modify();
            OmniActor->TargetSpline->Modify();

            // FNavigationSegmentLink points are authored in NavLinkProxy local space.
            // Keep the source actor transform and make both Omni splines root-relative
            // so the four native segment endpoints transfer directly.
            ConfigureLinearSpline(*OmniActor->SourceSpline, NativeLink.LeftStart, NativeLink.LeftEnd);
            ConfigureLinearSpline(*OmniActor->TargetSpline, NativeLink.RightStart, NativeLink.RightEnd);

            OmniActor->LinkDirection = ToOmniSplineDirection(NativeLink.Direction);
            OmniActor->PairingMode = EOmniPairingMode::MatchedGrid;
            OmniActor->ClosedSplineCoverage = EOmniClosedSplineCoverage::Perimeter;
            OmniActor->CoverageDensity = EOmniCoverageDensity::Custom;
            OmniActor->TraversalAction = EOmniTraversalAction::NativeMovement;
            OmniActor->bUseDirectionalTraversalActions = false;
            OmniActor->MaximumLinkDistance = 0.0f;
            OmniActor->SupportedAgents = NativeLink.SupportedAgents;
            OmniActor->AreaClass = NativeLink.GetAreaClass()
                ? NativeLink.GetAreaClass()
                : UNavArea_Default::StaticClass();
            OmniActor->SnapRadius = FMath::Max(1.0f, NativeLink.SnapRadius);
            OmniActor->NativeProjectionHeight = FMath::Max(
                0.0f,
                FMath::Max(NativeLink.LeftProjectHeight, NativeLink.MaxFallDownLength));

            // A native SegmentLink is continuous. Omni Spline represents it with matched
            // native point links. Use the native Snap Radius as the preferred sample gap
            // so neighboring generated-link snap regions overlap in the common case.
            const float PreferredSpacing = FMath::Clamp(FMath::Max(10.0f, NativeLink.SnapRadius), 10.0f, 500.0f);
            OmniActor->LinkSpacing = PreferredSpacing;

            const float LongestSegmentLength = FMath::Max(SourceLengthWorld, TargetLengthWorld);
            const int32 DesiredSamples = FMath::Max(2, FMath::CeilToInt(LongestSegmentLength / PreferredSpacing) + 1);
            OmniActor->MaximumSamplesPerSpline = FMath::Clamp(DesiredSamples, 2, 4096);
            OmniActor->MaximumGeneratedLinks = FMath::Clamp(DesiredSamples, 1, 1024);

            if (DesiredSamples > 1024)
            {
                ++SamplingCapWarningCount;
                UE_LOG(
                    LogOmniMigration,
                    Warning,
                    TEXT("Omni migration: %s SegmentLinks[%d] would require approximately %d matched samples at the migrated %.2f cm spacing. Omni caps generated links at 1024, so the converted Spline is an approximation at that cap. The original NavLinkProxy was preserved."),
                    *SourceLabel,
                    LinkIndex,
                    DesiredSamples,
                    PreferredSpacing);
            }

            const FString DesiredLabel = Proxy->SegmentLinks.Num() == 1
                ? FString::Printf(TEXT("%s_Omni_Segment"), *SourceLabel)
                : FString::Printf(TEXT("%s_Omni_Segment_%d"), *SourceLabel, LinkIndex + 1);
            OmniActor->SetActorLabel(DesiredLabel);
            OmniActor->SetFolderPath(Proxy->GetFolderPath());
            OmniActor->RegenerateLinks();

            CreatedOmniActors.Add(OmniActor);
            ++ConvertedSegmentLinkCount;

            UE_LOG(
                LogOmniMigration,
                Display,
                TEXT("Omni migration: %s SegmentLinks[%d] became an Omni Spline using matched normalized-distance sampling at %.2f cm spacing. This preserves the two authored segments and native link settings where supported, but it intentionally emits sampled native point links rather than one literal FNavigationSegmentLink. The original NavLinkProxy was preserved for validation."),
                *SourceLabel,
                LinkIndex,
                PreferredSpacing);
        }
    }

    if (!CreatedOmniActors.IsEmpty())
    {
        GEditor->SelectNone(false, true, false);
        for (AOmniSplineNavLink* OmniActor : CreatedOmniActors)
        {
            if (IsValid(OmniActor))
            {
                GEditor->SelectActor(OmniActor, true, false, true);
            }
        }
        GEditor->NoteSelectionChange();
    }

    UE_LOG(
        LogOmniMigration,
        Display,
        TEXT("Omni migration: created %d Omni Spline actor(s) from selected NavLinkProxy SegmentLinks. Original NavLinkProxy actors were left unchanged."),
        ConvertedSegmentLinkCount);

    if (DegenerateSegmentLinkCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %d degenerate SegmentLink(s) were not converted. See warnings above."),
            DegenerateSegmentLinkCount);
    }

    if (SamplingCapWarningCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %d SegmentLink conversion(s) reached Omni's 1024 generated-link cap and should be validated against the preserved native proxy."),
            SamplingCapWarningCount);
    }

    if (UntranslatedSettingWarningCount > 0)
    {
        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %d source SegmentLink setting(s) could not be represented exactly. See the Omni migration warnings above. Original NavLinkProxy actors were preserved for comparison."),
            UntranslatedSettingWarningCount);
    }
}

void FOmniModule::ConvertSelectedSmartLinks()
{
    if (!GEditor)
    {
        return;
    }

    USelection* SelectedActors = GEditor->GetSelectedActors();
    if (!SelectedActors)
    {
        return;
    }

    TArray<ANavLinkProxy*> SelectedProxies;
    for (FSelectionIterator It(*SelectedActors); It; ++It)
    {
        if (ANavLinkProxy* Proxy = Cast<ANavLinkProxy>(*It))
        {
            if (HasConvertibleSmartLink(Proxy))
            {
                SelectedProxies.Add(Proxy);
            }
        }
    }

    if (SelectedProxies.IsEmpty())
    {
        return;
    }

    const FScopedTransaction Transaction(LOCTEXT("ConvertNavLinkProxySmartLinksTransaction", "Convert NavLinkProxy Smart Links to Omni"));

    TArray<AOmniPointNavLink*> CreatedOmniActors;
    int32 ConvertedSmartLinkCount = 0;

    for (ANavLinkProxy* Proxy : SelectedProxies)
    {
        if (!IsValid(Proxy) || !Proxy->GetLevel())
        {
            continue;
        }

        UNavLinkCustomComponent* SmartLink = Proxy->GetSmartLinkComp();
        if (!IsValid(SmartLink))
        {
            continue;
        }

        FVector RelativeStart = FVector::ZeroVector;
        FVector RelativeEnd = FVector::ZeroVector;
        ENavLinkDirection::Type NativeDirection = ENavLinkDirection::BothWays;
        SmartLink->GetLinkData(RelativeStart, RelativeEnd, NativeDirection);

        AOmniPointNavLink* OmniActor = Cast<AOmniPointNavLink>(GEditor->AddActor(
            Proxy->GetLevel(),
            AOmniPointNavLink::StaticClass(),
            Proxy->GetActorTransform(),
            true,
            RF_Transactional,
            false));

        if (!OmniActor || !OmniActor->SourcePoint || !OmniActor->TargetPoint)
        {
            continue;
        }

        OmniActor->Modify();
        OmniActor->SourcePoint->Modify();
        OmniActor->TargetPoint->Modify();

        // UNavLinkCustomComponent returns link data relative to its owner. Keep the
        // source proxy transform so this remains a direct local-space transfer.
        OmniActor->SourcePoint->SetRelativeLocation(RelativeStart);
        OmniActor->TargetPoint->SetRelativeLocation(RelativeEnd);
        OmniActor->LinkDirection = ToOmniDirection(NativeDirection);
        OmniActor->TraversalAction = EOmniTraversalAction::Custom;
        OmniActor->bUseDirectionalTraversalActions = false;
        OmniActor->MaximumLinkDistance = 0.0f;
        OmniActor->SupportedAgents = SmartLink->GetSupportedAgents();

        TSubclassOf<UNavArea> EnabledArea = SmartLink->GetEnabledArea();
        if (!EnabledArea)
        {
            EnabledArea = UNavArea_Default::StaticClass();
        }
        OmniActor->AreaClass = EnabledArea;

        TSubclassOf<UNavArea> DisabledArea = SmartLink->GetDisabledArea();
        if (!DisabledArea)
        {
            DisabledArea = UNavArea_Null::StaticClass();
        }
        OmniActor->CustomDisabledAreaClass = DisabledArea;
        OmniActor->bCustomLinkEnabled = Proxy->IsSmartLinkEnabled();

        OmniActor->SetActorLabel(FString::Printf(TEXT("%s_Omni_Smart"), *Proxy->GetActorLabel()));
        OmniActor->SetFolderPath(Proxy->GetFolderPath());
        OmniActor->RegenerateLink();

        CreatedOmniActors.Add(OmniActor);
        ++ConvertedSmartLinkCount;

        UE_LOG(
            LogOmniMigration,
            Warning,
            TEXT("Omni migration: %s Smart Link geometry, direction, Supported Agents, enabled/disabled Nav Areas, and current enabled state were transferred. NavLinkProxy ReceiveSmartLinkReached/Blueprint or C++ traversal behavior cannot be copied automatically; rebind that behavior to the converted actor's CustomTraversal events. Smart-link broadcast/obstacle tuning is not transferred. The original NavLinkProxy was preserved."),
            *Proxy->GetActorLabel());

    }

    if (!CreatedOmniActors.IsEmpty())
    {
        GEditor->SelectNone(false, true, false);
        for (AOmniPointNavLink* OmniActor : CreatedOmniActors)
        {
            if (IsValid(OmniActor))
            {
                GEditor->SelectActor(OmniActor, true, false, true);
            }
        }
        GEditor->NoteSelectionChange();
    }

    UE_LOG(
        LogOmniMigration,
        Display,
        TEXT("Omni migration: created %d Omni Point Custom traversal actor(s) from selected NavLinkProxy Smart Links. Original NavLinkProxy actors were left unchanged."),
        ConvertedSmartLinkCount);
}
#endif

#undef LOCTEXT_NAMESPACE
