#include "kd_capi.h"

#include "DestResolver.h"
#include "Engine.h"
#include "VpnMonitor.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#include "third_party/dart/dart_api_dl.h"

// Мост C-API: JSON-строки наружу, события очереди/разбора/VPN — через
// Dart_SendPort (kCString копируется при постинге, так что после
// Dart_PostCObject буфер можно освобождать).

using json = nlohmann::json;

struct kd_engine
{
    std::unique_ptr<Engine> engine;
    std::unique_ptr<VpnMonitor> vpn;
    std::atomic<int64_t> port { 0 };
};

static const char* KD_VERSION = "1.0.0";

static const char* audioFormatName (AudioFormat f)
{
    return f == AudioFormat::m4a ? "m4a" : f == AudioFormat::wav ? "wav"
         : f == AudioFormat::flac ? "flac" : f == AudioFormat::ogg ? "vorbis"
         : "mp3";
}

static char* dupString (const std::string& s)
{
    char* out = (char*) std::malloc (s.size() + 1);
    if (out == nullptr) return nullptr;
    std::memcpy (out, s.c_str(), s.size() + 1);
    return out;
}

static void postToPort (kd_engine* e, const std::string& message)
{
    const int64_t port = e->port.load (std::memory_order_relaxed);
    if (port == 0) return;

    Dart_CObject obj {};
    obj.type = Dart_CObject_kString;
    obj.value.as_string = const_cast<char*> (message.c_str());
    Dart_PostCObject_DL (port, &obj); // строка копируется до возврата
}

intptr_t kd_attach_dart (void* dart_initialize_api_dl_data)
{
    return Dart_InitializeApiDL (dart_initialize_api_dl_data);
}

static Str qualityToString (VideoQuality q)
{
    return q == VideoQuality::q720 ? "720" : q == VideoQuality::q1080 ? "1080"
         : q == VideoQuality::q2160 ? "2160" : "best";
}

static const char* stateName (QueueItem::State s)
{
    switch (s)
    {
        case QueueItem::State::queued:  return "queued";
        case QueueItem::State::working: return "working";
        case QueueItem::State::done:    return "done";
        case QueueItem::State::failed:  return "failed";
    }
    return "queued";
}

static json itemToJson (const QueueItem& i)
{
    json files = json::array();
    for (const auto& f : i.files) files.push_back (f);

    json chain = json::array();
    for (const auto& c : i.cookieChain) chain.push_back (c);

    return {
        { "id", i.id },
        { "link", i.link },
        { "service", (int) i.service },
        { "serviceTitle", Detector::title (i.service) },
        { "state", stateName (i.state) },
        { "progress", i.progress },
        { "stage", i.stage },
        { "title", i.title },
        { "itemIndex", i.itemIndex },
        { "itemTotal", i.itemTotal },
        { "skipped", i.skipped },
        { "batchIndex", i.batchIndex },
        { "batchTotal", i.batchTotal },
        { "files", files },
        { "isAudio", i.isAudio },
        { "maxHeight", i.maxHeight },
        { "audioFormat", audioFormatName (i.audioFormat) },
        { "wholePlaylist", i.wholePlaylist },
        { "sections", i.sections },
        { "dest", kd::pathStr (i.dest) },
        { "isPhoto", i.isPhoto },
        { "cancelled", i.cancelled() },
        { "cookieChain", chain }   // в интерфейс не показывается, только отладка
    };
}

kd_engine* kd_engine_create (const char* tools_dir)
{
    if (tools_dir != nullptr && tools_dir[0] != '\0')
        ::setenv ("K_LOAD_TOOLS", tools_dir, 1);

    auto e = new kd_engine();
    e->engine = std::make_unique<Engine>();
    e->engine->onChange = [e] { postToPort (e, "{\"type\":\"queue_changed\"}"); };
    e->vpn = std::make_unique<VpnMonitor>();
    e->vpn->setOnChange ([e] (VpnMonitor::State state)
    {
        json ev { { "type", "vpn" },
                  { "state", state == VpnMonitor::State::on ? "on" : "off" } };
        postToPort (e, ev.dump());
    });
    return e;
}

void kd_engine_destroy (kd_engine* e)
{
    if (e == nullptr) return;
    e->vpn.reset();
    e->engine.reset();
    delete e;
}

int kd_set_event_port (kd_engine* e, int64_t native_port)
{
    if (e == nullptr) return -1;
    e->port.store (native_port, std::memory_order_relaxed);
    return 0;
}

char* kd_snapshot (kd_engine* e)
{
    if (e == nullptr || e->engine == nullptr) return dupString ("[]");
    json arr = json::array();
    for (const auto& item : e->engine->snapshot())
        arr.push_back (itemToJson (item));
    return dupString (arr.dump());
}

char* kd_split_links (const char* text)
{
    json arr = json::array();
    if (text != nullptr)
        for (const auto& link : Engine::splitLinks (text))
            arr.push_back (link);
    return dupString (arr.dump());
}

static Engine::Options parseOptions (const char* options_json)
{
    Engine::Options o;
    if (options_json == nullptr || options_json[0] == '\0') return o;
    const auto data = json::parse (options_json, nullptr, false);
    if (data.is_discarded() || ! data.is_object()) return o;

    if (data.contains ("dest") && data["dest"].is_string() && ! data["dest"].get<std::string>().empty())
        o.dest = fs::u8path (data["dest"].get<std::string>());
    if (data.contains ("mode") && data["mode"] == "audio") o.mode = MediaMode::audio;
    if (data.contains ("quality") && data["quality"].is_string())
    {
        const auto q = data["quality"].get<std::string>();
        o.quality = q == "720" ? VideoQuality::q720
                  : q == "1080" ? VideoQuality::q1080
                  : q == "2160" ? VideoQuality::q2160 : VideoQuality::best;
    }
    if (data.contains ("audioFormat") && data["audioFormat"].is_string())
    {
        const auto f = data["audioFormat"].get<std::string>();
        o.audioFormat = f == "m4a" ? AudioFormat::m4a
                      : f == "wav" ? AudioFormat::wav
                      : f == "flac" ? AudioFormat::flac : AudioFormat::mp3;
    }
    if (data.contains ("wholePlaylist") && data["wholePlaylist"].is_number_integer())
        o.wholePlaylistOverride = data["wholePlaylist"].get<int>();
    if (data.contains ("nameOverride") && data["nameOverride"].is_string())
        o.nameOverride = data["nameOverride"].get<std::string>();
    if (data.contains ("sections") && data["sections"].is_string())
        o.sections = data["sections"].get<std::string>();
    if (data.contains ("playlistLimit") && data["playlistLimit"].is_number_integer())
        o.playlistLimit = data["playlistLimit"].get<int>();
    return o;
}

static json probeToJson (const Probe& p, const Str& text)
{
    json heights = json::array();
    for (const auto& h : p.heights) heights.push_back (h);

    json out {
        { "type", "probe" },
        { "text", text },
        { "ok", p.ok },
        { "error", p.error },
        { "link", p.link },
        { "resolved", p.resolved },
        { "service", (int) p.service },
        { "serviceTitle", Detector::title (p.service) },
        { "title", p.title },
        { "uploader", p.uploader },
        { "thumbnail", p.thumbnail },
        { "duration", p.duration },
        { "count", p.count },
        { "heights", heights },
        { "isPlaylist", p.isPlaylist },
        { "hasPlaylist", p.hasPlaylist },
        { "isPhoto", p.isPhoto },
        { "isSearch", p.isSearch },
        { "drm", p.drm }
    };
    return out;
}

int kd_enqueue_batch (kd_engine* e, const char* links_json, const char* options_json)
{
    if (e == nullptr || e->engine == nullptr || links_json == nullptr) return 0;
    const auto data = json::parse (links_json, nullptr, false);
    if (data.is_discarded() || ! data.is_array()) return 0;

    StrVec links;
    for (const auto& l : data)
        if (l.is_string()) links.push_back (l.get<Str>());
    if (links.empty()) return 0;

    const auto options = parseOptions (options_json);
    e->engine->enqueueBatch (links, options);
    return (int) links.size();
}

int kd_enqueue_photo (kd_engine* e, const char* link, const char* options_json)
{
    if (e == nullptr || e->engine == nullptr || link == nullptr || link[0] == '\0') return 0;
    e->engine->enqueuePhoto (link, parseOptions (options_json));
    return 1;
}

void kd_cancel (kd_engine* e, int id)            { if (e && e->engine) e->engine->cancel (id); }
void kd_remove (kd_engine* e, int id)            { if (e && e->engine) e->engine->remove (id); }
void kd_clear_finished (kd_engine* e)            { if (e && e->engine) e->engine->clearFinished(); }

char* kd_probe_blocking (kd_engine* e, const char* text)
{
    if (e == nullptr || e->engine == nullptr || text == nullptr)
        return dupString ("{\"ok\":false,\"error\":\"нет ядра\"}");
    const Str t (text);
    return dupString (probeToJson (e->engine->probe (t), t).dump());
}

void kd_probe_async (kd_engine* e, const char* text)
{
    if (e == nullptr || e->engine == nullptr || text == nullptr) return;
    const Str t (text);
    e->engine->probeAsync (t, [e, t] (const Probe& p)
        { postToPort (e, probeToJson (p, t).dump()); });
}

// То же, но текстовый запрос ищется в заданном источнике (youtube/soundcloud).
void kd_probe_async_source (kd_engine* e, const char* text, const char* source)
{
    if (e == nullptr || e->engine == nullptr || text == nullptr) return;
    const Str t (text);
    const Str src (source == nullptr ? "" : source);
    e->engine->probeAsync (t, [e, t] (const Probe& p)
        { postToPort (e, probeToJson (p, t).dump()); }, src);
}

int kd_vpn_state (kd_engine* e)
{
    if (e == nullptr || e->vpn == nullptr) return 0;
    switch (e->vpn->state())
    {
        case VpnMonitor::State::on:  return 1;
        case VpnMonitor::State::off: return 2;
        default:                     return 0;
    }
}

char* kd_default_dest (kd_engine*)
{
    json out {
        { "base", kd::pathStr (DestResolver::baseFolder()) },
        { "folder", kd::pathStr (DestResolver::defaultFolder()) },
        { "subfolder", DestResolver::folderName() }
    };
    return dupString (out.dump());
}

char* kd_tools_status (kd_engine*)
{
    const auto dir = Engine::findToolsDir();
    json out {
        { "found", ! dir.empty() },
        { "dir", kd::pathStr (dir) }
    };
    return dupString (out.dump());
}

void kd_string_free (char* s) { std::free (s); }

const char* kd_version() { return KD_VERSION; }
