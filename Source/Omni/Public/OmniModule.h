#pragma once

#include "Modules/ModuleManager.h"

/** Runtime module for Omni navigation authoring and traversal. */
class FOmniModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
