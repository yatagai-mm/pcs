#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FPCSModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCS"));
		checkf(Plugin.IsValid(), TEXT("PCS plugin descriptor was not found."));

		// Vertex factory types are registered during module startup. The virtual
		// shader path must therefore be mounted before UE requests their source.
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/PCS"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
	}
};

IMPLEMENT_MODULE(FPCSModule, PCS)
