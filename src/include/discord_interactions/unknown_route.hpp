#pragma once

// Shared user-facing copy for the friendly unknown-route reply (T3.4).
//
// When an interaction targets a route whose worker Lambda does not exist
// (the AWS SDK surfaces LambdaErrors::RESOURCE_NOT_FOUND — see
// lambda_client.hpp's is_function_not_found), the async routers PATCH
// @original with this copy so the user gets a clean message instead of an
// eternal "thinking…" / "application did not respond". It is surfaced verbatim
// to Discord users, so it carries no format placeholders and no internal error
// text (CLAUDE.md error-copy rule). Kept in this tiny, dependency-free header
// (no AWS SDK / curl) so the unit suite can pin it without those toolchains.

namespace discord_interactions {

inline constexpr const char* unknown_route_user_copy =
    "That command isn't available right now.";

}  // namespace discord_interactions
