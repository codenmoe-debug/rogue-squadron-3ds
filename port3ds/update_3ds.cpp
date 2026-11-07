// Boot-time updates. 1) GitHub: says when a newer release of this port is out (releases carry source only).
// 2) Your PC: when update_source.txt in the SD folder holds "IP:port" of a PC running build_from_rom.py --serve and the PC
// has a different build, offers to install it: the 3dsx over this one, or the CIA through the system installer when the
// game runs from the HOME menu. Skipped without Wi-Fi or when no_update_check.txt exists.

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "renderer_3ds.h"
#include "rs3ds_build_id.h"

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

    std::printf("\nNew release: %s (you have %s).\nRun build_from_rom.py --serve on your PC to\nbuild it; it installs here at the next start.\n\n", tag.c_str(), kVersion);
    httpcExit();
}

namespace {

bool ask(const char* q) {
    std::printf("%s\nA: yes   B: no\n", q);
    while (aptMainLoop()) {
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_A) return true;
        if (k & KEY_B) return false;
        gspWaitForVBlank();
    }
    return false;
}

bool install_cia(const std::string& data) {
    if (R_FAILED(amInit())) return false;
    Handle h = 0;
    bool ok = R_SUCCEEDED(AM_StartCiaInstall(MEDIATYPE_SD, &h));
    for (size_t off = 0; ok && off < data.size(); off += 0x10000) {
        u32 wrote = 0;
        const u32 n = (u32)std::min<size_t>(0x10000, data.size() - off);
        ok = R_SUCCEEDED(FSFILE_Write(h, &wrote, off, data.data() + off, n, 0)) && wrote == n;
    }
    if (ok) ok = R_SUCCEEDED(AM_FinishCiaInstall(h));
    else if (h) AM_CancelCIAInstall(h);
    amExit();
    return ok;
}

} // namespace

void rs3ds_update_from_pc(const char* dir) {
    char host[128] = {};
    {
        const std::string cfg = std::string(dir) + "/update_source.txt";
        FILE* f = std::fopen(cfg.c_str(), "r");
        if (!f) return;
        if (!std::fgets(host, sizeof host, f)) host[0] = 0;
        std::fclose(f);
        host[std::strcspn(host, "\r\n ")] = 0;
        if (!host[0]) return;
    }
    {
        const std::string skip = std::string(dir) + "/no_update_check.txt";
        if (FILE* f = std::fopen(skip.c_str(), "r")) {
            std::fclose(f);
            return;
        }
    }
    if (!wifi_up() || R_FAILED(httpcInit(0x10000))) return;
    const std::string base = std::string("http://") + host + "/";
    std::string id;
    if (http_get(base + "build_id.txt", id, 256)) {
        id.erase(id.find_last_not_of("\r\n ") + 1);
        std::fprintf(stderr, "[update] PC %s has build %s, this is %s\n", host, id.c_str(), RS3DS_BUILD_ID);
        const bool cia = !envIsHomebrew();
        if (!id.empty() && id != RS3DS_BUILD_ID && ask("\nYour PC has a new build. Install it?")) {
            std::printf("Downloading...\n");
            std::string bin;
            const bool got = http_get(base + (cia ? "rs64.cia" : "rs64.3dsx"), bin, 48u * 1024 * 1024) && bin.size() > 1024;
            bool ok = false;
            if (got && cia) {
                std::printf("Installing...\n");
                ok = install_cia(bin);
            } else if (got && std::memcmp(bin.data(), "3DSX", 4) == 0) {
                const std::string tmp = std::string(dir) + "/rs64.3dsx.new", dst = std::string(dir) + "/rs64.3dsx";
                FILE* f = std::fopen(tmp.c_str(), "wb");
                ok = f && std::fwrite(bin.data(), 1, bin.size(), f) == bin.size();
                if (f) std::fclose(f);
                if (ok) {
                    std::remove(dst.c_str());
                    ok = std::rename(tmp.c_str(), dst.c_str()) == 0;
                }
            }
            std::printf(ok ? "Installed. Restart the game to use it.\n" : "Update failed; starting this version.\n");
            std::fprintf(stderr, "[update] install %s\n", ok ? "done" : "failed");
            if (ok && ask("Quit now?")) {
                httpcExit();
                svcExitProcess();
            }
        }
    }
    httpcExit();
}

