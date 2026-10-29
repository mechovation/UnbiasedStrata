#pragma once
#include "acquisition.hpp"
#include "base64.hpp"

namespace strata_acquisition {
// The original body stays intact. Coordinates are occurrence-based, not URL-based:
// duplicate sources retain their distinct roles and positions in parser file order.
struct SourceOccurrence {
    size_t message;
    size_t part;
    std::string role;
};
template<class Json>
std::vector<SourceOccurrence> acquire_images(Json & body, const Policy & policy,
                                           const std::function<bool()> & cancelled) {
    std::vector<SourceOccurrence> ordering;
    if (!body.contains("messages") || !body.at("messages").is_array()) return ordering;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    auto & messages = body.at("messages");
    for (size_t i = 0; i < messages.size(); ++i) {
        auto & message = messages.at(i);
        if (!message.is_object() || !message.contains("content") || !message.at("content").is_array()) continue;
        auto & parts = message.at("content");
        for (size_t j = 0; j < parts.size(); ++j) {
            auto & part = parts.at(j);
            if (!part.is_object() || part.value("type", std::string()) != "image_url") continue;
            ordering.push_back({i, j, message.value("role", std::string())});
            if (!part.contains("image_url") || !part.at("image_url").is_object() ||
                !part.at("image_url").contains("url") || !part.at("image_url").at("url").is_string())
                throw Error(400, "image_url requires a string URL");
            auto & value = part.at("image_url").at("url");
            const auto url = value.template get<std::string>();
            // Preserve existing data/raw-base64 and explicitly configured local-file behavior.
            // Everything that the upstream parser might interpret as HTTP is intercepted.
            if (url.compare(0, 4, "http") != 0 && url.find("://") == std::string::npos) continue;
            if (url.compare(0, 7, "file://") == 0) continue;
            auto bytes = fetch(url, policy, cancelled, deadline);
            std::string encoded = "data:image/octet-stream;base64,";
            encoded.reserve(encoded.size() + ((bytes.size()+2)/3)*4);
            base64::encode(bytes.begin(), bytes.end(), std::back_inserter(encoded));
            value = std::move(encoded); // upstream sees bytes, never a network URL or credentials
        }
    }
    return ordering;
}
} // namespace strata_acquisition
