#include "diag/OverlayState.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace stm::diag::overlay
{
    namespace
    {
        std::string ToStr(int a_value)
        {
            return std::to_string(a_value);
        }

        const char* Safe(const char* a_str)
        {
            return a_str ? a_str : "";
        }

        RE::BSLightingShaderMaterialBase* AsLightingMaterial(RE::BSShaderMaterial* a_material)
        {
            if (!a_material || a_material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
                return nullptr;
            }
            return static_cast<RE::BSLightingShaderMaterialBase*>(a_material);
        }
    }

    // The object registered under "<Area> [OvlN]" is NOT guaranteed to be a
    // NiNode - depending on the RaceMenu/skee build it can be the geometry
    // itself. An earlier version assumed NiNode and therefore reported every
    // slot as "not-a-node" even though all 24 nodes existed, which made the
    // render side of the probe useless.
    std::string ReadRenderState(RE::NiAVObject* a_root, std::string_view a_area, int a_slot)
    {
        if (!a_root) {
            return "absent";
        }

        const std::string nodeName = std::string(a_area) + " [Ovl" + ToStr(a_slot) + "]";
        auto*             node = a_root->GetObjectByName(RE::BSFixedString(nodeName.c_str()));
        if (!node) {
            return "absent";
        }

        const auto* rtti = node->GetRTTI();
        // NOTE: separators inside a sample must not be '|' - the sample itself
        // is "0=v|1=v|...", and DiffSegments splits on '|'. Using '|' here made
        // every changed line print bare "tex=..." fragments with no slot index.
        std::string result = "type=" + std::string(rtti && rtti->name ? rtti->name : "?");

        // Whatever could hold the overlay geometry.
        std::vector<RE::NiAVObject*> candidates;

        if (auto* asNode = netimmerse_cast<RE::NiNode*>(node)) {
            auto& children = asNode->GetChildren();
            result += ",kids=" + ToStr(static_cast<int>(children.size()));
            candidates.reserve(children.size());
            for (std::uint32_t i = 0; i < children.size(); ++i) {
                if (auto* child = children[i].get()) {
                    candidates.push_back(child);
                }
            }
        } else {
            result += ",self";
            candidates.push_back(node);
        }

        int described = 0;
        for (auto* candidate : candidates) {
            auto* geo = netimmerse_cast<RE::BSGeometry*>(candidate);
            if (!geo) {
                continue;
            }

            ++described;
            auto* shader = geo->lightingShaderProp_cast();
            if (!shader) {
                result += ",geo(no-light-shader)";
                continue;
            }

            result += ",alpha=" + std::to_string(shader->alpha);

            auto* material = shader->GetBaseMaterial();
            if (!material) {
                result += ",mat=none";
                continue;
            }

            auto* lighting = AsLightingMaterial(material);
            if (!lighting) {
                result += ",mat=other";
                continue;
            }

            // The tint is the half that decides whether an overlay is coloured
            // at all, and it is the half no log line shows: the ledger can say
            // 0xFFFF0000 while the material the shader actually samples still
            // holds white. Read the material itself.
            result += ",feat=" + std::to_string(static_cast<int>(lighting->GetFeature()));
            result += ",matA=" + ToStr(static_cast<int>(lighting->materialAlpha * 100.0f));

            // The material holding a tint says nothing about the shader reading
            // it: the shader is picked from the property flags, and bit 21
            // (kFaceGenRGBTint) is what makes the tint part of the pixel shader
            // at all. Without it the material can be red while the render stays
            // the texture's own colour.
            {
                const auto flags = shader->flags.underlying();
                char       buf[64]{};
                std::snprintf(buf, sizeof(buf), ",flg=%llX,tintFlag=%d",
                    static_cast<unsigned long long>(flags), static_cast<int>((flags >> 21) & 1));
                result += buf;
            }

            if (shader->emissiveColor) {
                char buf[80]{};
                std::snprintf(buf, sizeof(buf), ",emit=%.2f/%.2f/%.2f x%.2f",
                    shader->emissiveColor->red, shader->emissiveColor->green, shader->emissiveColor->blue,
                    shader->emissiveMult);
                result += buf;
            }

            if (lighting->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint) {
                // Not a NiObject, so netimmerse_cast does not apply; the feature
                // id is what makes the downcast sound.
                auto* tintMat = static_cast<RE::BSLightingShaderMaterialFacegenTint*>(material);
                char  tint[64]{};
                std::snprintf(tint, sizeof(tint), "%.2f/%.2f/%.2f",
                    tintMat->tintColor.red, tintMat->tintColor.green, tintMat->tintColor.blue);
                result += ",tint=";
                result += tint;
            }

            auto textureSet = lighting->GetTextureSet();
            if (!textureSet) {
                result += ",tex=none";
                continue;
            }

            result += ",tex=" + std::string(Safe(textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)));
        }

        if (described == 0) {
            result += ",no-geometry";
        }

        return result;
    }

    bool LooksLikeBlankSlot(std::string_view a_state)
    {
        if (a_state == "absent") {
            return true;
        }

        std::string lower(a_state);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        return lower.find("overlays\\default.dds") != std::string::npos
            || lower.find("overlays/default.dds") != std::string::npos
            || lower.find("alpha=0.000000") != std::string::npos;
    }

    int RenderCount(RE::NiAVObject* a_root, std::string_view a_area, int a_slots)
    {
        if (!a_root || a_slots <= 0) {
            return 0;
        }

        int count = 0;
        for (int i = 0; i < a_slots; ++i) {
            if (!LooksLikeBlankSlot(ReadRenderState(a_root, a_area, i))) {
                ++count;
            }
        }
        return count;
    }
}
