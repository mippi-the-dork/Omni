#pragma once

#include "Modules/ModuleManager.h"

struct FToolMenuSection;

class FOmniEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterEditorMenus();
    void BuildNavLinkMigrationMenu(FToolMenuSection& Section);
    bool CanConvertSelectedNavLinkProxies() const;
    bool CanConvertSelectedSegmentLinks() const;
    bool CanConvertSelectedSmartLinks() const;
    void ConvertSelectedNavLinkProxies();
    void ConvertSelectedSegmentLinks();
    void ConvertSelectedSmartLinks();
};
