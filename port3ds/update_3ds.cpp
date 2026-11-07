// Boot-time update check: asks GitHub for the latest release of this port and, when it is newer than this build,
// offers to download its rs64.3dsx over the running one (it takes effect on the next start). Skipped without Wi-Fi
// or when sdmc:/3ds/rs64/no_update_check.txt exists.

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "renderer_3ds.h"

namespace {

constexpr const char* kVersion = RS3DS_VERSION;
constexpr const char* kApi = "https://api.github.com/repos/yaboimanguy-a11y/rogue-squadron-3ds/releases/latest";
constexpr const char* kAgent = "rogue-squadron-3ds-updater";
constexpr u64 kTimeoutNs = 6000000000ull;

// GET url (following redirects) into out; false on any failure or timeout
bool http_get(std::string url, std::string& out, size_t limit) {
    for (int hop = 0; hop < 5; hop++) {
        httpcContext c;
        if (R_FAILED(httpcOpenContext(&c, HTTPC_METHOD_GET, url.c_str(), 1))) return false;
        httpcSetSSLOpt(&c, SSLCOPT_DisableVerify);
        httpcSetKeepAlive(&c, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&c, "User-Agent", kAgent);
        httpcAddRequestHeaderField(&c, "Accept", "*/*");
        u32 status = 0;
        if (R_FAILED(httpcBeginRequest(&c)) || R_FAILED(httpcGetResponseStatusCodeTimeout(&c, &status, kTimeoutNs))) {
            httpcCloseContext(&c);
            return false;
        }
        if (status >= 301 && status <= 308) {
            char loc[1024] = {};
            const bool ok = R_SUCCEEDED(httpcGetResponseHeader(&c, "Location", loc, sizeof loc - 1));
            httpcCloseContext(&c);
            if (!ok) return false;
            url = loc;
            continue;
        }
        if (status != 200) {
            httpcCloseContext(&c);
            return false;
        }
        out.clear();
        char buf[16384];
        for (;;) {
            u32 got = 0;
            const Result r = httpcReceiveDataTimeout(&c, (u8*)buf, sizeof buf, kTimeoutNs);
            httpcGetDownloadSizeState(&c, &got, nullptr);
            const size_t have = out.size();
            if (got > have) out.append(buf, got - have);
            if (out.size() > limit) break;
            if (r != (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) {
                httpcCloseContext(&c);
                return R_SUCCEEDED(r) && out.size() <= limit;
            }
        }
        httpcCloseContext(&c);
        return false;
    }
    return false;
}

// the string value of "key" in a JSON text (first match from pos), or empty
std::string json_str(const std::string& j, const char* key, size_t from = 0) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    p = j.find('"', j.find(':', p + k.size()) + 1);
    if (p == std::string::npos) return {};
    const size_t e = j.find('"', p + 1);
    return e == std::string::npos ? std::string() : j.substr(p + 1, e - p - 1);
}

// "v0.12" -> 0, 12: compares up to four numeric parts
bool newer(const std::string& a, const std::string& b) {
    long x[4] = {}, y[4] = {};
    auto parse = [](const std::string& v, long* o) {
        const char* p = v.c_str();
        while (*p && (*p < '0' || *p > '9')) p++;
        for (int i = 0; i < 4 && *p; i++) {
            char* e;
            o[i] = std::strtol(p, &e, 10);
            p = *e == '.' ? e + 1 : e;
            if (*e != '.') break;
        }
    };
    parse(a, x);
    parse(b, y);
    for (int i = 0; i < 4; i++) {
        if (x[i] != y[i]) return x[i] > y[i];
    }
    return false;
}

bool wifi_up() {
    u32 st = 0;
    if (R_FAILED(acInit())) return false;
    const bool ok = R_SUCCEEDED(ACU_GetWifiStatus(&st)) && st != 0;
    acExit();
    return ok;
}

} // namespace

void rs3ds_check_for_update(const char* dir) {
    {
        const std::string skip = std::string(dir) + "/no_update_check.txt";
        if (FILE* f = std::fopen(skip.c_str(), "r")) {
            std::fclose(f);
            return;
        }
    }
    if (!wifi_up()) return;
    if (R_FAILED(httpcInit(0x10000))) return;
    std::printf("Checking for updates...\n");
    std::string json;
    const bool got = http_get(kApi, json, 256 * 1024);
    const std::string tag = got ? json_str(json, "tag_name") : std::string();
    std::fprintf(stderr, "[update] this build %s, latest release %s\n", kVersion, tag.empty() ? "(unknown)" : tag.c_str());
    if (tag.empty() || !newer(tag, kVersion)) {
        if (!tag.empty()) std::printf("Up to date (%s).\n", kVersion);
        httpcExit();
        return;
    }

    // the release's .3dsx asset
    std::string asset;
    for (size_t p = json.find("\"browser_download_url\""); p != std::string::npos; p = json.find("\"browser_download_url\"", p + 1)) {
        const std::string u = json_str(json, "browser_download_url", p);
        if (u.size() > 5 && u.compare(u.size() - 5, 5, ".3dsx") == 0) {
            asset = u;
            break;
        }
    }
    std::printf("\nUpdate available: %s (you have %s).\n", tag.c_str(), kVersion);
    if (asset.empty()) {
        std::printf("Get it from github.com/yaboimanguy-a11y/\nrogue-squadron-3ds/releases\n\n");
        httpcExit();
        return;
    }
    std::printf("A: download and install   B: skip\n");
    bool go = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_A) { go = true; break; }
        if (k & KEY_B) break;
        gspWaitForVBlank();
    }
    if (go) {
        std::printf("Downloading...\n");
        std::string bin;
        if (http_get(asset, bin, 32u * 1024 * 1024) && bin.size() > 1024 && std::memcmp(bin.data(), "3DSX", 4) == 0) {
            const std::string tmp = std::string(dir) + "/rs64.3dsx.new", dst = std::string(dir) + "/rs64.3dsx";
            FILE* f = std::fopen(tmp.c_str(), "wb");
            const bool wrote = f && std::fwrite(bin.data(), 1, bin.size(), f) == bin.size();
            if (f) std::fclose(f);
            if (wrote && (std::remove(dst.c_str()), std::rename(tmp.c_str(), dst.c_str()) == 0)) {
                std::printf("Installed %s. It starts next time.\n", tag.c_str());
                std::fprintf(stderr, "[update] installed %s\n", tag.c_str());
            } else {
                std::printf("Could not write the update to the SD card.\n");
            }
        } else {
            std::printf("Download failed; starting this version.\n");
        }
    }
    httpcExit();
}
