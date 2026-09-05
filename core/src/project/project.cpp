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
    if (j.contains("name")) j.at("name").get_to(c.name);
    return c;
}

json track_to_json(const Track& t) {
    json clips = json::array();
    for (const auto& c : t.clips) clips.push_back(clip_to_json(c));
    return json{{"name", t.name}, {"locked", t.locked}, {"clips", clips}};
}

Track track_from_json(const json& j, const Track::Kind kind) {
    Track t;
    t.kind = kind;
    if (j.contains("name")) j.at("name").get_to(t.name);
    if (j.contains("locked")) j.at("locked").get_to(t.locked);
    for (const auto& cj : j.at("clips")) t.clips.push_back(clip_from_json(cj));
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.tl_in < b.tl_in; });
    return t;
}

json tracks_to_json(const std::vector<Track>& tracks) {
    json arr = json::array();
    for (const auto& t : tracks) arr.push_back(track_to_json(t));
    return arr;
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
                                 {"bin", m.bin}});

        const json doc{
            {"canvas_project", kProjectVersion},
            {"name", project.name},
            {"fps", project.sequence.fps},
            {"next_clip_id", project.sequence.next_clip_id},
            {"media", media},
            {"bins", project.bins},
            {"video_tracks", tracks_to_json(project.sequence.video_tracks)},
            {"audio_tracks", tracks_to_json(project.sequence.audio_tracks)}};

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
        CANVAS_LOG("project: SAVED '%s' version=%d clips_with_transitions=%zu (video=%zu audio=%zu)",
               path.c_str(), kProjectVersion,
               count_transitions(project.sequence.video_tracks) +
                   count_transitions(project.sequence.audio_tracks),
               count_transitions(project.sequence.video_tracks),
               count_transitions(project.sequence.audio_tracks));
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

        const auto count_transitions = [](const std::vector<Track>& tracks) {
            std::size_t n = 0;
            for (const auto& t : tracks)
                for (const auto& c : t.clips)
                    if (c.has_transition()) ++n;
            return n;
        };
        CANVAS_LOG("project: LOADED '%s' version=%d clips_with_transitions=%zu (video=%zu audio=%zu)",
               path.c_str(), version,
               count_transitions(p.sequence.video_tracks) +
                   count_transitions(p.sequence.audio_tracks),
               count_transitions(p.sequence.video_tracks),
               count_transitions(p.sequence.audio_tracks));

        out = std::move(p);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}
