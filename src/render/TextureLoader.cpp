#include "render/TextureLoader.h"
#include "render/DDSTextureLoader11.h"
#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>
#include <algorithm>
#include <cctype>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stm::render
{
    namespace
    {
        constexpr std::size_t kMaxCacheEntries = 32;

        struct CacheEntry
        {
            std::string pathKey;
            ID3D11ShaderResourceView* srv = nullptr;
        };

        std::mutex s_cacheMutex;
        std::list<CacheEntry> s_lruList;
        std::unordered_map<std::string, std::list<CacheEntry>::iterator> s_cacheMap;

        bool CaseInsensitiveStartsWith(std::string_view a_str, std::string_view a_prefix)
        {
            if (a_str.size() < a_prefix.size()) return false;
            for (std::size_t i = 0; i < a_prefix.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a_str[i])) !=
                    std::tolower(static_cast<unsigned char>(a_prefix[i]))) {
                    return false;
                }
            }
            return true;
        }

        ID3D11ShaderResourceView* LoadDdsFromVfsStream(RE::BSResourceNiBinaryStream& a_stream, const std::string& a_path)
        {
            std::uint32_t totalSize = a_stream.stream ? a_stream.stream->totalSize : 0;
            if (totalSize == 0 && a_stream.stream) {
                RE::BSResource::Info info;
                if (a_stream.stream->DoGetInfo(info) == RE::BSResource::ErrorCode::kNone) {
                    totalSize = static_cast<std::uint32_t>(info.fileSize);
                }
            }

            if (totalSize < 128) {
                SKSE::log::warn("TextureLoader: file '{}' size too small ({} bytes) for DDS", a_path, totalSize);
                return nullptr;
            }

            std::vector<std::uint8_t> buffer(totalSize);
            if (!a_stream.read(buffer.data(), totalSize)) {
                SKSE::log::warn("TextureLoader: failed reading {} bytes from '{}'", totalSize, a_path);
                return nullptr;
            }

            auto* bsDev = RE::BSGraphics::Renderer::GetDevice();
            if (!bsDev) {
                SKSE::log::warn("TextureLoader: BSGraphics::Renderer device not available");
                return nullptr;
            }
            auto* d3dDev = reinterpret_cast<ID3D11Device*>(bsDev);

            ID3D11ShaderResourceView* srv = nullptr;
            HRESULT hr = DirectX::CreateDDSTextureFromMemory(
                d3dDev,
                buffer.data(),
                buffer.size(),
                nullptr,
                &srv
            );

            if (FAILED(hr) || !srv) {
                SKSE::log::warn("TextureLoader: CreateDDSTextureFromMemory failed (0x{:08X}) for '{}'",
                    static_cast<std::uint32_t>(hr), a_path);
                return nullptr;
            }

            SKSE::log::info("TextureLoader: loaded '{}' ({} bytes, VFS)", a_path, totalSize);
            return srv;
        }

        ID3D11ShaderResourceView* LoadDdsFromVfsDirect(const std::string& a_vfsPath)
        {
            // 1. Try opening the VFS stream using BSResource
            RE::BSResourceNiBinaryStream stream(a_vfsPath.c_str());
            if (!stream.good()) {
                // If backslash failed or forward slash failed, try the opposite slash style
                std::string altPath = a_vfsPath;
                char target = (a_vfsPath.find('/') != std::string::npos) ? '\\' : '/';
                for (char& c : altPath) {
                    if (c == '/' || c == '\\') c = target;
                }
                RE::BSResourceNiBinaryStream altStream(altPath.c_str());
                if (!altStream.good()) {
                    SKSE::log::warn("TextureLoader: file '{}' not found in Skyrim VFS (loose or BSA)", a_vfsPath);
                    return nullptr;
                }
                return LoadDdsFromVfsStream(altStream, altPath);
            }

            return LoadDdsFromVfsStream(stream, a_vfsPath);
        }
    }

    std::string TextureLoader::NormalizeVfsPath(std::string_view a_rawPath)
    {
        if (a_rawPath.empty()) {
            return {};
        }

        std::string path(a_rawPath);
        for (char& c : path) {
            if (c == '/') {
                c = '\\';
            }
        }

        // Trim leading spaces or backslashes
        std::size_t start = 0;
        while (start < path.size() && (path[start] == ' ' || path[start] == '\\')) {
            start++;
        }
        if (start > 0) {
            path.erase(0, start);
        }

        // Strip leading "data\"
        if (CaseInsensitiveStartsWith(path, "data\\")) {
            path.erase(0, 5);
        }

        // Ensure "textures\actors\character\slavetats\" prefix
        if (!CaseInsensitiveStartsWith(path, "textures\\")) {
            if (CaseInsensitiveStartsWith(path, "actors\\character\\slavetats\\")) {
                path = "textures\\" + path;
            } else if (CaseInsensitiveStartsWith(path, "slavetats\\")) {
                path = "textures\\actors\\character\\" + path;
            } else {
                path = "textures\\actors\\character\\slavetats\\" + path;
            }
        }

        // Lowercase the extension if .dds
        if (path.size() >= 4) {
            std::string ext = path.substr(path.size() - 4);
            if (CaseInsensitiveStartsWith(ext, ".dds")) {
                path.replace(path.size() - 4, 4, ".dds");
            }
        } else {
            path += ".dds";
        }

        return path;
    }

    ID3D11ShaderResourceView* TextureLoader::LoadTattooTexture(const std::string& a_rawPath)
    {
        if (a_rawPath.empty()) {
            return nullptr;
        }

        const std::string vfsPath = NormalizeVfsPath(a_rawPath);
        if (vfsPath.empty()) {
            return nullptr;
        }

        std::lock_guard<std::mutex> lock(s_cacheMutex);

        auto it = s_cacheMap.find(vfsPath);
        if (it != s_cacheMap.end()) {
            // Move touched entry to front of LRU list
            s_lruList.splice(s_lruList.begin(), s_lruList, it->second);
            return it->second->srv;
        }

        // Not in cache, load from VFS
        ID3D11ShaderResourceView* srv = LoadDdsFromVfsDirect(vfsPath);
        if (!srv) {
            return nullptr;
        }

        // Evict LRU entry if cache reached limit
        if (s_lruList.size() >= kMaxCacheEntries) {
            auto& oldest = s_lruList.back();
            if (oldest.srv) {
                oldest.srv->Release();
            }
            s_cacheMap.erase(oldest.pathKey);
            s_lruList.pop_back();
        }

        // Insert new entry at front
        s_lruList.push_front({ vfsPath, srv });
        s_cacheMap[vfsPath] = s_lruList.begin();

        return srv;
    }

    void TextureLoader::ClearCache()
    {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        for (auto& entry : s_lruList) {
            if (entry.srv) {
                entry.srv->Release();
                entry.srv = nullptr;
            }
        }
        s_lruList.clear();
        s_cacheMap.clear();
    }
}
