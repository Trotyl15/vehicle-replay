#ifndef MAP_TILES_H
#define MAP_TILES_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include "stb_image.h"

// ─────────────────────────────────────────────────────
// Slippy-map tile math  (OSM / Google convention)
// ─────────────────────────────────────────────────────
struct TileCoord {
    int x, y, z;
    bool operator==(const TileCoord& o) const { return x==o.x && y==o.y && z==o.z; }
};

struct TileCoordHash {
    size_t operator()(const TileCoord& t) const {
        return std::hash<int>()(t.x) ^ (std::hash<int>()(t.y) << 10) ^ (std::hash<int>()(t.z) << 20);
    }
};

// Convert lat/lon → tile index at zoom z
inline TileCoord latLonToTile(double lat, double lon, int z) {
    double n = std::pow(2.0, z);
    int tx = (int)std::floor((lon + 180.0) / 360.0 * n);
    double latRad = lat * M_PI / 180.0;
    int ty = (int)std::floor((1.0 - std::log(std::tan(latRad) + 1.0/std::cos(latRad)) / M_PI) / 2.0 * n);
    return {tx, ty, z};
}

// Convert tile index → lat/lon of the NW corner
inline void tileToLatLon(int tx, int ty, int z, double& lat, double& lon) {
    double n = std::pow(2.0, z);
    lon = tx / n * 360.0 - 180.0;
    double latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * ty / n)));
    lat = latRad * 180.0 / M_PI;
}

// ─────────────────────────────────────────────────────
// MapTileManager – fetch, cache & upload tiles to GL
// ─────────────────────────────────────────────────────
class MapTileManager {
public:
    int zoomLevel = 18;         // default zoom
    bool enabled = true;
    bool useSatellite = false;  // false = roadmap, true = satellite

    // Reference origin for ENU conversion (set by the application)
    double refLat = 0, refLon = 0;
    bool   refSet = false;

    // Scale factor matching the app's ENU→GL scaling
    double enuScale = 0.025;    // same as main.cpp

    MapTileManager() {
        // Create cache directory
        mkdir("map_cache", 0755);
    }

    ~MapTileManager() {
        for (auto& [k, tex] : textures) {
            if (tex) glDeleteTextures(1, &tex);
        }
    }

    void setReference(double lat, double lon) {
        refLat = lat; refLon = lon; refSet = true;
    }

    // Call each frame with current lat/lon; loads surrounding tiles
    void update(double lat, double lon) {
        if (!enabled) return;
        if (!refSet) { setReference(lat, lon); }

        TileCoord center = latLonToTile(lat, lon, zoomLevel);

        // Load a 5×5 grid of tiles around the car
        const int radius = 2;
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                TileCoord tc = {center.x + dx, center.y + dy, zoomLevel};
                ensureTile(tc);
            }
        }
    }

    // Render all loaded tiles as textured ground quads
    void render(const glm::mat4& viewProj, GLuint quadVAO) {
        if (!enabled || textures.empty()) return;

        for (auto& [tc, tex] : textures) {
            if (!tex) continue;

            // Compute the world-space quad for this tile
            double nwLat, nwLon, seLat, seLon;
            tileToLatLon(tc.x,     tc.y,     tc.z, nwLat, nwLon);
            tileToLatLon(tc.x + 1, tc.y + 1, tc.z, seLat, seLon);

            // Convert corners to ENU then to GL coords (same as main.cpp)
            glm::dvec2 nwENU = llToENULocal(nwLat, nwLon);
            glm::dvec2 seENU = llToENULocal(seLat, seLon);

            float x0 = (float)(nwENU.x * enuScale);
            float z0 = (float)(-nwENU.y * enuScale);   // north → -Z
            float x1 = (float)(seENU.x * enuScale);
            float z1 = (float)(-seENU.y * enuScale);

            // Store tile render info for the shader
            TileRenderInfo info;
            info.tex = tex;
            info.x0 = x0; info.z0 = z0;
            info.x1 = x1; info.z1 = z1;
            tileRenderList.push_back(info);
        }
    }

    struct TileRenderInfo {
        GLuint tex;
        float x0, z0, x1, z1;   // NW and SE corners in GL coords
    };
    std::vector<TileRenderInfo> tileRenderList;

    void clearRenderList() { tileRenderList.clear(); }

private:
    std::unordered_map<TileCoord, GLuint, TileCoordHash> textures;
    std::unordered_map<TileCoord, bool, TileCoordHash>   pending;  // tiles we tried

    static constexpr double EARTH_R = 6'378'137.0;

    glm::dvec2 llToENULocal(double lat, double lon) const {
        double dLat = (lat - refLat) * M_PI / 180.0;
        double dLon = (lon - refLon) * M_PI / 180.0;
        double north = EARTH_R * dLat;
        double east  = EARTH_R * dLon * std::cos(refLat * M_PI / 180.0);
        return {east, north};
    }

    std::string tileCachePath(const TileCoord& tc) const {
        std::ostringstream oss;
        oss << "map_cache/" << tc.z << "_" << tc.x << "_" << tc.y << ".png";
        return oss.str();
    }

    std::string tileURL(const TileCoord& tc) const {
        // Google Maps tile URL
        // mt0-mt3 are Google's tile servers
        int server = (tc.x + tc.y) % 4;
        std::ostringstream oss;
        if (useSatellite) {
            oss << "https://mt" << server << ".google.com/vt/lyrs=s&x="
                << tc.x << "&y=" << tc.y << "&z=" << tc.z;
        } else {
            oss << "https://mt" << server << ".google.com/vt/lyrs=m&x="
                << tc.x << "&y=" << tc.y << "&z=" << tc.z;
        }
        return oss.str();
    }

    void ensureTile(const TileCoord& tc) {
        // Already loaded or tried?
        if (textures.count(tc) || pending.count(tc)) return;
        pending[tc] = true;

        std::string cachePath = tileCachePath(tc);

        // Try loading from disk cache first
        if (fileExists(cachePath)) {
            GLuint tex = loadTextureFromFile(cachePath);
            if (tex) { textures[tc] = tex; return; }
        }

        // Download in background wouldn't block – but for simplicity
        // we do a synchronous curl here (tiles are small, ~20-40KB)
        std::string url = tileURL(tc);
        std::string cmd = "curl -s -o \"" + cachePath + "\" \"" + url + "\" 2>/dev/null";
        int ret = system(cmd.c_str());
        if (ret == 0 && fileExists(cachePath)) {
            GLuint tex = loadTextureFromFile(cachePath);
            if (tex) { textures[tc] = tex; }
        }
    }

    bool fileExists(const std::string& path) const {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && st.st_size > 0;
    }

    GLuint loadTextureFromFile(const std::string& path) const {
        int w, h, ch;
        // Don't flip – Google tiles have origin at top-left, and we handle UVs in the shader
        stbi_set_flip_vertically_on_load(false);
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!data) {
            // Not a valid image (could be an error page); remove it
            std::remove(path.c_str());
            return 0;
        }

        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(data);

        // Reset flip state for other loaders
        stbi_set_flip_vertically_on_load(true);
        return tex;
    }
};

#endif // MAP_TILES_H
