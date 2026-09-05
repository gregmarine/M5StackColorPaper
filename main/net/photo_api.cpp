/*
 * SPDX-License-Identifier: MIT
 */
#include "net/photo_api.h"

#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cctype>
#include <cstdlib>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "hal/storage/hal_storage.h"
#include "net/web_server.h"

namespace net {
namespace {

constexpr const char* TAG       = "photo_api";
constexpr const char* PHOTO_DIR = "/data";

// A 400x600 4-bit BMP is 118 + 300*600 = 180,118 bytes; 600x400 is the same.
// The cap is generous enough for either plus slack, and small enough that a
// runaway upload cannot fill the 6 MB volume on its own.
constexpr size_t MAX_UPLOAD_BYTES = 512 * 1024;
constexpr size_t MIN_BMP_BYTES    = 118;

// Uploads land here first and are renamed into place only once complete, so a
// dropped Wi-Fi connection leaves no half-written photo in the list.
constexpr const char* TEMP_UPLOAD = "/data/.upload.tmp";

constexpr size_t UPLOAD_CHUNK = 4096;

volatile bool s_refreshing        = false;
volatile bool s_show_pending      = false;
volatile uint16_t s_show_index    = 0;

struct PhotoEntry {
    std::string name;
    size_t bytes;
};

bool isBmpName(const char* name)
{
    const size_t len = std::strlen(name);
    if (len < 5) return false;
    return strcasecmp(name + len - 4, ".bmp") == 0;
}

// Only names that can be written straight into PHOTO_DIR. No separators, no
// traversal, no dotfiles (which is also how AppleDouble "._x" files that
// Finder leaves on the drive stay out of the list).
bool isSafeName(const std::string& name)
{
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.') return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return isBmpName(name.c_str());
}

std::string photoPath(const std::string& name)
{
    return std::string(PHOTO_DIR) + "/" + name;
}

// The firmware shows photos in name order, so this listing is the running
// order too.
std::vector<PhotoEntry> listPhotos()
{
    std::vector<PhotoEntry> out;
    hal_storage_lock();
    DIR* dir = opendir(PHOTO_DIR);
    if (dir != nullptr) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR) continue;
            if (entry->d_name[0] == '.') continue;
            if (!isBmpName(entry->d_name)) continue;
            struct stat st = {};
            size_t bytes   = 0;
            if (stat(photoPath(entry->d_name).c_str(), &st) == 0) bytes = st.st_size;
            out.push_back({entry->d_name, bytes});
        }
        closedir(dir);
    }
    hal_storage_unlock();
    std::sort(out.begin(), out.end(),
              [](const PhotoEntry& a, const PhotoEntry& b) { return a.name < b.name; });
    return out;
}

esp_err_t sendJson(httpd_req_t* req, cJSON* root)
{
    // Any API traffic means someone is still using the app, so hold off the
    // hotspot's idle timeout.
    webTouch();

    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

esp_err_t sendError(httpd_req_t* req, httpd_err_code_t code, const char* message)
{
    ESP_LOGW(TAG, "%s -> %s", req->uri, message);
    return httpd_resp_send_err(req, code, message);
}

// Reads ?name=... (or ?index=...) out of the query string.
bool queryValue(httpd_req_t* req, const char* key, std::string& out)
{
    const size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1) return false;
    std::string query(len, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), len) != ESP_OK) return false;
    char value[96] = {};
    if (httpd_query_key_value(query.c_str(), key, value, sizeof(value)) != ESP_OK) return false;
    std::string decoded;
    // Percent-decoding, because filenames can carry spaces.
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (value[i] == '%' && value[i + 1] && value[i + 2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(value[i + 1]), lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    out = decoded;
    return true;
}

/* ------------------------------- handlers ------------------------------- */

esp_err_t listHandler(httpd_req_t* req)
{
    cJSON* root   = cJSON_CreateObject();
    cJSON* photos = cJSON_AddArrayToObject(root, "photos");
    for (const auto& photo : listPhotos()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", photo.name.c_str());
        cJSON_AddNumberToObject(item, "bytes", static_cast<double>(photo.bytes));
        cJSON_AddItemToArray(photos, item);
    }
    return sendJson(req, root);
}

esp_err_t storageHandler(httpd_req_t* req)
{
    uint64_t total = 0, freeb = 0;
    hal_storage_lock();
    const esp_err_t err = esp_vfs_fat_info(PHOTO_DIR, &total, &freeb);
    hal_storage_unlock();
    if (err != ESP_OK) {
        return sendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not read the filesystem");
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", static_cast<double>(total));
    cJSON_AddNumberToObject(root, "free", static_cast<double>(freeb));
    cJSON_AddNumberToObject(root, "used", static_cast<double>(total - freeb));
    return sendJson(req, root);
}

esp_err_t statusHandler(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "refreshing", s_refreshing);
    cJSON_AddBoolToObject(root, "showQueued", s_show_pending);
    return sendJson(req, root);
}

// Streams a photo back so the web app can decode it and draw a thumbnail.
esp_err_t getPhotoHandler(httpd_req_t* req)
{
    const std::string name = req->uri + std::strlen("/photo/");
    if (!isSafeName(name)) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "bad photo name");
    }

    hal_storage_lock();
    FILE* file = fopen(photoPath(name).c_str(), "rb");
    hal_storage_unlock();
    if (file == nullptr) {
        return sendError(req, HTTPD_404_NOT_FOUND, "no such photo");
    }

    webTouch();
    httpd_resp_set_type(req, "image/bmp");
    // Photos are immutable once written -- a change is a new upload under a
    // new name -- so the phone may keep them.
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");

    std::vector<char> buffer(UPLOAD_CHUNK);
    esp_err_t result = ESP_OK;
    while (true) {
        hal_storage_lock();
        const size_t got = fread(buffer.data(), 1, buffer.size(), file);
        hal_storage_unlock();
        if (got == 0) break;
        if (httpd_resp_send_chunk(req, buffer.data(), got) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    hal_storage_lock();
    fclose(file);
    hal_storage_unlock();
    if (result == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return result;
}

esp_err_t uploadHandler(httpd_req_t* req)
{
    std::string name;
    if (!queryValue(req, "name", name) || !isSafeName(name)) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "bad or missing ?name=");
    }
    if (req->content_len < MIN_BMP_BYTES || req->content_len > MAX_UPLOAD_BYTES) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "unreasonable photo size");
    }

    uint64_t total = 0, freeb = 0;
    hal_storage_lock();
    esp_vfs_fat_info(PHOTO_DIR, &total, &freeb);
    hal_storage_unlock();
    // Leave a little headroom: filling a FAT volume completely makes even
    // deleting things awkward.
    if (freeb < req->content_len + 64 * 1024) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "not enough space on the device");
    }

    hal_storage_lock();
    FILE* file = fopen(TEMP_UPLOAD, "wb");
    hal_storage_unlock();
    if (file == nullptr) {
        return sendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not open the file");
    }

    std::vector<char> buffer(UPLOAD_CHUNK);
    size_t remaining = req->content_len;
    bool header_ok   = false;
    bool failed      = false;

    while (remaining > 0) {
        const int got = httpd_req_recv(req, buffer.data(), std::min(remaining, buffer.size()));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            failed = true;
            break;
        }
        // Check the signature as soon as the first bytes arrive rather than
        // writing the whole thing and rejecting it afterwards.
        if (!header_ok) {
            if (got < 2 || buffer[0] != 'B' || buffer[1] != 'M') {
                failed = true;
                break;
            }
            header_ok = true;
        }
        hal_storage_lock();
        const size_t wrote = fwrite(buffer.data(), 1, got, file);
        hal_storage_unlock();
        if (wrote != static_cast<size_t>(got)) {
            failed = true;
            break;
        }
        remaining -= got;
    }

    hal_storage_lock();
    fclose(file);
    if (failed) {
        unlink(TEMP_UPLOAD);
    } else {
        const std::string dest = photoPath(name);
        unlink(dest.c_str());  // rename() will not overwrite on FatFs
        if (rename(TEMP_UPLOAD, dest.c_str()) != 0) {
            unlink(TEMP_UPLOAD);
            failed = true;
        }
    }
    hal_storage_unlock();

    if (failed) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "upload failed");
    }

    ESP_LOGI(TAG, "stored %s (%u bytes)", name.c_str(), (unsigned)req->content_len);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name.c_str());
    cJSON_AddBoolToObject(root, "ok", true);
    return sendJson(req, root);
}

esp_err_t deleteHandler(httpd_req_t* req)
{
    const std::string name = req->uri + std::strlen("/api/photos/");
    if (!isSafeName(name)) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "bad photo name");
    }
    hal_storage_lock();
    const int result = unlink(photoPath(name).c_str());
    hal_storage_unlock();
    if (result != 0) {
        return sendError(req, HTTPD_404_NOT_FOUND, "no such photo");
    }
    ESP_LOGI(TAG, "deleted %s", name.c_str());
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return sendJson(req, root);
}

esp_err_t showHandler(httpd_req_t* req)
{
    std::string index_text;
    if (!queryValue(req, "index", index_text)) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "missing ?index=");
    }
    const long index = std::strtol(index_text.c_str(), nullptr, 10);
    const auto photos = listPhotos();
    if (index < 0 || index >= static_cast<long>(photos.size())) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }

    // Recorded, not done: a colour refresh takes 15-30 s and the phone would
    // give up long before. The hotspot loop performs it.
    s_show_index   = static_cast<uint16_t>(index);
    s_show_pending = true;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "queued", true);
    cJSON_AddStringToObject(root, "name", photos[index].name.c_str());
    return sendJson(req, root);
}

// The frame draws photos in filename order, so ordering is renaming. Names get
// a NNN_ prefix; any prefix already there is stripped first so repeated
// reorders do not stack up "001_002_003_photo.bmp".
std::string stripOrderPrefix(const std::string& name)
{
    if (name.size() > 4 && isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) &&
        isdigit((unsigned char)name[2]) && name[3] == '_') {
        return name.substr(4);
    }
    return name;
}

std::string reorderTempName(size_t i)
{
    return ".reorder" + std::to_string(i) + ".tmp";
}

esp_err_t reorderHandler(httpd_req_t* req)
{
    if (req->content_len == 0 || req->content_len > 16 * 1024) {
        return sendError(req, HTTPD_400_BAD_REQUEST, "bad request body");
    }
    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < req->content_len) {
        const int got = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return sendError(req, HTTPD_400_BAD_REQUEST, "could not read the body");
        received += got;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return sendError(req, HTTPD_400_BAD_REQUEST, "expected a JSON array of names");
    }

    std::vector<std::string> order;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root)
    {
        if (!cJSON_IsString(item) || !isSafeName(item->valuestring)) {
            cJSON_Delete(root);
            return sendError(req, HTTPD_400_BAD_REQUEST, "bad name in the order");
        }
        order.emplace_back(item->valuestring);
    }
    cJSON_Delete(root);

    // The order must be a permutation of exactly what is on the device. A
    // partial list would leave the photos it omits holding names the renames
    // below are about to claim, and the loser of that collision gets deleted.
    // Cheaper to refuse.
    {
        std::vector<std::string> have;
        for (const auto& photo : listPhotos()) have.push_back(photo.name);
        std::vector<std::string> want = order;
        std::sort(have.begin(), have.end());
        std::sort(want.begin(), want.end());
        if (have != want) {
            return sendError(req, HTTPD_400_BAD_REQUEST,
                             "the order must list every photo exactly once");
        }
    }

    // Two passes through a temporary name: the new name for one photo is very
    // often the current name of another, and a single pass would clobber it.
    hal_storage_lock();
    bool ok       = true;
    size_t parked = 0;
    for (; parked < order.size(); parked++) {
        const std::string from = photoPath(order[parked]);
        const std::string via  = photoPath(reorderTempName(parked));
        if (rename(from.c_str(), via.c_str()) != 0) {
            ok = false;
            break;
        }
    }
    if (ok) {
        for (size_t i = 0; i < order.size(); i++) {
            // Wide enough for any count snprintf could produce; the compiler
            // cannot see that the list is bounded.
            char prefix[16];
            std::snprintf(prefix, sizeof(prefix), "%03u_", (unsigned)(i + 1));
            const std::string via = photoPath(reorderTempName(i));
            const std::string to  = photoPath(prefix + stripOrderPrefix(order[i]));
            unlink(to.c_str());
            if (rename(via.c_str(), to.c_str()) != 0) {
                ok = false;
                break;
            }
        }
    } else {
        // Put back whatever was already parked. Otherwise a failure half way
        // through strands photos under dot-names the frame ignores, and they
        // look deleted.
        for (size_t i = 0; i < parked; i++) {
            rename(photoPath(reorderTempName(i)).c_str(), photoPath(order[i]).c_str());
        }
    }
    hal_storage_unlock();

    if (!ok) {
        return sendError(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not reorder");
    }
    ESP_LOGI(TAG, "reordered %u photos", (unsigned)order.size());
    return listHandler(req);
}

const httpd_uri_t URIS[] = {
    {.uri = "/api/photos", .method = HTTP_GET, .handler = listHandler, .user_ctx = nullptr},
    {.uri = "/api/photos", .method = HTTP_POST, .handler = uploadHandler, .user_ctx = nullptr},
    {.uri = "/api/photos/*", .method = HTTP_DELETE, .handler = deleteHandler, .user_ctx = nullptr},
    {.uri = "/api/storage", .method = HTTP_GET, .handler = storageHandler, .user_ctx = nullptr},
    {.uri = "/api/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr},
    {.uri = "/api/show", .method = HTTP_POST, .handler = showHandler, .user_ctx = nullptr},
    {.uri = "/api/reorder", .method = HTTP_POST, .handler = reorderHandler, .user_ctx = nullptr},
    {.uri = "/photo/*", .method = HTTP_GET, .handler = getPhotoHandler, .user_ctx = nullptr},
};

}  // namespace

esp_err_t photoApiRegister(httpd_handle_t server)
{
    for (const auto& uri : URIS) {
        const esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s", uri.uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

bool photoApiTakeShowRequest(uint16_t& index)
{
    if (!s_show_pending) return false;
    index          = s_show_index;
    s_show_pending = false;
    return true;
}

void photoApiSetRefreshing(bool refreshing)
{
    s_refreshing = refreshing;
}

}  // namespace net
