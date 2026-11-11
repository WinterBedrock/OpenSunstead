#pragma once
#include <string>
#include <vector>

namespace SoundCloud {

struct Track {
    long long id = 0;
    std::string title;
    std::string artist;
    std::string artworkUrl;   // raw artwork_url (caller upgrades -large -> -t500x500)
    std::string streamUrl;    // resolved progressive MP3 (or preview) URL
    long long durationMs = 0;
    bool isPreview = false;   // true when only a 30 s preview stream was found
};

struct Result {
    bool ok = false;
    std::string error;
    Track track;
};

// Search and resolve the first playable hit for `query`.
Result searchFirst(const std::string& clientId, const std::string& query);

// Search and resolve a RANDOM playable hit for `query` (shuffles the
// candidates instead of always taking the first one).
Result searchRandom(const std::string& clientId, const std::string& query);

Result chartsRandom(const std::string& clientId);

// ---------------------------------------------------------------------------
// Recommendations (the "wave"): real personalized picks instead of raw search.
// All of these work with the plain api-v2 client_id — NO OAuth anywhere:
// SoundCloud closed new app registration years ago, so an OAuth flow would be
// dead code for every user. Public profile data is open to the client_id key.
//
//   relatedRandom   -> GET /tracks/{id}/related: SoundCloud's own
//                      recommendation graph. Chaining it after each played
//                      track gives a station that follows the listener's taste.
//   resolveUser     -> GET /resolve?url=<profile>: profile URL or bare
//                      username -> numeric user id (once per session).
//   userLikesRandom -> GET /users/{id}/likes: the user's own public likes —
//                      the true "my wave".
// ---------------------------------------------------------------------------

// Random playable track related to `trackId` (client_id only).
Result relatedRandom(const std::string& clientId, long long trackId);

struct UserInfo {
    bool ok = false;
    std::string error;
    long long id = 0;
    std::string username;
    std::string permalinkUrl;
};

// Resolve a profile URL ("soundcloud.com/<name>", full link) or a bare
// username to the numeric user id + display name.
UserInfo resolveUser(const std::string& clientId, const std::string& urlOrName);

// Random playable track from a user's PUBLIC liked tracks (client_id only).
Result userLikesRandom(const std::string& clientId, long long userId);

bool downloadToFile(const std::string& url, const std::string& path);

// Raw bytes of a (https) URL: artwork JPEGs. Empty vector on failure.
std::vector<unsigned char> downloadBytes(const std::string& url);

// -large.jpg -> -t500x500.jpg artwork upgrade (no-op when not applicable).
std::string upgradeArtwork(const std::string& url);

} // namespace SoundCloud
