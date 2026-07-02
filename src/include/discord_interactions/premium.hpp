#pragma once

// Entitlements and premium-upgrade prompts for monetized apps.
//
// Discord delivers an app's active entitlements on every interaction in the
// top-level "entitlements" array. Features are gated by SKU ownership, and
// upgrades are prompted with a style-6 "premium" button whose label and target
// Discord renders itself from the SKU — the app supplies only the sku_id.
//
// Note: the PREMIUM_REQUIRED (type 10) interaction callback is DEPRECATED by
// Discord in favor of premium buttons; response.hpp records the constant for
// completeness only. Prefer premium_button().

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace discord_interactions {

using json = nlohmann::json;

// The interaction's entitlements array, or an empty array when absent or malformed.
inline json entitlements(const json& interaction) {
    if (interaction.is_object()) {
        const auto it = interaction.find("entitlements");
        if (it != interaction.end() && it->is_array()) {
            return *it;
        }
    }
    return json::array();
}

// True when the interaction carries an entitlement that matches sku_id, is not
// "deleted": true, and either has an absent/null "ends_at" or an "ends_at" that
// is lexicographically greater than now_iso8601 (ISO-8601 UTC strings compare
// correctly byte-for-byte when zero-padded to the same shape). An empty
// now_iso8601 (the default) skips the expiry check entirely.
inline bool has_entitlement_for_sku(const json& interaction, const std::string& sku_id,
                                    const std::string& now_iso8601 = "") {
    const json ents = entitlements(interaction);
    for (const auto& entitlement : ents) {
        if (!entitlement.is_object()) {
            continue;
        }

        const auto sku_it = entitlement.find("sku_id");
        if (sku_it == entitlement.end() || !sku_it->is_string() ||
            sku_it->get<std::string>() != sku_id) {
            continue;
        }

        const auto deleted_it = entitlement.find("deleted");
        if (deleted_it != entitlement.end() && deleted_it->is_boolean() &&
            deleted_it->get<bool>()) {
            continue;
        }

        if (!now_iso8601.empty()) {
            const auto ends_it = entitlement.find("ends_at");
            if (ends_it != entitlement.end() && ends_it->is_string()) {
                const std::string ends_at = ends_it->get<std::string>();
                if (ends_at <= now_iso8601) {
                    continue;  // expired
                }
            }
        }

        return true;
    }
    return false;
}

// A premium upgrade button (type 2, style 6). Carries only the sku_id — Discord
// renders the label and the purchase flow itself, so no label or custom_id keys
// are emitted.
inline json premium_button(const std::string& sku_id) {
    return json{{"type", 2}, {"style", 6}, {"sku_id", sku_id}};
}

}  // namespace discord_interactions
