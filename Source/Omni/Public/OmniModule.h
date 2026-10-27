#pragma once

#include "Modules/ModuleManager.h"

#if WITH_EDITOR
struct FToolMenuSection;
#endif

class FOmniModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

#if WITH_EDITOR
private:
    void RegisterEditorMenus();
    void BuildNavLinkMigrationMenu(FToolMenuSection& Section);
    bool CanConvertSelectedNavLinkProxies() const;
    bool CanConvertSelectedSegmentLinks() const;
    bool CanConvertSelectedSmartLinks() const;
    void ConvertSelectedNavLinkProxies();
    void ConvertSelectedSegmentLinks();
    void ConvertSelectedSmartLinks();
#endif
};
