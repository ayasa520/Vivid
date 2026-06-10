#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <gio/gio.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <json-glib/json-glib.h>

#include "SceneWallpaper.hpp"
#include "Utils/Platform.hpp"
#include "WPSceneScriptMedia.hpp"
#include "WPUserProperties.hpp"

namespace vivid::scene
{

inline bool should_log_scene_media_decode(uint64_t count) {
    return count <= 8 || count % 64 == 0;
}

constexpr gint64 SCENE_MEDIA_SLOW_OPERATION_THRESHOLD_US = 20000;
constexpr size_t SCENE_MEDIA_THUMBNAIL_CACHE_LIMIT = 8;

struct SceneProject {
    std::string project_dir;
    std::string scene_path;
    std::string assets_path;
    wallpaper::UserPropertyMap default_user_properties;
    wallpaper::UserPropertyMap user_properties;
    std::unordered_map<std::string, std::string> user_property_types;
};

inline void apply_user_property_overrides(SceneProject& project,
                                          const char* user_properties_json,
                                          const char* log_context);

inline wallpaper::FillMode to_wallpaper_fill_mode(int fill_mode) {
    switch (fill_mode) {
    case 0:
        return wallpaper::FillMode::STRETCH;
    case 1:
        return wallpaper::FillMode::ASPECTFIT;
    case 3:
        /*
         * The bundled scene renderer has no CSS-style "scale down" mode.
         * Mapping it to contain keeps the full authored scene visible instead
         * of accidentally treating value 3 as crop through enum fallthrough.
         */
        return wallpaper::FillMode::ASPECTFIT;
    case 2:
    default:
        return wallpaper::FillMode::ASPECTCROP;
    }
}

inline bool property_type_prefers_string(const char* type) {
    if (!type)
        return false;

    return g_ascii_strcasecmp(type, "text") == 0 ||
        g_ascii_strcasecmp(type, "textinput") == 0 ||
        g_ascii_strcasecmp(type, "combo") == 0 ||
        g_ascii_strcasecmp(type, "file") == 0 ||
        g_ascii_strcasecmp(type, "directory") == 0 ||
        g_ascii_strcasecmp(type, "scenetexture") == 0;
}

inline bool property_prefers_string(JsonObject* property) {
    const char* type = json_object_has_member(property, "type")
        ? json_object_get_string_member(property, "type")
        : "";
    return property_type_prefers_string(type);
}

inline std::string describe_user_property_value_for_log(const wallpaper::UserProperty& property) {
    // The scene switch bug we are chasing depends on whether a combo property
    // arrived as a string or a shader number.  Keep this formatter explicit so
    // logs show the exact representation that the parser will evaluate.
    if (const auto* string_value = std::get_if<std::string>(&property.value))
        return std::string("string:") + *string_value;

    const auto* shader_value = std::get_if<wallpaper::ShaderValue>(&property.value);
    if (!shader_value)
        return "unknown";

    std::string description = "shader:[";
    for (size_t index = 0; index < shader_value->size(); index++) {
        if (index != 0)
            description += ",";
        description += std::to_string((*shader_value)[index]);
        if (index >= 3 && shader_value->size() > 4) {
            description += ",...";
            break;
        }
    }
    description += "]";
    return description;
}

inline std::string describe_user_property_for_log(const wallpaper::UserPropertyMap& properties,
                                                  const char* name) {
    // A missing tracked property is just as important as a wrong value here:
    // missing means the new scene will fall back to its authored default during
    // parse, which is exactly the class of switch-only flicker we need to rule in
    // or out from runtime logs.
    const auto iter = properties.find(name ? name : "");
    if (iter == properties.end())
        return "missing";
    return describe_user_property_value_for_log(iter->second);
}

inline std::string describe_user_property_keys_for_log(const wallpaper::UserPropertyMap& properties) {
    // The renderer bridge should report the actual payload shape it forwards to SceneWallpaper
    // without knowing anything about a specific scene's UI properties or shader uniform names.
    std::string description = "[";
    size_t count = 0;
    for (const auto& [name, _] : properties) {
        if (count != 0)
            description += ",";
        description += name;
        count++;
        if (count >= 12 && properties.size() > count) {
            description += ",...";
            break;
        }
    }
    description += "]";
    return description;
}

inline bool parse_numeric_components_from_string(const char* text, std::vector<float>* out_components) {
    if (!text || !out_components)
        return false;

    out_components->clear();
    g_auto(GStrv) parts = g_strsplit_set(text, " ,", -1);
    for (gchar** part = parts; part && *part; part++) {
        if (**part == '\0')
            continue;

        char* endptr = nullptr;
        double component = g_ascii_strtod(*part, &endptr);
        if (endptr == *part)
            continue;

        out_components->push_back(static_cast<float>(component));
    }

    return !out_components->empty();
}

inline bool parse_numeric_components(JsonNode* value, std::vector<float>* out_components) {
    if (!value || !out_components)
        return false;

    out_components->clear();

    if (JSON_NODE_HOLDS_ARRAY(value)) {
        JsonArray* array = json_node_get_array(value);
        if (!array)
            return false;

        const auto length = json_array_get_length(array);
        out_components->reserve(length);
        for (guint index = 0; index < length; index++) {
            JsonNode* item = json_array_get_element(array, index);
            if (!item || !JSON_NODE_HOLDS_VALUE(item))
                return false;

            const GType item_type = json_node_get_value_type(item);
            if (item_type != G_TYPE_DOUBLE && item_type != G_TYPE_INT64 && item_type != G_TYPE_BOOLEAN)
                return false;

            if (item_type == G_TYPE_BOOLEAN)
                out_components->push_back(json_node_get_boolean(item) ? 1.0f : 0.0f);
            else
                out_components->push_back(static_cast<float>(json_node_get_double(item)));
        }

        return !out_components->empty();
    }

    if (!JSON_NODE_HOLDS_VALUE(value))
        return false;

    const GType value_type = json_node_get_value_type(value);
    if (value_type == G_TYPE_STRING)
        return parse_numeric_components_from_string(json_node_get_string(value), out_components);

    if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_INT64) {
        out_components->push_back(static_cast<float>(json_node_get_double(value)));
        return true;
    }

    if (value_type == G_TYPE_BOOLEAN) {
        out_components->push_back(json_node_get_boolean(value) ? 1.0f : 0.0f);
        return true;
    }

    return false;
}

inline bool parse_user_property_value_node(JsonNode* value,
                                           const char* type,
                                           wallpaper::UserPropertyValue* out_value) {
    if (!value || !out_value)
        return false;

    const bool prefer_string = property_type_prefers_string(type);
    if (JSON_NODE_HOLDS_ARRAY(value)) {
        if (prefer_string)
            return false;

        std::vector<float> components;
        if (!parse_numeric_components(value, &components))
            return false;

        *out_value = wallpaper::ShaderValue(components);
        return true;
    }

    if (!JSON_NODE_HOLDS_VALUE(value))
        return false;

    const GType value_type = json_node_get_value_type(value);
    if (g_ascii_strcasecmp(type ? type : "", "bool") == 0) {
        if (value_type == G_TYPE_BOOLEAN) {
            *out_value = wallpaper::ShaderValue(json_node_get_boolean(value) ? 1.0f : 0.0f);
            return true;
        }

        if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_INT64) {
            *out_value = wallpaper::ShaderValue(std::abs(json_node_get_double(value)) >= 0.0001 ? 1.0f : 0.0f);
            return true;
        }

        if (value_type == G_TYPE_STRING) {
            const char* string_value = json_node_get_string(value);
            const gboolean truthy = string_value &&
                g_ascii_strcasecmp(string_value, "0") != 0 &&
                g_ascii_strcasecmp(string_value, "false") != 0 &&
                *string_value != '\0';
            *out_value = wallpaper::ShaderValue(truthy ? 1.0f : 0.0f);
            return true;
        }

        return false;
    }

    if (value_type == G_TYPE_BOOLEAN) {
        *out_value = wallpaper::ShaderValue(json_node_get_boolean(value) ? 1.0f : 0.0f);
        return true;
    }

    if (value_type == G_TYPE_DOUBLE || value_type == G_TYPE_INT64) {
        if (prefer_string) {
            char buffer[G_ASCII_DTOSTR_BUF_SIZE] = {};
            g_ascii_dtostr(buffer, sizeof(buffer), json_node_get_double(value));
            *out_value = std::string(buffer);
            return true;
        }

        *out_value = wallpaper::ShaderValue(static_cast<float>(json_node_get_double(value)));
        return true;
    }

    if (value_type != G_TYPE_STRING)
        return false;

    const char* string_value = json_node_get_string(value);
    if (!string_value)
        return false;

    if (prefer_string) {
        *out_value = std::string(string_value);
        return true;
    }

    std::vector<float> components;
    if (parse_numeric_components_from_string(string_value, &components)) {
        *out_value = wallpaper::ShaderValue(components);
        return true;
    }

    *out_value = std::string(string_value);
    return true;
}

inline bool parse_property_default(JsonObject* property, wallpaper::UserProperty* out_property) {
    if (!property || !out_property || !json_object_has_member(property, "value"))
        return false;

    JsonNode* value = json_object_get_member(property, "value");
    if (!value)
        return false;

    if (json_object_has_member(property, "condition")) {
        const char* condition = json_object_get_string_member(property, "condition");
        out_property->condition = condition ? condition : "";
    } else {
        out_property->condition.clear();
    }

    const char* type = json_object_has_member(property, "type")
        ? json_object_get_string_member(property, "type")
        : nullptr;
    out_property->is_boolean = g_ascii_strcasecmp(type ? type : "", "bool") == 0;

    return parse_user_property_value_node(value, type, &out_property->value);
}

inline bool read_json_file(const char* path, JsonNode** out_root, const char* log_context) {
    g_autoptr(GError) error = nullptr;
    g_autofree gchar* contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, &error)) {
        g_warning("VividScene(%s): failed to read json file %s: %s",
                  log_context,
                  path,
                  error ? error->message : "(unknown error)");
        return false;
    }

    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, contents, static_cast<gssize>(length), &error)) {
        g_warning("VividScene(%s): failed to parse json file %s: %s",
                  log_context,
                  path,
                  error ? error->message : "(unknown error)");
        return false;
    }

    *out_root = json_node_copy(json_parser_get_root(parser));
    return true;
}

inline std::string resolve_regular_file(const std::string& project_dir, const std::string& relative_path) {
    if (relative_path.empty())
        return {};

    auto path = std::filesystem::path(project_dir) / relative_path;
    if (!std::filesystem::is_regular_file(path))
        return {};

    return path.string();
}

inline std::string get_optional_string_member(JsonObject* object, const char* member_name) {
    if (!object || !member_name || !json_object_has_member(object, member_name))
        return {};

    JsonNode* member = json_object_get_member(object, member_name);
    if (!member || !JSON_NODE_HOLDS_VALUE(member) || json_node_get_value_type(member) != G_TYPE_STRING)
        return {};

    const char* value = json_node_get_string(member);
    return value ? value : "";
}

inline bool entry_file_is_legacy_scene_project(const std::string& entry_file) {
    if (entry_file.empty())
        return false;

    const auto extension = std::filesystem::path(entry_file).extension().string();

    // Some older official Wallpaper Engine scene manifests omit the top-level
    // "type" field, but still point at a scene document. Keep this renderer-side
    // inference as narrow as the JavaScript loader so unsupported executables or
    // unrelated project formats cannot enter the scene renderer by accident.
    return g_ascii_strcasecmp(extension.c_str(), ".json") == 0 ||
        g_ascii_strcasecmp(extension.c_str(), ".pkg") == 0;
}

inline std::string resolve_assets_path(const std::string& project_dir) {
    auto dir = std::filesystem::path(project_dir);
    for (auto current = dir; current.has_parent_path(); current = current.parent_path()) {
        auto assets = current / "steamapps" / "common" / "wallpaper_engine" / "assets";
        if (std::filesystem::is_directory(assets))
            return assets.string();

        if (current == current.root_path())
            break;
    }

    return {};
}

inline bool load_scene_project(const char* project_dir, SceneProject& project, const char* log_context) {
    auto manifest_path = std::filesystem::path(project_dir) / "project.json";
    if (!std::filesystem::is_regular_file(manifest_path)) {
        g_warning("VividScene(%s): project manifest not found: %s", log_context, manifest_path.c_str());
        return false;
    }

    JsonNode* root = nullptr;
    if (!read_json_file(manifest_path.c_str(), &root, log_context))
        return false;

    g_autoptr(JsonNode) root_holder = root;
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("VividScene(%s): project manifest root is not an object: %s",
                  log_context,
                  manifest_path.c_str());
        return false;
    }

    JsonObject* object = json_node_get_object(root);
    const std::string type = get_optional_string_member(object, "type");
    std::string file_member = get_optional_string_member(object, "file");
    if (!type.empty() && g_ascii_strcasecmp(type.c_str(), "scene") != 0) {
        g_warning("VividScene(%s): unsupported project type '%s' in %s",
                  log_context,
                  type.c_str(),
                  manifest_path.c_str());
        return false;
    }

    // Explicit project metadata remains authoritative. Only manifests with a
    // missing type get the legacy scene inference, matching the common official
    // default-project shape used by Techno and Audiophile.
    if (type.empty() && !entry_file_is_legacy_scene_project(file_member)) {
        g_warning("VividScene(%s): project type is missing and entry file '%s' is not a legacy scene entry in %s",
                  log_context,
                  file_member.c_str(),
                  manifest_path.c_str());
        return false;
    }

    std::string scene_path = resolve_regular_file(project_dir, file_member);
    if (scene_path.empty())
        scene_path = resolve_regular_file(project_dir, "scene.pkg");
    if (scene_path.empty()) {
        g_warning("VividScene(%s): failed to resolve scene package under %s", log_context, project_dir);
        return false;
    }

    std::string assets_path = resolve_assets_path(project_dir);
    if (assets_path.empty()) {
        g_warning("VividScene(%s): failed to resolve Wallpaper Engine assets for %s",
                  log_context,
                  project_dir);
        return false;
    }

    project = {};

    if (json_object_has_member(object, "general")) {
        JsonObject* general = json_object_get_object_member(object, "general");
        if (general && json_object_has_member(general, "properties")) {
            JsonObject* properties = json_object_get_object_member(general, "properties");
            if (properties) {
                g_autoptr(GList) members = json_object_get_members(properties);
                for (GList* iter = members; iter; iter = iter->next) {
                    const char* name = static_cast<const char*>(iter->data);
                    JsonObject* property = json_object_get_object_member(properties, name);
                    if (!property)
                        continue;

                    wallpaper::UserProperty parsed_property;
                    if (!parse_property_default(property, &parsed_property))
                        continue;

                    project.default_user_properties[name] = parsed_property;
                    project.user_properties[name] = std::move(parsed_property);
                    project.user_property_types[name] = json_object_has_member(property, "type")
                        ? json_object_get_string_member(property, "type")
                        : "";
                }
            }
        }
    }

    project.project_dir = project_dir;
    project.scene_path = std::move(scene_path);
    project.assets_path = std::move(assets_path);
    return true;
}

inline bool load_scene_project_with_overrides(const char* project_dir,
                                              const char* user_properties_json,
                                              SceneProject& project,
                                              const char* log_context) {
    if (project_dir && *project_dir &&
        !load_scene_project(project_dir, project, log_context)) {
        return false;
    }

    apply_user_property_overrides(project, user_properties_json, log_context);
    return true;
}

inline void apply_user_property_overrides(SceneProject& project,
                                          const char* user_properties_json,
                                          const char* log_context) {
    project.user_properties = project.default_user_properties;

    if (!user_properties_json || *user_properties_json == '\0')
        return;

    g_autoptr(JsonNode) root = json_from_string(user_properties_json, nullptr);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("VividScene(%s): invalid user property override json for %s",
                  log_context,
                  project.project_dir.c_str());
        return;
    }

    JsonObject* overrides = json_node_get_object(root);
    if (!overrides) {
        g_warning("VividScene(%s): override json is not an object for %s",
                  log_context,
                  project.project_dir.c_str());
        return;
    }

    guint applied_count = 0;
    g_autoptr(GList) members = json_object_get_members(overrides);
    for (GList* iter = members; iter; iter = iter->next) {
        const char* name = static_cast<const char*>(iter->data);
        auto property_iter = project.user_properties.find(name);
        if (property_iter == project.user_properties.end())
            continue;

        JsonNode* member = json_object_get_member(overrides, name);
        if (!member)
            continue;

        JsonNode* value = member;
        const char* type = nullptr;
        if (JSON_NODE_HOLDS_OBJECT(member)) {
            JsonObject* member_object = json_node_get_object(member);
            if (!member_object || !json_object_has_member(member_object, "value"))
                continue;
            value = json_object_get_member(member_object, "value");
            type = json_object_has_member(member_object, "type")
                ? json_object_get_string_member(member_object, "type")
                : nullptr;
        }

        if (!type) {
            auto type_iter = project.user_property_types.find(name);
            if (type_iter != project.user_property_types.end())
                type = type_iter->second.c_str();
        }

        wallpaper::UserPropertyValue parsed_value;
        if (parse_user_property_value_node(value, type, &parsed_value)) {
            property_iter->second.value = std::move(parsed_value);
            applied_count++;
        }
    }

    g_message("VividScene(%s): applied user property overrides project=%s applied=%u hrbigb2=%s",
              log_context,
              project.project_dir.c_str(),
              applied_count,
              describe_user_property_for_log(project.user_properties, "hrbigb2").c_str());
}

inline bool ensure_scene_wallpaper(std::unique_ptr<wallpaper::SceneWallpaper>& scene,
                                   const char* cache_dir_name,
                                   const char* log_context,
                                   const SceneProject& project) {
    if (scene || project.scene_path.empty())
        return !project.scene_path.empty();

    g_message("VividScene: creating SceneWallpaper for %s", project.scene_path.c_str());
    scene = std::make_unique<wallpaper::SceneWallpaper>();
    if (!scene->init()) {
        g_warning("VividScene(%s): failed to initialize scene wallpaper", log_context);
        scene.reset();
        return false;
    }

    g_message("VividScene: SceneWallpaper initialized successfully");
    scene->setPropertyString(
        wallpaper::PROPERTY_CACHE_PATH,
        wallpaper::platform::GetCachePath(cache_dir_name).string());
    return true;
}

inline void sync_scene_user_properties(wallpaper::SceneWallpaper& scene,
                                       const SceneProject& project) {
    g_message("VividScene: forwarding live PROPERTY_USER_PROPERTIES project=%s count=%zu keys=%s",
              project.project_dir.c_str(),
              project.user_properties.size(),
              describe_user_property_keys_for_log(project.user_properties).c_str());
    scene.setPropertyObject(
        wallpaper::PROPERTY_USER_PROPERTIES,
        std::make_shared<wallpaper::UserPropertyMap>(project.user_properties));
}

inline void stage_scene_load_user_properties(wallpaper::SceneWallpaper& scene,
                                             const SceneProject& project) {
    // Project reloads are a two-phase operation: first stage the user properties
    // that the next parse must see, then change PROPERTY_SOURCE to perform the
    // parse.  Using a dedicated load-time property prevents those next-project
    // values from being live-applied to the outgoing scene while it is still the
    // transition frame on screen.
    scene.setPropertyObject(
        wallpaper::PROPERTY_LOAD_USER_PROPERTIES,
        std::make_shared<wallpaper::UserPropertyMap>(project.user_properties));
}

inline bool parse_media_color(JsonObject* root,
                              const char* member_name,
                              std::array<float, 3>* out_color) {
    if (!root || !member_name || !out_color || !json_object_has_member(root, member_name))
        return false;

    JsonNode* value = json_object_get_member(root, member_name);
    std::vector<float> components;
    if (!parse_numeric_components(value, &components) || components.size() < 3)
        return false;

    *out_color = { components[0], components[1], components[2] };
    return true;
}

inline GdkPixbuf* load_media_thumbnail_pixbuf(const char* thumbnail_path, const char* log_context) {
    if (!thumbnail_path || *thumbnail_path == '\0')
        return nullptr;

    g_autoptr(GError) error = nullptr;
    g_autoptr(GFile) file = nullptr;
    g_autofree gchar* scheme = g_uri_parse_scheme(thumbnail_path);
    if (scheme)
        file = g_file_new_for_uri(thumbnail_path);
    else
        file = g_file_new_for_path(thumbnail_path);

    g_autoptr(GFileInputStream) stream = g_file_read(file, nullptr, &error);
    if (!stream) {
        g_warning("VividScene(%s): failed to open media thumbnail %s: %s",
                  log_context,
                  thumbnail_path,
                  error ? error->message : "unknown error");
        return nullptr;
    }

    g_clear_error(&error);
    GdkPixbuf* pixbuf =
        gdk_pixbuf_new_from_stream_at_scale(G_INPUT_STREAM(stream), 512, 512, TRUE, nullptr, &error);
    if (!pixbuf) {
        g_warning("VividScene(%s): failed to decode media thumbnail %s: %s",
                  log_context,
                  thumbnail_path,
                  error ? error->message : "unknown error");
        return nullptr;
    }
    return pixbuf;
}

struct DecodedSceneMediaThumbnail {
    int32_t width { 0 };
    int32_t height { 0 };
    std::vector<uint8_t> rgba;
};

inline void copy_rgba_pixels_from_pixbuf(GdkPixbuf* pixbuf,
                                         DecodedSceneMediaThumbnail* decoded_thumbnail) {
    if (!pixbuf || !decoded_thumbnail)
        return;

    g_autoptr(GdkPixbuf) rgba_pixbuf = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
    GdkPixbuf* source = rgba_pixbuf ? rgba_pixbuf : pixbuf;

    const int width = gdk_pixbuf_get_width(source);
    const int height = gdk_pixbuf_get_height(source);
    const int rowstride = gdk_pixbuf_get_rowstride(source);
    const int channels = gdk_pixbuf_get_n_channels(source);
    const guchar* pixels = gdk_pixbuf_get_pixels(source);
    if (!pixels || width <= 0 || height <= 0 || channels < 4)
        return;

    decoded_thumbnail->width = width;
    decoded_thumbnail->height = height;
    decoded_thumbnail->rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; y++) {
        const guchar* src_row = pixels + y * rowstride;
        uint8_t* dst_row = decoded_thumbnail->rgba.data() +
            static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; x++) {
            const guchar* src = src_row + x * channels;
            uint8_t* dst = dst_row + x * 4;
            dst[0] = static_cast<uint8_t>(src[0]);
            dst[1] = static_cast<uint8_t>(src[1]);
            dst[2] = static_cast<uint8_t>(src[2]);
            dst[3] = static_cast<uint8_t>(src[3]);
        }
    }
}

inline void copy_decoded_thumbnail_to_media_state(
    const std::shared_ptr<DecodedSceneMediaThumbnail>& decoded_thumbnail,
    wallpaper::WPSceneScriptMediaState*                media_state) {
    if (!decoded_thumbnail || !media_state)
        return;

    media_state->thumbnail_width = decoded_thumbnail->width;
    media_state->thumbnail_height = decoded_thumbnail->height;
    media_state->thumbnail_rgba = decoded_thumbnail->rgba;
}

inline std::shared_ptr<DecodedSceneMediaThumbnail>
decode_scene_media_thumbnail(const char* thumbnail_path, const char* log_context) {
    g_autoptr(GdkPixbuf) pixbuf = load_media_thumbnail_pixbuf(thumbnail_path, log_context);
    if (!pixbuf)
        return nullptr;

    auto decoded_thumbnail = std::make_shared<DecodedSceneMediaThumbnail>();
    copy_rgba_pixels_from_pixbuf(pixbuf, decoded_thumbnail.get());
    return decoded_thumbnail;
}

inline std::shared_ptr<DecodedSceneMediaThumbnail>
get_cached_scene_media_thumbnail(const char* thumbnail_path, const char* log_context, bool* cache_hit) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, std::shared_ptr<DecodedSceneMediaThumbnail>> cache;
    static std::deque<std::string> cache_order;

    if (cache_hit)
        *cache_hit = false;

    if (!thumbnail_path || *thumbnail_path == '\0')
        return nullptr;

    {
        std::lock_guard<std::mutex> locker(cache_mutex);
        auto it = cache.find(thumbnail_path);
        if (it != cache.end()) {
            if (cache_hit)
                *cache_hit = true;
            return it->second;
        }
    }

    auto decoded_thumbnail = decode_scene_media_thumbnail(thumbnail_path, log_context);
    if (!decoded_thumbnail)
        return nullptr;

    std::lock_guard<std::mutex> locker(cache_mutex);
    auto [it, inserted] = cache.emplace(thumbnail_path, decoded_thumbnail);
    if (!inserted)
        return it->second;

    cache_order.emplace_back(thumbnail_path);
    while (cache_order.size() > SCENE_MEDIA_THUMBNAIL_CACHE_LIMIT) {
        const std::string oldest_path = cache_order.front();
        cache_order.pop_front();
        cache.erase(oldest_path);
    }

    return decoded_thumbnail;
}

inline std::shared_ptr<wallpaper::WPSceneScriptMediaState>
build_scene_media_state_from_json(const char* media_state_json, const char* log_context) {
    static std::atomic<uint64_t> decode_count { 0 };
    const uint64_t current_decode = decode_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const gint64 started_at_us = g_get_monotonic_time();
    auto media_state = std::make_shared<wallpaper::WPSceneScriptMediaState>();
    if (!media_state_json || *media_state_json == '\0')
        return media_state;

    g_autoptr(JsonNode) root = json_from_string(media_state_json, nullptr);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("VividScene(%s): failed to parse media state JSON", log_context);
        return media_state;
    }

    JsonObject* object = json_node_get_object(root);
    if (!object)
        return media_state;

    if (json_object_has_member(object, "title"))
        media_state->title = json_object_get_string_member(object, "title");
    if (json_object_has_member(object, "artist"))
        media_state->artist = json_object_get_string_member(object, "artist");
    if (json_object_has_member(object, "albumTitle"))
        media_state->album_title = json_object_get_string_member(object, "albumTitle");
    if (json_object_has_member(object, "albumArtist"))
        media_state->album_artist = json_object_get_string_member(object, "albumArtist");
    if (json_object_has_member(object, "subTitle"))
        media_state->sub_title = json_object_get_string_member(object, "subTitle");
    if (json_object_has_member(object, "genres"))
        media_state->genres = json_object_get_string_member(object, "genres");
    if (json_object_has_member(object, "contentType"))
        media_state->content_type = json_object_get_string_member(object, "contentType");
    if (json_object_has_member(object, "hasThumbnail"))
        media_state->has_thumbnail = json_object_get_boolean_member(object, "hasThumbnail");
    if (json_object_has_member(object, "playbackState"))
        media_state->playback_state = json_object_get_int_member(object, "playbackState");

    parse_media_color(object, "primaryColor", &media_state->primary_color);
    parse_media_color(object, "secondaryColor", &media_state->secondary_color);
    parse_media_color(object, "tertiaryColor", &media_state->tertiary_color);
    parse_media_color(object, "textColor", &media_state->text_color);
    parse_media_color(object, "highContrastColor", &media_state->high_contrast_color);

    if (!media_state->has_thumbnail)
        return media_state;

    const char* thumbnail_path =
        json_object_has_member(object, "thumbnailPath")
        ? json_object_get_string_member(object, "thumbnailPath")
        : nullptr;
    bool cache_hit = false;
    auto decoded_thumbnail = get_cached_scene_media_thumbnail(thumbnail_path, log_context, &cache_hit);
    if (decoded_thumbnail) {
        copy_decoded_thumbnail_to_media_state(decoded_thumbnail, media_state.get());
        const gint64 elapsed_us = g_get_monotonic_time() - started_at_us;
        if (should_log_scene_media_decode(current_decode) ||
            elapsed_us >= SCENE_MEDIA_SLOW_OPERATION_THRESHOLD_US) {
            g_message(
                "VividScene(%s): media decode #%" G_GUINT64_FORMAT " path=%s title='%s' artist='%s' size=%dx%d rgba-bytes=%zu cache-hit=%s duration=%.2fms",
                log_context,
                current_decode,
                thumbnail_path ? thumbnail_path : "(null)",
                media_state->title.c_str(),
                media_state->artist.c_str(),
                media_state->thumbnail_width,
                media_state->thumbnail_height,
                media_state->thumbnail_rgba.size(),
                cache_hit ? "true" : "false",
                static_cast<double>(elapsed_us) / 1000.0);
        }
    } else if (media_state->has_thumbnail && should_log_scene_media_decode(current_decode)) {
        g_message(
            "VividScene(%s): media decode #%" G_GUINT64_FORMAT " missing pixbuf path=%s title='%s' artist='%s'",
            log_context,
            current_decode,
            thumbnail_path ? thumbnail_path : "(null)",
            media_state->title.c_str(),
            media_state->artist.c_str());
    }

    return media_state;
}

inline void sync_scene_media_state(wallpaper::SceneWallpaper& scene,
                                   const std::shared_ptr<wallpaper::WPSceneScriptMediaState>& media_state) {
    scene.setPropertyObject(wallpaper::PROPERTY_MEDIA_STATE, media_state);
}

inline std::shared_ptr<std::vector<float>>
build_scene_audio_samples_from_variant(GVariant* audio_samples, const char* log_context) {
    auto samples = std::make_shared<std::vector<float>>();
    if (!audio_samples)
        return samples;

    if (!g_variant_is_of_type(audio_samples, G_VARIANT_TYPE("ad"))) {
        g_warning("VividScene(%s): expected audio samples variant of type ad", log_context);
        return samples;
    }

    gsize length = 0;
    const auto* values = static_cast<const gdouble*>(g_variant_get_fixed_array(audio_samples,
                                                                               &length,
                                                                               sizeof(gdouble)));
    if (!values || length == 0)
        return samples;

    samples->reserve(length);
    for (gsize i = 0; i < length; i++)
        samples->push_back(static_cast<float>(values[i]));
    return samples;
}

inline void sync_scene_audio_samples(wallpaper::SceneWallpaper& scene,
                                     const std::shared_ptr<std::vector<float>>& audio_samples) {
    scene.setPropertyObject(wallpaper::PROPERTY_AUDIO_SAMPLES, audio_samples);
}

inline void configure_scene_wallpaper(wallpaper::SceneWallpaper& scene,
                                      const SceneProject& project,
                                      double volume,
                                      gboolean muted,
                                      int fill_mode,
                                      int fps) {
    // Match the KDE backend's long-lived SceneViewer model: update the reusable
    // SceneWallpaper with the next project's state, then let the source property
    // be the single operation that asks the renderer looper to parse a new scene.
    // Keeping assets ahead of source prevents a reload from seeing a new source
    // with stale global Wallpaper Engine assets.
    g_message("VividScene: configuring SceneWallpaper source=%s staged-user-properties=%zu hrbigb2=%s",
              project.scene_path.c_str(),
              project.user_properties.size(),
              describe_user_property_for_log(project.user_properties, "hrbigb2").c_str());
    stage_scene_load_user_properties(scene, project);
    scene.setPropertyFloat(wallpaper::PROPERTY_VOLUME, static_cast<float>(volume));
    scene.setPropertyBool(wallpaper::PROPERTY_MUTED, muted);
    scene.setPropertyInt32(
        wallpaper::PROPERTY_FILLMODE,
        static_cast<int32_t>(to_wallpaper_fill_mode(fill_mode)));
    scene.setPropertyInt32(wallpaper::PROPERTY_FPS, fps);
    scene.setPropertyString(wallpaper::PROPERTY_ASSETS, project.assets_path);
    scene.setPropertyString(wallpaper::PROPERTY_SOURCE, project.scene_path);
}

} // namespace vivid::scene
