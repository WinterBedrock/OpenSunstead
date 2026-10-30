#include "NoSlow.h"
#include "../ModuleManager.h"

bool NoSlow::ShouldCancel() {
    Module* m = ModuleManager::GetInstance().GetModuleByName("NoSlow");
    return m && m->IsEnabled();
}
