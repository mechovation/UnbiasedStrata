// Spike A only: exercise the actual frontend parser/literal/tokenization code without
// changing production visibility. No engine, inference, sockets, CUDA, or row encoding.
#include "../strata_context.cpp"
#include "vision-recipe.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace vr = vision_recipe;
static unsigned encode_calls = 0;
extern "C" int32_t __wrap_mtmd_encode(mtmd_context *, const mtmd_image_tokens *) {
    ++encode_calls; throw std::runtime_error("spike must not encode");
}
extern "C" int32_t __wrap_mtmd_encode_chunk(mtmd_context *, const mtmd_input_chunk *) {
    ++encode_calls; throw std::runtime_error("spike must not encode chunks");
}
extern "C" int32_t __wrap_mtmd_batch_encode(mtmd_batch *) {
    ++encode_calls; throw std::runtime_error("spike must not batch encode");
}
namespace {
void require(bool ok, const std::string & why) { if (!ok) throw std::runtime_error(why); }
struct Hash {
    EVP_MD_CTX * ctx = EVP_MD_CTX_new();
    Hash() { require(ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1, "hash init"); }
    ~Hash() { EVP_MD_CTX_free(ctx); }
    void add(const void * p, size_t n) { require(EVP_DigestUpdate(ctx, p, n) == 1, "hash update"); }
    void add(const std::string & s) { size_t n = s.size(); add(&n, sizeof(n)); add(s.data(), s.size()); }
    std::string finish() {
        unsigned char result[EVP_MAX_MD_SIZE]; unsigned n;
        require(EVP_DigestFinal_ex(ctx, result, &n) == 1, "hash final");
        std::ostringstream out; for (unsigned i = 0; i < n; ++i) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(result[i]);
        return out.str();
    }
};
std::string file_hash(const std::string & path) {
    std::ifstream file(path, std::ios::binary); require(bool(file), "fingerprint file open");
    Hash h; std::vector<char> block(1024*1024);
    while (file.read(block.data(), block.size()) || file.gcount()) h.add(block.data(), file.gcount());
    require(file.eof(), "fingerprint file read"); return h.finish();
}
std::string metadata_hash(const std::string & path) {
    gguf_context * gg=gguf_init_from_file(path.c_str(),{true,nullptr});
    require(gg!=nullptr,"vocabulary metadata fingerprint");
    size_t size=gguf_get_meta_size(gg);
    if(size>256*1024*1024) {gguf_free(gg); throw std::runtime_error("metadata fingerprint limit");}
    std::vector<char> data(size); gguf_get_meta_data(gg,data.data()); gguf_free(gg);
    Hash h; h.add(data.data(),data.size()); return h.finish();
}
raw_buffer bmp(unsigned w, unsigned h, unsigned seed) {
    const unsigned stride = (w*3+3)&~3u, size = 54 + stride*h;
    raw_buffer bytes(size);
    auto u16 = [&](size_t p, unsigned v) { for (int i = 0; i < 2; ++i) bytes[p+i] = v >> (8*i); };
    auto u32 = [&](size_t p, unsigned v) { for (int i = 0; i < 4; ++i) bytes[p+i] = v >> (8*i); };
    bytes[0]='B'; bytes[1]='M'; u32(2,size); u32(10,54); u32(14,40); u32(18,w); u32(22,h); u16(26,1); u16(28,24);
    for (unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) for(unsigned c=0;c<3;++c)
        bytes[54+y*stride+x*3+c] = (x*7+y*3+c*47+seed)%256;
    return bytes;
}
std::string data_url(const raw_buffer & bytes) {
    std::string out="data:image/bmp;base64,";
    base64::encode(bytes.begin(), bytes.end(), std::back_inserter(out)); return out;
}
json image(const raw_buffer & bytes) { return {{"type","image_url"},{"image_url",{{"url",data_url(bytes)}}}}; }
json text(const std::string & s) { return {{"type","text"},{"text",s}}; }
json chat(json parts) {
    return {{"messages",json::array({{{"role","user"},{"content",parts}}})}, {"chat_template_kwargs",{{"enable_thinking",false}}}};
}
struct Case { std::string name; json body; bool rejected = false; };
std::vector<Case> cases() {
    const auto a=bmp(224,224,1), b=bmp(448,112,2), c=bmp(112,448,3), d=bmp(333,197,4), e=bmp(2048,1024,5);
    std::vector<Case> out;
    out.push_back({"text-only",chat(json::array({text("Describe nothing." )}))});
    out.push_back({"image-only",chat(json::array({image(a)}))});
    out.push_back({"leading-image",chat(json::array({image(a),text("Describe." )}))});
    out.push_back({"trailing-image",chat(json::array({text("Describe this:"),image(a)}))});
    out.push_back({"adjacent",chat(json::array({image(a),image(b),image(c)}))});
    out.push_back({"duplicates",chat(json::array({image(a),image(a),image(a)}))});
    out.push_back({"interleaved",chat(json::array({text("one\n"),image(a),text("\ntwo "),image(b),text("three"),image(a),text(" end")}))});
    out.push_back({"odd-aspect",chat(json::array({image(d),image(c)}))});
    out.push_back({"large-resize",chat(json::array({image(e)}))});
    out.push_back({"literals",chat(json::array({text("<|image_pad|> <|vision_start|> <think> <tool_call>"),image(a),
                   text("literal <__media__> <|im_end|>"),image(b),text("<|vision_end|>")}))});
    // The random internal parser marker is not a vocabulary control token. The
    // existing frontend rejects an exact injected spelling; preserve that rejection.
    out.push_back({"internal-marker-collision",chat(json::array({text(get_media_marker()),image(a)})),true});
    out.push_back({"utf8-text-boundaries",chat(json::array({text("é🙂漢字\n"),image(a),text("\n\n العربية é"),image(b),text("!?")}))});
    auto history=chat(json::array({text("Compare."),image(b)}));
    history["messages"]=json::array({
        {{"role","user"},{"content",json::array({text("First:"),image(a)})}},
        {{"role","assistant"},{"content","<think>Consider carefully.</think>Earlier view."}},
        history["messages"][0]});
    out.push_back({"assistant-reasoning-history",history});
    auto tools=chat(json::array({text("Inspect."),image(a)}));
    tools["tools"]=json::array({{{"type","function"},{"function",{{"name","inspect"},{"description","literal <|image_pad|>"},
        {"parameters",{{"type","object"},{"properties",json::object()}}}}}}});
    out.push_back({"tools-definition",tools});
    auto tool_history=tools;
    tool_history["messages"].push_back({{"role","assistant"},{"content",""},{"tool_calls",json::array({
        {{"id","call_1"},{"type","function"},{"function",{{"name","inspect"},{"arguments","{}"}}}}})}});
    tool_history["messages"].push_back({{"role","tool"},{"tool_call_id","call_1"},{"content",json::array({text("Tool image."),image(b)})}});
    tool_history["messages"].push_back({{"role","user"},{"content","Describe both. <|im_start|> is literal."}});
    out.push_back({"tool-image-history",tool_history});
    out.push_back({"long-context",chat(json::array({text(std::string(8192,'x')),image(a),text(std::string(8192,'y'))}))});
    return out;
}
struct Rendered { std::string prompt; std::vector<raw_buffer> files; std::vector<std::pair<size_t,size_t>> plain; };
Rendered render(strata_context_impl & ctx, json body) {
    Rendered r;
    json * tools=body.contains("tools")?&body.at("tools"):nullptr;
    bool marked=ctx.literals.mark_request(body.at("messages"),tools);
    auto parsed=oaicompat_chat_params_parse(body,ctx.chat_params,r.files);
    r.prompt=parsed.at("prompt").get<std::string>();
    if(marked) r.plain=ctx.literals.unmark(r.prompt);
    return r;
}
server_tokens baseline(strata_context_impl & ctx, const Rendered & r) {
    return r.plain.empty()?process_mtmd_prompt(ctx.mctx,r.prompt,r.files,ctx.init_opt):ctx.tokenize_prompt(r.prompt,r.plain,r.files);
}
// Tokenize the single image using the real preprocessing and mtmd tokenizer, with
// add_special=false. ALL wrappers/text IDs come from this call, never guessed.
server_tokens single_image(strata_context_impl & ctx, const raw_buffer & bytes) {
    auto decoded=mtmd_helper_bitmap_init_from_buf(ctx.mctx,bytes.data(),bytes.size(),false,ctx.init_opt);
    require(decoded.bitmap && !decoded.video_ctx,"still image decode");
    mtmd::bitmap bitmap(decoded.bitmap);
    const mtmd_bitmap * ptr=bitmap.ptr.get();
    const std::string marker=get_media_marker();
    mtmd_input_text input{marker.data(),marker.size(),false,true};
    mtmd::input_chunks chunks(mtmd_input_chunks_init());
    require(mtmd_tokenize(ctx.mctx,chunks.ptr.get(),&input,&ptr,1)==0,"single image tokenize");
    return server_tokens(chunks,true);
}
using Cache = std::map<std::string,vr::Recipe>;
std::string source_id(const raw_buffer & file) { Hash h; h.add(file.data(),file.size()); return h.finish(); }
server_tokens compose(strata_context_impl & ctx, const Rendered & r, const Cache & cache, const vr::Generation & generation) {
    server_tokens out(llama_tokens{},true);
    const bool bos=llama_vocab_get_add_bos(ctx.vocab), eos=llama_vocab_get_add_eos(ctx.vocab);
    const std::string marker=get_media_marker(); size_t next=0;
    auto append_text=[&](const std::string & s) { for(auto t:common_tokenize(ctx.vocab,s,false,true)) out.push_back(t); };
    auto segment=[&](const std::string & s, bool add_bos, bool add_eos) {
        if(add_bos) out.push_back(llama_vocab_bos(ctx.vocab));
        size_t pos=0;
        for(size_t at=s.find(marker);at!=std::string::npos;at=s.find(marker,pos)) {
            append_text(s.substr(pos,at-pos));
            require(next<r.files.size(),"composition missing source");
            auto recipe=cache.find(source_id(r.files[next++])); require(recipe!=cache.end(),"composition cache miss");
            auto restored=vr::restore(recipe->second,generation,ctx.vision_width);
            // Do not use the pinned server_tokens concatenation overload: it
            // pushes LLAMA_TOKEN_NULL through push_back(token), which rejects it.
            for(size_t i=0;i<restored.size();) {
                if(restored[i]!=LLAMA_TOKEN_NULL) out.push_back(restored[i++]);
                else {
                    const auto * chunk=restored.find_chunk(i).get();
                    out.push_back(chunk); i+=mtmd_input_chunk_get_n_tokens(chunk);
                }
            }
            pos=at+marker.size();
        }
        append_text(s.substr(pos));
        if(add_eos) out.push_back(llama_vocab_eos(ctx.vocab));
    };
    if(r.plain.empty()) segment(r.prompt,bos,eos);
    else {
        // Match the EXISTING frontend contract exactly, including segment-local EOS
        // when a literal span splits media-containing special segments.
        if(bos) out.push_back(llama_vocab_bos(ctx.vocab));
        auto special=[&](const std::string & s) {
            if(s.empty()) return;
            segment(s,false,eos && s.find(marker)!=std::string::npos);
        };
        size_t pos=0;
        for(const auto & span:r.plain) {
            special(r.prompt.substr(pos,span.first-pos));
            llama_tokens plain; ctx.tokenize_plain(r.prompt.substr(span.first,span.second-span.first),plain);
            for(auto t:plain) out.push_back(t);
            pos=span.second;
        }
        special(r.prompt.substr(pos));
    }
    require(next==r.files.size(),"composition unused source");
    return out;
}
struct Owner {
    strata_context_impl ctx;
    ~Owner() {
        ctx.chat_params.tmpls.reset();
        if(ctx.mctx) mtmd_free(ctx.mctx);
        if(ctx.model) llama_model_free(ctx.model);
    }
};
struct Detached { vr::Recipe recipe; std::vector<char> expected; vr::Generation generation; int width; };
void rejects(const std::function<void()> & fn) {
    bool failed=false; try {fn();} catch(const std::exception &) {failed=true;}
    require(failed,"invalid recipe was accepted");
}
} // namespace

int main(int argc,char ** argv) {
    if(argc!=4) {std::cerr<<"usage: vision-recipe MODEL_GGUF MMPROJ_GGUF PIN\n"; return 2;}
    try {
        // Real-projector fixtures, no downloads. CPU only; do not load an engine or model tensors.
        require(ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU)==nullptr,"requires CUDA-free build");
        gguf_context * gg=gguf_init_from_file(argv[2],{true,nullptr}); require(gg!=nullptr,"projector metadata");
        auto k=gguf_find_key(gg,"clip.projector_type"); require(k>=0,"missing projector family");
        std::string family=gguf_get_val_str(gg,k); gguf_free(gg);
        require(family=="qwen3vl_merger","unsupported projector family (spike qualifies Qwen3-VL still images only)");
        const std::string projector_hash=file_hash(argv[2]);
        // Include ALL tokenizer metadata (scores/merges/config as well as token
        // spellings), without reading the large language-model tensor payload.
        const std::string model_metadata_hash=metadata_hash(argv[1]);
        size_t tested=0, negatives=0, isolated_replays=0; std::vector<Detached> detached;
        // Public upstream fixture, not fabricated serialization: its grid/batch
        // describes a non-Qwen layout and must fail capture before any round trip.
        {
            mtmd::input_chunks chunks(mtmd_test_create_input_chunks());
            server_tokens unsupported(chunks,true);
            rejects([&]{vr::capture(unsupported,{"fixture","fixture","fixture","qwen-still-v1"},1);});
            ++negatives;
            std::cout<<"PASS unsupported-layout-capture\n";
        }
        for(int flags=0;flags<4;++flags) for(int budget:{64,1024}) {
            Owner owner; auto & ctx=owner.ctx;
            llama_model_kv_override overrides[3]{};
            std::snprintf(overrides[0].key,sizeof(overrides[0].key),"tokenizer.ggml.add_bos_token");
            overrides[0].tag=LLAMA_KV_OVERRIDE_TYPE_BOOL; overrides[0].val_bool=flags&1;
            std::snprintf(overrides[1].key,sizeof(overrides[1].key),"tokenizer.ggml.add_eos_token");
            overrides[1].tag=LLAMA_KV_OVERRIDE_TYPE_BOOL; overrides[1].val_bool=flags&2;
            auto mp=llama_model_default_params(); mp.vocab_only=true; mp.kv_overrides=overrides;
            ctx.model=llama_model_load_from_file(argv[1],mp); require(ctx.model!=nullptr,"vocabulary load");
            ctx.vocab=llama_model_get_vocab(ctx.model); ctx.init_literals();
            require(llama_vocab_get_add_bos(ctx.vocab)==bool(flags&1) && llama_vocab_get_add_eos(ctx.vocab)==bool(flags&2),"special override not effective");
            common_params params; params.model.path=argv[1]; params.mmproj.path=argv[2];
            params.mmproj_use_gpu=false; params.cpuparams.n_threads=4; params.image_max_tokens=budget;
            std::string error; require(ctx.init_vision(params,error),"CPU projector init: "+error);
            ctx.chat_params.use_jinja=true; ctx.chat_params.prefill_assistant=false;
            ctx.chat_params.reasoning_format=COMMON_REASONING_FORMAT_AUTO;
            ctx.chat_params.tmpls=common_chat_templates_init(ctx.model,"");
            ctx.chat_params.allow_image=true; ctx.chat_params.allow_audio=false; ctx.chat_params.allow_video=false;
            ctx.chat_params.enable_thinking=false;
            Hash vocab; vocab.add(model_metadata_hash);
            for(int32_t i=0;i<llama_vocab_n_tokens(ctx.vocab);++i) {
                vocab.add(common_token_to_piece(ctx.vocab,i,true));
                auto attr=llama_vocab_get_attr(ctx.vocab,i); vocab.add(&attr,sizeof(attr));
            }
            vocab.add(common_chat_templates_source(ctx.chat_params.tmpls.get())); vocab.add(std::to_string(flags));
            vr::Generation generation{vocab.finish(),projector_hash,std::string(argv[3])+":CPU:still:max="+std::to_string(budget),"qwen-still-v1"};
            Cache cache;
            for(const auto & test:cases()) {
                auto rendered=render(ctx,test.body);
                // Populate from isolated images, then compare entire prompt against the current real path.
                for(const auto & file:rendered.files) {
                    auto id=source_id(file);
                    if(!cache.count(id)) {auto tokens=single_image(ctx,file); cache.emplace(id,vr::capture(tokens,generation,ctx.vision_width));}
                }
                if(test.rejected) {
                    rejects([&]{baseline(ctx,rendered);});
                    rejects([&]{compose(ctx,rendered,cache,generation);});
                    ++negatives;
                    std::cout<<"PASS rejected="<<test.name<<" flags="<<flags<<" budget="<<budget<<'\n';
                    continue;
                }
                auto original=baseline(ctx,rendered);
                auto recipe=vr::capture(original,generation,ctx.vision_width);
                auto rebuilt=vr::restore(recipe,generation,ctx.vision_width);
                vr::compare(original,rebuilt);
                auto composed=compose(ctx,rendered,cache,generation);
                try {vr::compare(original,composed);} catch(const std::exception & e) {
                    throw std::runtime_error(test.name+" flags="+std::to_string(flags)+" budget="+std::to_string(budget)+": "+e.what());
                }
                // Admission must count full cells, not compressed M-RoPE positions.
                for(int room:{-1,0,1,32}) {
                    int64_t context=int64_t(original.size())+ENGINE_CONTEXT_RESERVE+room;
                    const auto composed_room=context-int64_t(composed.size())-ENGINE_CONTEXT_RESERVE;
                    require(composed_room==room && (composed_room>0)==(room>0),"remaining-room/admission predicate mismatch");
                }
                detached.push_back({std::move(recipe),original.serialize(),generation,ctx.vision_width});
                ++tested;
                std::cout<<"PASS case="<<test.name<<" bos="<<bool(flags&1)<<" eos="<<bool(flags&2)<<" budget="<<budget
                         <<" tokens="<<original.size()<<" positions="<<original.pos_next()<<" media="<<rendered.files.size()<<" plain="<<rendered.plain.size()<<'\n';
            }
            require(!cache.empty(),"empty recipe cache");
            for(const auto & entry:cache) {
                auto original=vr::restore(entry.second,generation,ctx.vision_width);
                detached.push_back({entry.second,original.serialize(),generation,ctx.vision_width});
                ++isolated_replays;
            }
            auto valid=cache.begin()->second;
            for(auto field:{0,1,2,3}) {
                auto g=generation;
                if(field==0) g.vocabulary+="changed"; if(field==1) g.projector+="changed";
                if(field==2) g.preprocessing+="changed"; if(field==3) g.family="unsupported";
                rejects([&]{vr::restore(valid,g,ctx.vision_width);}); ++negatives;
            }
            rejects([&]{vr::restore(valid,generation,ctx.vision_width+1);}); ++negatives;
            for(int mutation=0;mutation<5;++mutation) {
                auto bad=valid;
                for(auto & p:bad.parts) if(p.kind==vr::Part::Kind::image) {
                    if(mutation==0) ++p.token_count;
                    if(mutation==1) ++p.position_count;
                    if(mutation==2) ++p.grid_x;
                    if(mutation==3) p.metadata.pop_back();
                    if(mutation==4) ++p.decoder_positions[0].x;
                    break;
                }
                rejects([&]{vr::restore(bad,generation,ctx.vision_width);}); ++negatives;
            }
        } // ALL original models/projector contexts, bitmaps, chunks and prompt buffers destroyed.
        for(const auto & item:detached) {
            auto tokens=vr::restore(item.recipe,item.generation,item.width);
            require(tokens.serialize()==item.expected,"detached ownership replay changed bytes");
            server_tokens appended(llama_tokens{},true);
            for(size_t i=0;i<tokens.size();) {
                if(tokens[i]!=LLAMA_TOKEN_NULL) appended.push_back(tokens[i++]);
                else {
                    const auto * chunk=tokens.find_chunk(i).get();
                    appended.push_back(chunk); i+=mtmd_input_chunk_get_n_tokens(chunk);
                }
            }
            tokens.clear();
            require(appended.serialize()==item.expected,"detached append retained borrowed storage");
        }
        std::cout<<"SPIKE_A_PASS cases="<<tested<<" rejected_invalid="<<negatives<<" detached_replays="<<detached.size()
                 <<" isolated_replays="<<isolated_replays<<" family="<<family<<" gpu=none encode_calls="<<encode_calls<<'\n';
        return 0;
    } catch(const std::exception & e) {std::cerr<<"SPIKE_A_FAIL "<<e.what()<<'\n'; return 1;}
}
