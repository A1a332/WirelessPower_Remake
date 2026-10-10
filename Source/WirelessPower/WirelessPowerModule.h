#pragma once

#include "Modules/ModuleManager.h"

/** Standard UE plugin module entry point. The actual content registration happens in
 *  UWirelessPowerGameWorldModule (auto-discovered by SML as a native root module). */
class FWirelessPowerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
