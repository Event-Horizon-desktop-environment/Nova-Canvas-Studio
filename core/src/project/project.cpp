#include "canvas/core/project/project.hpp"

#include "canvas/core/util/log.hpp"

#include <nlohmann/json.hpp>
#include <fstream>

namespace canvas::core {

namespace {

using json = nlohmann::json;

constexpr int kProjectVersion = 3;

json clip_to_json(const Clip& c) {
    return json{{"id", c.id},
                {"media", c.media},
                {"tl_in", c.tl_in},
                {"tl_out", c.tl_out},
                {"src_in", c.src_in},
                {"src_out", c.src_out},
                {"linked", c.linked_id},
                {"enabled", c.enabled},
                {"transition_out", static_cast<int>(c.transition_out)},
                {"transition_out_duration", c.transition_out_duration},
                {"transition_in", static_cast<int>(c.transition_in)},
                {"transition_in_duration", c.transition_in_duration},
                {"transition_out_curve", c.transition_out_curve_value},
                {"transition_out_ease", c.transition_out_ease},
                {"transition_in_curve", c.transition_in_curve_value},
                {"transition_in_ease", c.transition_in_ease},
                {"transition_out_start_ratio", c.transition_out_start_ratio},
                {"transition_out_end_ratio", c.transition_out_end_ratio},
                {"transition_in_start_ratio", c.transition_in_start_ratio},
                {"transition_in_end_ratio", c.transition_in_end_ratio},
                {"volume_db", c.volume_db},
                {"pan", c.pan},
                {"scale_x", c.scale_x},
                {"scale_y", c.scale_y},
                {"pos_x", c.pos_x},
                {"pos_y", c.pos_y},
                {"rotation_deg", c.rotation_deg},
                {"anchor_dx", c.anchor_dx},
                {"anchor_dy", c.anchor_dy},
                {"flip_h", c.flip_h},
                {"flip_v", c.flip_v},
                {"opacity", c.opacity},
                {"blend_mode", static_cast<int>(c.blend_mode)},
                {"name", c.name}};
}

Clip clip_from_json(const json& j) {
    Clip c;
    j.at("id").get_to(c.id);
    j.at("media").get_to(c.media);
    j.at("tl_in").get_to(c.tl_in);
    j.at("tl_out").get_to(c.tl_out);
    j.at("src_in").get_to(c.src_in);
    j.at("src_out").get_to(c.src_out);
    if (j.contains("linked")) j.at("linked").get_to(c.linked_id);
    if (j.contains("enabled")) j.at("enabled").get_to(c.enabled);
    if (j.contains("transition_out"))
        c.transition_out = static_cast<TransitionType>(j.at("transition_out").get<int>());
    if (j.contains("transition_out_duration"))
        j.at("transition_out_duration").get_to(c.transition_out_duration);
    if (j.contains("transition_in"))
        c.transition_in = static_cast<TransitionType>(j.at("transition_in").get<int>());
    if (j.contains("transition_in_duration"))
        j.at("transition_in_duration").get_to(c.transition_in_duration);
    if (j.contains("transition_out_curve"))
        j.at("transition_out_curve").get_to(c.transition_out_curve_value);
    if (j.contains("transition_out_ease"))
        j.at("transition_out_ease").get_to(c.transition_out_ease);
    if (j.contains("transition_in_curve"))
        j.at("transition_in_curve").get_to(c.transition_in_curve_value);
    if (j.contains("transition_in_ease"))
        j.at("transition_in_ease").get_to(c.transition_in_ease);
    if (j.contains("transition_out_start_ratio"))
        j.at("transition_out_start_ratio").get_to(c.transition_out_start_ratio);
    if (j.contains("transition_out_end_ratio"))
        j.at("transition_out_end_ratio").get_to(c.transition_out_end_ratio);
    if (j.contains("transition_in_start_ratio"))
        j.at("transition_in_start_ratio").get_to(c.transition_in_start_ratio);
    if (j.contains("transition_in_end_ratio"))
        j.at("transition_in_end_ratio").get_to(c.transition_in_end_ratio);
    if (j.contains("volume_db")) j.at("volume_db").get_to(c.volume_db);
    if (j.contains("pan")) j.at("pan").get_to(c.pan);
    if (j.contains("scale_x")) j.at("scale_x").get_to(c.scale_x);
    if (j.contains("scale_y")) j.at("scale_y").get_to(c.scale_y);
    if (j.contains("pos_x")) j.at("pos_x").get_to(c.pos_x);
    if (j.contains("pos_y")) j.at("pos_y").get_to(c.pos_y);
    if (j.contains("rotation_deg")) j.at("rotation_deg").get_to(c.rotation_deg);
    if (j.contains("anchor_dx")) j.at("anchor_dx").get_to(c.anchor_dx);
    if (j.contains("anchor_dy")) j.at("anchor_dy").get_to(c.anchor_dy);
    if (j.contains("flip_h")) j.at("flip_h").get_to(c.flip_h);
    if (j.contains("flip_v")) j.at("flip_v").get_to(c.flip_v);
    if (j.contains("opacity")) j.at("opacity").get_to(c.opacity);
    if (j.contains("blend_mode"))
        c.blend_mode = static_cast<BlendMode>(j.at("blend_mode").get<int>());
    if (j.contains("name")) j.at("name").get_to(c.name);
    return c;
}

json track_to_json(const Track& t) {
    json clips = json::array();
    for (const auto& c : t.clips) clips.push_back(clip_to_json(c));
    return json{{"name", t.name}, {"locked", t.locked}, {"muted", t.muted},
                {"solo", t.solo}, {"gain_db", t.gain_db}, {"clips", clips}};
}

Track track_from_json(const json& j, const Track::Kind kind) {
    Track t;
    t.kind = kind;
    if (j.contains("name")) j.at("name").get_to(t.name);
    if (j.contains("locked")) j.at("locked").get_to(t.locked);
    if (j.contains("muted")) j.at("muted").get_to(t.muted);
    if (j.contains("solo")) j.at("solo").get_to(t.solo);
    if (j.contains("gain_db")) j.at("gain_db").get_to(t.gain_db);
    for (const auto& cj : j.at("clips")) t.clips.push_back(clip_from_json(cj));
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.tl_in < b.tl_in; });
    return t;
}

json tracks_to_json(const std::vector<Track>& tracks) {
    json arr = json::array();
    for (const auto& t : tracks) arr.push_back(track_to_json(t));
    return arr;
}

// --- Deliver / render-queue persistence -----------------------------------
// The deliver settings + render-job snapshots ride along in the same project
// document so a saved project reopens into its full export context.

json deliver_video_to_json(const DeliverVideoSettings& v) {
    return json{{"export_video", v.export_video},
                {"format", v.format},
                {"codec", v.codec},
                {"encoder", static_cast<int>(v.encoder)},
                {"network_optimization", v.network_optimization},
                {"resolution", v.resolution},
                {"custom_width", v.custom_width},
                {"custom_height", v.custom_height},
                {"use_vertical_resolution", v.use_vertical_resolution},
                {"frame_rate", v.frame_rate},
                {"custom_fps", v.custom_fps},
                {"export_alpha", v.export_alpha},
                {"chapters_from_markers", v.chapters_from_markers},
                {"encoding_profile", static_cast<int>(v.encoding_profile)},
                {"key_frames", static_cast<int>(v.key_frames)},
                {"key_frame_interval", v.key_frame_interval},
                {"frame_reordering", v.frame_reordering},
                {"rate_control", static_cast<int>(v.rate_control)},
                {"quality", v.quality},
                {"target_bitrate_kbps", v.target_bitrate_kbps},
                {"max_bitrate_kbps", v.max_bitrate_kbps},
                {"multi_encode", static_cast<int>(v.multi_encode)},
                {"preset", v.preset},
                {"tuning", static_cast<int>(v.tuning)},
                {"two_pass", v.two_pass},
                {"lookahead_frames", v.lookahead_frames},
                {"lookahead_level", v.lookahead_level},
                {"adaptive_i_at_scene_cuts", v.adaptive_i_at_scene_cuts},
                {"adaptive_b_frame", v.adaptive_b_frame},
                {"aq_strength", v.aq_strength},
                {"non_reference_p_frame", v.non_reference_p_frame},
                {"weighted_prediction", v.weighted_prediction},
                {"temporal_filtering", v.temporal_filtering},
                {"unidirectional_b_frames", v.unidirectional_b_frames},
                {"pixel_aspect", static_cast<int>(v.pixel_aspect)},
                {"data_levels", static_cast<int>(v.data_levels)},
                {"retain_sub_black_super_white", v.retain_sub_black_super_white},
                {"color_space_tag", v.color_space_tag},
                {"gamma_tag", v.gamma_tag},
                {"data_burn_in", v.data_burn_in},
                {"bypass_reenecode_when_possible", v.bypass_reenecode_when_possible},
                {"render_all_video_tracks", v.render_all_video_tracks},
                {"force_sizing_high_quality", v.force_sizing_high_quality},
                {"force_debayer_high_quality", v.force_debayer_high_quality},
                {"flat_pass", v.flat_pass},
                {"visionos_bypass", v.visionos_bypass},
                {"disable_sizing_and_blanking", v.disable_sizing_and_blanking}};
}

DeliverVideoSettings deliver_video_from_json(const json& v) {
    DeliverVideoSettings out;
    out.export_video = v.value("export_video", out.export_video);
    out.format = v.value("format", out.format);
    out.codec = v.value("codec", out.codec);
    out.encoder = static_cast<EncoderBackend>(v.value("encoder", static_cast<int>(out.encoder)));
    out.network_optimization = v.value("network_optimization", out.network_optimization);
    out.resolution = v.value("resolution", out.resolution);
    out.custom_width = v.value("custom_width", out.custom_width);
    out.custom_height = v.value("custom_height", out.custom_height);
    out.use_vertical_resolution = v.value("use_vertical_resolution", out.use_vertical_resolution);
    out.frame_rate = v.value("frame_rate", out.frame_rate);
    out.custom_fps = v.value("custom_fps", out.custom_fps);
    out.export_alpha = v.value("export_alpha", out.export_alpha);
    out.chapters_from_markers = v.value("chapters_from_markers", out.chapters_from_markers);
    out.encoding_profile = static_cast<EncodingProfile>(
        v.value("encoding_profile", static_cast<int>(out.encoding_profile)));
    out.key_frames = static_cast<KeyFrameMode>(v.value("key_frames", static_cast<int>(out.key_frames)));
    out.key_frame_interval = v.value("key_frame_interval", out.key_frame_interval);
    out.frame_reordering = v.value("frame_reordering", out.frame_reordering);
    out.rate_control = static_cast<RateControl>(v.value("rate_control", static_cast<int>(out.rate_control)));
    out.quality = v.value("quality", out.quality);
    out.target_bitrate_kbps = v.value("target_bitrate_kbps", out.target_bitrate_kbps);
    out.max_bitrate_kbps = v.value("max_bitrate_kbps", out.max_bitrate_kbps);
    out.multi_encode = static_cast<MultiEncode>(v.value("multi_encode", static_cast<int>(out.multi_encode)));
    out.preset = v.value("preset", out.preset);
    out.tuning = static_cast<EncoderTuning>(v.value("tuning", static_cast<int>(out.tuning)));
    out.two_pass = v.value("two_pass", out.two_pass);
    out.lookahead_frames = v.value("lookahead_frames", out.lookahead_frames);
    out.lookahead_level = v.value("lookahead_level", out.lookahead_level);
    out.adaptive_i_at_scene_cuts = v.value("adaptive_i_at_scene_cuts", out.adaptive_i_at_scene_cuts);
    out.adaptive_b_frame = v.value("adaptive_b_frame", out.adaptive_b_frame);
    out.aq_strength = v.value("aq_strength", out.aq_strength);
    out.non_reference_p_frame = v.value("non_reference_p_frame", out.non_reference_p_frame);
    out.weighted_prediction = v.value("weighted_prediction", out.weighted_prediction);
    out.temporal_filtering = v.value("temporal_filtering", out.temporal_filtering);
    out.unidirectional_b_frames = v.value("unidirectional_b_frames", out.unidirectional_b_frames);
    out.pixel_aspect = static_cast<PixelAspect>(v.value("pixel_aspect", static_cast<int>(out.pixel_aspect)));
    out.data_levels = static_cast<DataLevels>(v.value("data_levels", static_cast<int>(out.data_levels)));
    out.retain_sub_black_super_white = v.value("retain_sub_black_super_white", out.retain_sub_black_super_white);
    out.color_space_tag = v.value("color_space_tag", out.color_space_tag);
    out.gamma_tag = v.value("gamma_tag", out.gamma_tag);
    out.data_burn_in = v.value("data_burn_in", out.data_burn_in);
    out.bypass_reenecode_when_possible = v.value("bypass_reenecode_when_possible", out.bypass_reenecode_when_possible);
    out.render_all_video_tracks = v.value("render_all_video_tracks", out.render_all_video_tracks);
    out.force_sizing_high_quality = v.value("force_sizing_high_quality", out.force_sizing_high_quality);
    out.force_debayer_high_quality = v.value("force_debayer_high_quality", out.force_debayer_high_quality);
    out.flat_pass = v.value("flat_pass", out.flat_pass);
    out.visionos_bypass = v.value("visionos_bypass", out.visionos_bypass);
    out.disable_sizing_and_blanking = v.value("disable_sizing_and_blanking", out.disable_sizing_and_blanking);
    return out;
}

json deliver_to_json(const DeliverSettings& ds) {
    const auto& a = ds.audio;
    const auto& f = ds.file;
    const auto& adv = ds.advanced;
    return json{{"preset_name", ds.preset_name},
                {"render_scope", static_cast<int>(ds.render_scope)},
                {"video", deliver_video_to_json(ds.video)},
                {"audio",
                 {{"export_audio", a.export_audio},
                  {"codec", a.codec},
                  {"bitrate_kbps", a.bitrate_kbps},
                  {"sample_rate", a.sample_rate},
                  {"channels", a.channels},
                  {"render_track_audio", a.render_track_audio},
                  {"normalize_audio", a.normalize_audio},
                  {"normalize_target_lufs", a.normalize_target_lufs}}},
                {"file", {{"file_name", f.file_name}, {"location", f.location}, {"embed_media", f.embed_media}}},
                {"advanced",
                 {{"threads", adv.threads},
                  {"enable_pipewire", adv.enable_pipewire},
                  {"disallow_masking_metadata", adv.disallow_masking_metadata},
                  {"extra_options", adv.extra_options}}}};
}

DeliverSettings deliver_from_json(const json& d) {
    DeliverSettings out;
    out.preset_name = d.value("preset_name", out.preset_name);
    out.render_scope = static_cast<RenderScope>(d.value("render_scope", static_cast<int>(out.render_scope)));
    if (d.contains("video")) out.video = deliver_video_from_json(d.at("video"));
    const json a = d.value("audio", json::object());
    out.audio.export_audio = a.value("export_audio", out.audio.export_audio);
    out.audio.codec = a.value("codec", out.audio.codec);
    out.audio.bitrate_kbps = a.value("bitrate_kbps", out.audio.bitrate_kbps);
    out.audio.sample_rate = a.value("sample_rate", out.audio.sample_rate);
    out.audio.channels = a.value("channels", out.audio.channels);
    out.audio.render_track_audio = a.value("render_track_audio", out.audio.render_track_audio);
    out.audio.normalize_audio = a.value("normalize_audio", out.audio.normalize_audio);
    out.audio.normalize_target_lufs = a.value("normalize_target_lufs", out.audio.normalize_target_lufs);
    const json f = d.value("file", json::object());
    out.file.file_name = f.value("file_name", out.file.file_name);
    out.file.location = f.value("location", out.file.location);
    out.file.embed_media = f.value("embed_media", out.file.embed_media);
    const json adv = d.value("advanced", json::object());
    out.advanced.threads = adv.value("threads", out.advanced.threads);
    out.advanced.enable_pipewire = adv.value("enable_pipewire", out.advanced.enable_pipewire);
    out.advanced.disallow_masking_metadata =
        adv.value("disallow_masking_metadata", out.advanced.disallow_masking_metadata);
    out.advanced.extra_options = adv.value("extra_options", out.advanced.extra_options);
    return out;
}

json job_to_json(const RenderJobSnapshot& j) {
    return json{{"id", j.id},
                {"name", j.name},
                {"output_path", j.output_path},
                {"settings", deliver_to_json(j.settings)},
                {"total_frames", j.total_frames},
                {"status", j.status},
                {"progress", j.progress},
                {"render_fps", j.render_fps},
                {"error", j.error},
                {"elapsed_seconds", j.elapsed_seconds},
                {"frames_rendered", j.frames_rendered},
                {"finished_at", j.finished_at}};
}

RenderJobSnapshot job_from_json(const json& j) {
    RenderJobSnapshot out;
    out.id = j.value("id", out.id);
    out.name = j.value("name", out.name);
    out.output_path = j.value("output_path", out.output_path);
    if (j.contains("settings")) out.settings = deliver_from_json(j.at("settings"));
    out.total_frames = j.value("total_frames", out.total_frames);
    out.status = j.value("status", out.status);
    out.progress = j.value("progress", out.progress);
    out.render_fps = j.value("render_fps", out.render_fps);
    out.error = j.value("error", out.error);
    out.elapsed_seconds = j.value("elapsed_seconds", out.elapsed_seconds);
    out.frames_rendered = j.value("frames_rendered", out.frames_rendered);
    out.finished_at = j.value("finished_at", out.finished_at);
    return out;
}

}  // namespace

const MediaEntry* Project::media_by_id(const MediaId id) const noexcept {
    for (const auto& m : media)
        if (m.id == id) return &m;
    return nullptr;
}

bool save_project(const Project& project, const std::string& path, std::string* error) {
    try {
        json media = json::array();
        for (const auto& m : project.media)
            media.push_back(json{{"id", m.id},
                                 {"path", m.path},
                                 {"fps", m.fps},
                                 {"width", m.width},
                                 {"height", m.height},
                                 {"total_frames", m.total_frames},
                                 {"bin", m.bin},
                                 {"has_audio", m.has_audio}});

        json doc{
            {"canvas_project", kProjectVersion},
            {"name", project.name},
            {"fps", project.sequence.fps},
            {"next_clip_id", project.sequence.next_clip_id},
            {"media", media},
            {"bins", project.bins},
            {"video_tracks", tracks_to_json(project.sequence.video_tracks)},
            {"audio_tracks", tracks_to_json(project.sequence.audio_tracks)}};
        // Deliver settings + render queue ride along with the timeline.
        doc["deliver_settings"] = deliver_to_json(project.deliver_settings);
        json render_jobs = json::array();
        for (const auto& j : project.render_jobs) render_jobs.push_back(job_to_json(j));
        doc["render_jobs"] = std::move(render_jobs);

        std::ofstream out(path);
        if (!out) {
            if (error) *error = "cannot open '" + path + "' for writing";
            return false;
        }
        out << doc.dump(2) << '\n';
        const auto count_transitions = [](const std::vector<Track>& tracks) {
            std::size_t n = 0;
            for (const auto& t : tracks)
                for (const auto& c : t.clips)
                    if (c.has_transition()) ++n;
            return n;
        };
        CANVAS_LOG("project: SAVED '%s' version=%d clips_with_transitions=%zu (video=%zu audio=%zu) render_jobs=%zu",
               path.c_str(), kProjectVersion,
               count_transitions(project.sequence.video_tracks) +
                   count_transitions(project.sequence.audio_tracks),
               count_transitions(project.sequence.video_tracks),
               count_transitions(project.sequence.audio_tracks),
               project.render_jobs.size());
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool load_project(Project& out, const std::string& path, std::string* error) {
    try {
        std::ifstream in(path);
        if (!in) {
            if (error) *error = "cannot open '" + path + "'";
            return false;
        }
        json doc = json::parse(in);
        // Current format key is "canvas_project"; older files predating the
        // Nova Canvas rename serialized the same document under
        // "event_horizon_project", so keep reading that key for backwards
        // compatibility.
        int version = doc.value("canvas_project", 0);
        if (version == 0) version = doc.value("event_horizon_project", 0);
        if (version > kProjectVersion) {
            if (error) *error = "project version " + std::to_string(version) + " is newer than supported";
            return false;
        }

        Project p;
        p.name = doc.value("name", "Untitled Project");
        p.sequence.fps = doc.value("fps", 30.0);
        p.sequence.next_clip_id = doc.value("next_clip_id", ClipId{1});

        uint64_t max_id = 0;
        for (const auto& mj : doc.value("media", json::array())) {
            MediaEntry m;
            mj.at("id").get_to(m.id);
            mj.at("path").get_to(m.path);
            m.fps = mj.value("fps", 0.0);
            mj.at("width").get_to(m.width);
            mj.at("height").get_to(m.height);
            mj.at("total_frames").get_to(m.total_frames);
            if (mj.contains("bin")) mj.at("bin").get_to(m.bin);
            m.has_audio = mj.value("has_audio", false);
            p.media.push_back(std::move(m));
        }

        for (const auto& b : doc.value("bins", json::array())) p.bins.push_back(b.get<std::string>());

        for (const auto& tj : doc.value("video_tracks", json::array())) {
            p.sequence.video_tracks.push_back(track_from_json(tj, Track::Kind::Video));
            for (const auto& c : p.sequence.video_tracks.back().clips) max_id = std::max(max_id, c.id);
        }
        for (const auto& tj : doc.value("audio_tracks", json::array())) {
            p.sequence.audio_tracks.push_back(track_from_json(tj, Track::Kind::Audio));
            for (const auto& c : p.sequence.audio_tracks.back().clips) max_id = std::max(max_id, c.id);
        }
        p.sequence.next_clip_id = std::max(p.sequence.next_clip_id, max_id + 1);

        // Deliver context: settings + saved render queue (tolerant of files
        // saved before either existed).
        if (doc.contains("deliver_settings")) {
            try {
                p.deliver_settings = deliver_from_json(doc.at("deliver_settings"));
            } catch (const std::exception&) {
                // keep the defaults on a malformed block
            }
        }
        for (const auto& jj : doc.value("render_jobs", json::array()))
            p.render_jobs.push_back(job_from_json(jj));

        const auto count_transitions = [](const std::vector<Track>& tracks) {
            std::size_t n = 0;
            for (const auto& t : tracks)
                for (const auto& c : t.clips)
                    if (c.has_transition()) ++n;
            return n;
        };
        CANVAS_LOG("project: LOADED '%s' version=%d clips_with_transitions=%zu (video=%zu audio=%zu) render_jobs=%zu",
               path.c_str(), version,
               count_transitions(p.sequence.video_tracks) +
                   count_transitions(p.sequence.audio_tracks),
               count_transitions(p.sequence.video_tracks),
               count_transitions(p.sequence.audio_tracks),
               p.render_jobs.size());

        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}
