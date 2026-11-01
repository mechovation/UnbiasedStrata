// Included by vision-native.cpp: real CPU reader/projector, no engine semantics.
#include "codec-test-fixtures.hpp"
#include <webp/decode.h>
namespace pr6_reader_codecs {
using namespace pr6_fixtures;
struct Case {
    std::string name;
    Bytes source, rgb;
    int width, height;
    std::string prefix, suffix;
    vision_recipe::Part part;
    Bytes rows;
};
static Bytes decode_reference(const Bytes & source, int width, int height) {
    int x = 0, y = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> rgb(
        stbi_load_from_memory(source.data(), int(source.size()), &x, &y, &channels, 3), stbi_image_free);
    assert(rgb && x == width && y == height);
    return Bytes(rgb.get(), rgb.get() + size_t(x) * y * 3);
}
static void capture(Case & c, mtmd_context * ctx, uint32_t width) {
    std::unique_ptr<mtmd_bitmap, decltype(&mtmd_bitmap_free)> bitmap(
        mtmd_bitmap_init(c.width, c.height, c.rgb.data()), mtmd_bitmap_free);
    assert(bitmap);
    const auto id = vision_reader::source_id(digest(c.source));
    mtmd_bitmap_set_id(bitmap.get(), id.c_str());
    std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)> chunks(mtmd_input_chunks_init(), mtmd_input_chunks_free);
    const auto * marker = mtmd_get_marker(ctx);
    const mtmd_input_text text{marker, std::strlen(marker), false, true};
    const mtmd_bitmap * images[]{bitmap.get()};
    assert(mtmd_tokenize(ctx, chunks.get(), &text, images, 1) == 0);
    const mtmd_input_chunk * image = nullptr;
    for (size_t i = 0; i < mtmd_input_chunks_size(chunks.get()); ++i) {
        const auto * chunk = mtmd_input_chunks_get(chunks.get(), i);
        if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
            assert(!image); image = chunk;
            c.part = vision_recipe::detail::image(chunk, width);
        } else {
            assert(mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_TEXT);
            size_t size = 0; const auto * tokens = mtmd_input_chunk_get_tokens_text(chunk, &size);
            auto & run = image ? c.suffix : c.prefix;
            for (size_t j = 0; j < size; ++j)
                for (unsigned b = 0; b < 4; ++b) run += char(uint32_t(tokens[j]) >> (8*b));
        }
    }
    assert(image && mtmd_encode_chunk(ctx, image) == 0);
    const auto * rows = reinterpret_cast<const uint8_t *>(mtmd_get_output_embd(ctx)); assert(rows);
    c.rows.assign(rows, rows + c.part.token_count * width * sizeof(float));
}
static void run(const char * self, const char * guard, const char * model_path, const char * projector_path,
                llama_model * model, const vision_payload::Config & config, const vision_payload::Limits & caps) {
    std::vector<Case> cases;
    auto add = [&](std::string name, Bytes source, Bytes rgb, int width, int height) {
        assert(rgb.size() == size_t(width) * height * 3);
        cases.push_back({std::move(name),std::move(source),std::move(rgb),width,height,{},{},{},{}});
    };
    // Asymmetric in both axes; all eight orientations must produce distinct rows.
    const int width = 112, height = 56;
    Bytes pixels(width * height * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t at = size_t(y * width + x) * 3;
        pixels[at] = uint8_t(x * 2); pixels[at+1] = uint8_t(y * 4); pixels[at+2] = uint8_t((x + 3*y) % 251);
    }
    const auto baseline_jpeg = jpeg({}, width, height, pixels);
    const auto baseline = decode_reference(baseline_jpeg, width, height);
    for (auto order : {EXIF_BYTE_ORDER_INTEL, EXIF_BYTE_ORDER_MOTOROLA}) for (unsigned value = 1; value <= 8; ++value)
        add("exif-" + std::to_string(order) + "-" + std::to_string(value), jpeg(metadata(value,order), width,height,pixels),
            orient_rgb(baseline,width,height,value), value >= 5 ? height : width, value >= 5 ? width : height);
    add("jpeg-no-exif",baseline_jpeg,baseline,width,height);
    add("jpeg-bad-exif",jpeg(Bytes{'E','x','i','f',0,0,0xff},width,height,pixels),baseline,width,height);
    add("jpeg-invalid-orientation",jpeg(metadata(9,EXIF_BYTE_ORDER_INTEL),width,height,pixels),baseline,width,height);
    const auto still = webp(Bytes{0,0,255,0, 0,0,255,128, 255,0,0,255});
    const auto green = webp(Bytes{0,255,0,255, 0,255,0,255, 0,255,0,255});
    const Bytes alpha_rgb{255,255,255,255,127,127,0,0,255};
    add("webp-alpha",still,alpha_rgb,3,1);
    add("webp-animation",animation(still,green),alpha_rgb,3,1);
    Bytes canvas(7*5*3,255);
    canvas[(2*7+3)*3+1] = canvas[(2*7+3)*3+2] = 127;
    canvas[(2*7+4)*3] = canvas[(2*7+4)*3+1] = 0;
    add("webp-offset-canvas",animation(still,green,7,5,2),canvas,7,5);
    add("webp-second-frame-control",green,Bytes{0,255,0,0,255,0,0,255,0},3,1);
    Bytes argb(size_t(width)*height*4);
    assert(libyuv::RAWToARGB(pixels.data(),width*3,argb.data(),width*4,width,height) == 0);
    add("webp-opaque",webp(argb,width,height),pixels,width,height);
    add("png-transparent-black",transparent_png,Bytes{0,0,0},1,1);

    // Independent RGB controls only: no vision_image::decode in this oracle.
    auto cp = mtmd_context_params_default(); cp.use_gpu = false; cp.warmup = false; cp.n_threads = 4; cp.image_max_tokens = 64;
    auto * ctx = mtmd_init_from_file(projector_path,model,cp); assert(ctx);
    for (auto & c : cases) capture(c,ctx,config.width);
    assert(cases[0].part.grid_x != cases[0].part.grid_y);
    for (size_t i = 0; i < 8; ++i) {
        assert(cases[i].rows == cases[i+8].rows);
        for (size_t j = 0; j < i; ++j) assert(cases[i].rows != cases[j].rows);
        assert(cases[i].part.grid_x == (i >= 4 ? cases[0].part.grid_y : cases[0].part.grid_x));
        assert(cases[i].part.grid_y == (i >= 4 ? cases[0].part.grid_x : cases[0].part.grid_y));
    }
    assert(cases[19].rows == cases[20].rows && cases[19].rows != cases[22].rows);
    Case white{"white-control",{},Bytes{255,255,255},1,1,{},{},{},{}};
    capture(white,ctx,config.width);
    assert(cases.back().rows != white.rows); // PNG alpha must not be flattened white.
    mtmd_free(ctx); // Never keep two projector contexts alive in this test.

    char name[] = "/tmp/pr6-codec-count-XXXXXX"; int fd = ::mkstemp(name); assert(fd >= 0); ::close(fd); counter = name;
    auto sink = std::make_shared<Output>();
    ea::Limits engine_caps{4*1024*1024,4096,16384,64,config.vocabulary_size,10s,1s,1s};
    auto engine = std::make_shared<ea::Adapter>(ea::Launch{guard,self,{self,"engine"},{}},engine_caps,2,sink);
    auto safety = std::make_shared<CpuOnlySafety>(); safety->adapter = engine;
    auto artifacts = std::make_shared<vision_artifact::Budget>(vision_artifact::Usage{64*1024*1024,16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536,4});
    Events events;
    {
        vs::Launch launch{guard,self,{self,"reader",model_path,projector_path,"cpu"},
            {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter},100,config,caps};
        vs::Supervisor supervisor(launch,{180s,1s,1s,5s,50ms,600s,8},2,safety,events.inbox,std::make_shared<vs::Reaper>(),engine);
        unsigned starts = 1; assert(supervisor.start(starts)); events.take(vc::Event::Kind::ready,starts);
        auto submit = [&](uint64_t job, const std::vector<Bytes> & sources, std::shared_ptr<vc::Request> control) {
            auto s = std::make_unique<io::Submission>(); s->job = job; s->control = control; s->artifacts = artifacts; s->recipes = recipes;
            s->limits = {65536,16384,32768}; s->snapshot_bytes = 65536; s->prepare = 180s; s->encode = 180s; s->cleanup = 5s;
            for (size_t i = 0; i < sources.size(); ++i) {
                auto hash = digest(sources[i]); vision_artifact::Artifact::Ptr source;
                assert(vision_artifact::Artifact::source(*artifacts,{0,job,uint32_t(i),hash},
                    {reinterpret_cast<const char *>(sources[i].data()),sources[i].size()},caps.source_bytes,deadline(),never,source) == vision_artifact::Result::ok);
                s->inputs.push_back({source,vision_reader::source_id(hash)});
            }
            return s;
        };
        uint64_t task = 1000;
        for (const auto & c : cases) {
            const io::Identity id{++task,supervisor.instance(),task};
            auto control = std::make_shared<vc::Request>(deadline()); auto submission = submit(id.job,{c.source},control);
            const auto before = encodes();
            assert(supervisor.prepare(id.task,submission)); events.take(vc::Event::Kind::prepared,id.task);
            auto prepared = supervisor.prepared(); assert(prepared && prepared->recipes.size() == 1);
            const auto & r = prepared->recipes[0];
            assert(r.prefix_ids == c.prefix && r.suffix_ids == c.suffix && r.metadata == std::string(c.part.metadata.begin(),c.part.metadata.end()));
            assert(r.shape.x == c.part.grid_x && r.shape.y == c.part.grid_y && r.shape.cells == c.part.token_count && r.shape.positions == uint32_t(c.part.position_count));
            assert(supervisor.admit(id,true)); events.take(vc::Event::Kind::encoded,id.task);
            auto encoded = supervisor.encoded(); assert(encoded && encoded->rows.size() == 1 && encoded->rows[0]->bytes() == c.rows.size());
            Bytes actual(c.rows.size());
            assert(::pread(encoded->rows[0]->fd(),actual.data(),actual.size(),0) == ssize_t(actual.size()));
            assert(actual == c.rows && encodes() == before + 1);
            control->cancel(vc::Reason::disconnect); // cooperative RELEASE, no engine handoff
            events.take(vc::Event::Kind::job_terminal,id.task);
            assert(supervisor.terminal()->outcome == vision_job::Outcome::cancelled && supervisor.acknowledge(id));
            prepared.reset(); encoded.reset();
            assert(supervisor.instance() == 100 && artifacts->usage().bytes == 0 && recipes->usage().bytes == 0);
            std::cout << "PR6_READER_CASE " << c.name << " grid=" << c.part.grid_x << 'x' << c.part.grid_y << " rows=exact release=pass\n" << std::flush;
        }
        struct Bad { const char * name; Bytes source; uint32_t error; bool typed = false; };
        const Bytes truncated_webp(still.begin(),still.end()-1);
        WebPBitstreamFeatures features{};
        assert(WebPGetFeatures(truncated_webp.data(),truncated_webp.size(),&features) == VP8_STATUS_OK);
        const std::vector<Bad> bad{
            {"unreadable",Bytes{1,2,3},1},
            {"jpeg-header-truncated",Bytes{0xff,0xd8,0xff},1},
            {"webp-header-truncated",Bytes(still.begin(),still.begin()+12),0,true},
            {"webp-ambiguous-decoder",truncated_webp,3},
            {"webp-canvas-limit",animation(still,green,4097,1),2},
        };
        for (size_t i = 0; i < bad.size(); ++i) {
            const io::Identity id{++task,supervisor.instance(),task};
            auto control = std::make_shared<vc::Request>(deadline());
            // Failure after one good PREPARE must reclaim the earlier handle too.
            auto submission = submit(id.job,{cases[0].source,bad[i].source},control);
            const auto before = encodes(), decoded = calls('D');
            assert(supervisor.prepare(id.task,submission));
            events.take(bad[i].typed ? vc::Event::Kind::completed : vc::Event::Kind::failed,id.task);
            events.take(vc::Event::Kind::job_terminal,id.task);
            auto terminal = supervisor.terminal();
            assert(terminal && terminal->outcome == (bad[i].typed ? vision_job::Outcome::input_error : vision_job::Outcome::failure));
            assert(terminal->error_class == bad[i].error);
            if (bad[i].typed) {
                assert(terminal->input_failure && terminal->input_failure->occurrence == 1 &&
                    terminal->input_failure->reason == vision_payload::InputError::webp_header);
                assert(supervisor.state() == vs::State::ready && supervisor.instance() == id.instance && safety->leased);
            } else { assert(!terminal->input_failure); await([&] { return supervisor.state() == vs::State::backoff; }); }
            assert(supervisor.acknowledge(id) && encodes() == before && calls('D') == decoded+1);
            assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0 && !safety->revoked);
            assert(bool(safety->leased) == bad[i].typed);
            std::cout << "PR6_READER_REJECT " << bad[i].name << " error_class=" << bad[i].error << " typed=" << bad[i].typed << " earlier_handle=reclaimed retry=none\n" << std::flush;
            if (!bad[i].typed && i+1 < bad.size()) {
                std::this_thread::sleep_for(60ms); ++starts;
                assert(supervisor.start(starts)); events.take(vc::Event::Kind::ready,starts);
            }
        }
        assert(sink->tokens == 0); // No engine or semantic claim.
    }
    events.take(vc::Event::Kind::stopped,0);
    assert(engine->stopped() && !safety->revoked && !safety->leased);
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0);
    ::unlink(counter.c_str()); counter.clear();
    std::cout << "PR6_READER_CODECS_PASS cases=" << cases.size() << " rejects=5 grids=exact metadata=exact rows=exact gpu=none semantics=untested\n";
}
} // namespace pr6_reader_codecs
