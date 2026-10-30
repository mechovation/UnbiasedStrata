// Owned chat regeneration for typed tool-image omissions. No acquisition/decoder.
#pragma once
#include "server-common.h"
#include "literals.hpp"
#include "base64.hpp"
#include "vendor/hash/hash.h"
#include <memory>

namespace strata_frontend::vision_chat {
inline constexpr const char * omission_note = "[Image omitted: unreadable tool-result image.]";
struct Rendered {
    std::string prompt;
    std::vector<std::pair<size_t,size_t>> plain;
    std::vector<std::string> image_ids;
    json task_params; // freshly rendered parser/delimiter/settings metadata
};
class Chat {
    struct Occurrence { size_t message, part; bool tool; };
    json body_;
    std::vector<Occurrence> occurrences_;
    // Service-owned immutable template configuration; Bridge is joined before it dies.
    const server_chat_params * params_;
    Literals literals_;
    size_t bytes_ = 0;
    bool bound_ = false;
public:
    // Called BEFORE literal marking/template expansion. Strip source URLs from
    // the retained copy; bind() installs only the already-acquired owned bytes.
    Chat(const json & body, const server_chat_params & params, const Literals & literals)
        : body_(body), params_(&params), literals_(literals) {
        auto & messages = body_.at("messages");
        for (size_t m = 0; m < messages.size(); ++m) {
            auto & message = messages.at(m);
            if (!message.contains("content") || !message.at("content").is_array()) continue;
            auto & parts = message.at("content");
            for (size_t p = 0; p < parts.size(); ++p) if (parts.at(p).value("type",std::string()) == "image_url") {
                occurrences_.push_back({m,p,message.value("role",std::string()) == "tool"});
                parts.at(p)["image_url"] = json{{"url",""}};
            }
        }
    }
    size_t count() const { return occurrences_.size(); }
    bool tool(size_t occurrence) const { return bound_ && occurrence < count() && occurrences_[occurrence].tool; }
    size_t bytes() const { return bytes_; }
    void bind(const std::vector<raw_buffer> & files, size_t cap) {
        if (bound_ || files.size() != count()) throw std::invalid_argument("image occurrence map mismatch");
        size_t bytes = body_.dump().size(), raw = 0;
        if (bytes > cap) throw std::invalid_argument("chat regeneration budget exceeded");
        for (const auto & file : files) {
            if (file.empty() || file.size() > 10*1024*1024) throw std::invalid_argument("image source limit exceeded");
            const size_t encoded = 32 + ((file.size()+2)/3)*4;
            if (encoded > cap-bytes) throw std::invalid_argument("chat regeneration budget exceeded");
            bytes += encoded; raw += file.size();
        }
        // Charge retained body + render copy/output workspace + decoded owned
        // parser inputs, before building base64. This does not increase any cap.
        if (raw > cap || bytes > (cap-raw)/4) throw std::invalid_argument("chat regeneration budget exceeded");
        bytes_ = 4*bytes + raw;
        for (size_t i = 0; i < files.size(); ++i) {
            std::string url = "data:image/octet-stream;base64,";
            base64::encode(files[i].begin(),files[i].end(),std::back_inserter(url));
            const auto & o = occurrences_[i];
            body_["messages"][o.message]["content"][o.part]["image_url"]["url"] = std::move(url);
        }
        bound_ = true;
    }
    Rendered render(const std::vector<uint32_t> & omitted) const {
        if (!bound_) throw std::invalid_argument("unbound chat inputs");
        auto body = body_; // fresh template/literal pass, never pad-run surgery
        size_t previous = 0; bool first = true;
        for (auto i : omitted) {
            if (!tool(i) || (!first && i <= previous)) throw std::invalid_argument("invalid tool omission map");
            const auto & o = occurrences_[i];
            body["messages"][o.message]["content"][o.part] = json{{"type","text"},{"text",omission_note}};
            previous = i; first = false;
        }
        auto * tools = body.contains("tools") ? &body.at("tools") : nullptr;
        const bool marked = literals_.mark_request(body.at("messages"),tools);
        std::vector<raw_buffer> files;
        // Every source here is an owned data URI. Never call acquire_images or
        // retain HTTP observers, source URLs, file paths, slots or tasks.
        auto parsed = oaicompat_chat_params_parse(body,*params_,files);
        if (files.size() != count()-omitted.size()) throw std::invalid_argument("regenerated image count mismatch");
        Rendered out; out.prompt = parsed.at("prompt").get<std::string>();
        parsed.erase("prompt"); out.task_params = std::move(parsed);
        if (marked) out.plain = literals_.unmark(out.prompt);
        for (const auto & file : files) out.image_ids.push_back(hash_sha256_hex(file.data(),file.size()));
        return out;
    }
};
} // namespace strata_frontend::vision_chat
