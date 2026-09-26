// MenuUI: ImGui menu (feature list is defined in Features.h).
#pragma once

#include <jni.h>
#include <string>
#include <vector>

namespace MenuUI {

    void InitFont();
    void Draw();

    const std::vector<const char*>& GetFeatures();
    void SetJniEnv(JavaVM* vm, jobject contextGlobal);

    struct Feature {
        int         featNum  = -1;
        std::string type;
        std::string name;
        int         min      = 0;
        int         max      = 0;
        long        maxLong  = 0;
        std::string extra;
        bool        defOn    = false;
        bool        inCollapse = false;
        std::string collapseTitle;
    };

    std::vector<Feature> ParseFeatures();
    bool IsInsideMenu(float x, float y);
}
