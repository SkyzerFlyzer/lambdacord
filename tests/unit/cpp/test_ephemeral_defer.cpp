// T3.2 — manifest-driven ephemeral defer at ingress.
//
// Covers the two pure C++ pieces of the chain:
//   - discord_interactions::route_in_csv_allowlist: the ingress-side parser
//     for the generated DISCORD_EPHEMERAL_DEFER_ROUTES env var (comma-separated
//     command paths, entries whitespace-trimmed, exact full-path match).
//   - discord_interactions::route_from_manifest_entry: accepts both the plain
//     string route form ("discord-cmd-x") and the object route form
//     ({"lambda": "discord-cmd-x", "ephemeral_defer": true}).

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <stdexcept>

#include "discord_interactions/interaction.hpp"
#include "discord_interactions/routing.hpp"

using nlohmann::json;
using discord_interactions::route_in_csv_allowlist;
using discord_interactions::route_from_manifest_entry;

// ---------------------------------------------------------------------------
// route_in_csv_allowlist
// ---------------------------------------------------------------------------

TEST_CASE("route_in_csv_allowlist: empty csv (unset env) matches nothing") {
    CHECK_FALSE(route_in_csv_allowlist("account link", ""));
}

TEST_CASE("route_in_csv_allowlist: single entry exact match") {
    CHECK(route_in_csv_allowlist("account link", "account link"));
}

TEST_CASE("route_in_csv_allowlist: multiple entries, match anywhere") {
    const std::string csv{"example ping,account link,other cmd"};
    CHECK(route_in_csv_allowlist("example ping", csv));
    CHECK(route_in_csv_allowlist("account link", csv));
    CHECK(route_in_csv_allowlist("other cmd", csv));
}

TEST_CASE("route_in_csv_allowlist: entries are whitespace-trimmed") {
    CHECK(route_in_csv_allowlist("account link", "  account link  , other cmd "));
    CHECK(route_in_csv_allowlist("other cmd", "  account link  , other cmd "));
    CHECK(route_in_csv_allowlist("account link", "\taccount link\t"));
}

TEST_CASE("route_in_csv_allowlist: no match") {
    CHECK_FALSE(route_in_csv_allowlist("account unlink", "account link,other cmd"));
}

TEST_CASE("route_in_csv_allowlist: exact full-path match only") {
    // "account" must NOT match the "account link" entry, and vice versa.
    CHECK_FALSE(route_in_csv_allowlist("account", "account link"));
    CHECK_FALSE(route_in_csv_allowlist("account link", "account"));
    CHECK_FALSE(route_in_csv_allowlist("account link extra", "account link"));
    // Interior whitespace is part of the path; only edges are trimmed.
    CHECK_FALSE(route_in_csv_allowlist("account  link", "account link"));
}

TEST_CASE("route_in_csv_allowlist: empty command path never matches") {
    CHECK_FALSE(route_in_csv_allowlist("", "account link"));
    CHECK_FALSE(route_in_csv_allowlist("", ","));
    CHECK_FALSE(route_in_csv_allowlist("", " , "));
}

TEST_CASE("route_in_csv_allowlist: empty and whitespace-only entries are skipped") {
    CHECK(route_in_csv_allowlist("account link", ",account link,"));
    CHECK(route_in_csv_allowlist("account link", " , account link , "));
    CHECK_FALSE(route_in_csv_allowlist("account link", ", ,"));
}

// ---------------------------------------------------------------------------
// route_from_manifest_entry: string + object route forms
// ---------------------------------------------------------------------------

namespace {

json manifest_with_command_route(const json& route_value) {
    return json{
        {"name", "example"},
        {"routes", {{"commands", {{"account link", route_value}}}}},
        {"error_mapper", {{"lambda", "discord-error-mapper-example"}}},
    };
}

}  // namespace

TEST_CASE("route_from_manifest_entry: plain string form still works") {
    const json manifest = manifest_with_command_route("discord-cmd-account-link");
    const auto route = route_from_manifest_entry(manifest, "commands", "account link");
    CHECK(route.module == "example");
    CHECK(route.route == "account link");
    CHECK(route.function_name == "discord-cmd-account-link");
    CHECK(route.error_mapper_function_name == "discord-error-mapper-example");
}

TEST_CASE("route_from_manifest_entry: object form with ephemeral_defer") {
    const json manifest = manifest_with_command_route(
        json{{"lambda", "discord-cmd-account-link"}, {"ephemeral_defer", true}});
    const auto route = route_from_manifest_entry(manifest, "commands", "account link");
    CHECK(route.function_name == "discord-cmd-account-link");
    CHECK(route.route == "account link");
}

TEST_CASE("route_from_manifest_entry: object form without ephemeral_defer") {
    const json manifest = manifest_with_command_route(
        json{{"lambda", "discord-cmd-account-link"}});
    const auto route = route_from_manifest_entry(manifest, "commands", "account link");
    CHECK(route.function_name == "discord-cmd-account-link");
}

TEST_CASE("route_from_manifest_entry: object form missing lambda key throws") {
    const json manifest = manifest_with_command_route(json{{"ephemeral_defer", true}});
    CHECK_THROWS_AS(
        route_from_manifest_entry(manifest, "commands", "account link"),
        std::runtime_error);
}

TEST_CASE("route_from_manifest_entry: object form with non-string lambda throws") {
    const json manifest = manifest_with_command_route(json{{"lambda", 42}});
    CHECK_THROWS_AS(
        route_from_manifest_entry(manifest, "commands", "account link"),
        std::runtime_error);
}

TEST_CASE("route_from_manifest_entry: missing route still throws") {
    const json manifest = manifest_with_command_route("discord-cmd-account-link");
    CHECK_THROWS_AS(
        route_from_manifest_entry(manifest, "commands", "no such route"),
        std::runtime_error);
}
