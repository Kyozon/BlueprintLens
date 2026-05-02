#include "Modules/ModuleManager.h"

class FBlueprintLensModule final : public IModuleInterface
{
};

IMPLEMENT_MODULE(FBlueprintLensModule, BlueprintLensExporter)
