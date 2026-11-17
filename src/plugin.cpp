// SynTHBloK-MTX8: registers this plugin's modules.
#include "plugin.hpp"

Plugin* pluginInstance;

extern Model* modelSynTHBloKMatrix8;

void init(Plugin* p) {
    pluginInstance = p;
    p->addModel(modelSynTHBloKMatrix8);
}
