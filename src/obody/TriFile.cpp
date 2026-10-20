#include "obody/TriFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace stm::obody
{
    namespace
    {
        std::string ToLower(std::string_view a_text)
        {
            std::string out(a_text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        // The tri format is a chain of length-prefixed records with no
        // offsets and no checksums: a truncated or foreign file has to be
        // rejected by reading carefully, not by trusting a header field.
        class Cursor
        {
        public:
            Cursor(const std::uint8_t* a_data, std::size_t a_size) : _data(a_data), _size(a_size) {}

            bool Read(void* a_out, std::size_t a_count)
            {
                if (_pos + a_count > _size) {
                    return false;
                }
                std::memcpy(a_out, _data + _pos, a_count);
                _pos += a_count;
                return true;
            }

            bool ReadString(std::string& a_out)
            {
                std::uint8_t length = 0;
                if (!Read(&length, 1)) {
                    return false;
                }
                if (_pos + length > _size) {
                    return false;
                }
                a_out.assign(reinterpret_cast<const char*>(_data + _pos), length);
                _pos += length;
                return true;
            }

            bool AtEnd() const { return _pos >= _size; }

        private:
            const std::uint8_t* _data;
            std::size_t         _size;
            std::size_t         _pos = 0;
        };

        // "PIRT" little endian.
        constexpr std::uint32_t kTriMagic = 0x54524950u;
        constexpr std::size_t   kMaxTriBytes = 256u * 1024u * 1024u;

        std::shared_ptr<TriFile> ParseTri(const std::vector<std::uint8_t>& a_bytes, std::string& a_error)
        {
            Cursor cursor(a_bytes.data(), a_bytes.size());

            std::uint32_t magic = 0;
            if (!cursor.Read(&magic, 4) || magic != kTriMagic) {
                a_error = "not a body tri file (bad magic)";
                return nullptr;
            }

            auto file = std::make_shared<TriFile>();

            std::uint16_t shapeCount = 0;
            if (!cursor.Read(&shapeCount, 2)) {
                a_error = "truncated shape count";
                return nullptr;
            }

            for (std::uint16_t i = 0; i < shapeCount; ++i) {
                TriShape shape;
                if (!cursor.ReadString(shape.name)) {
                    a_error = "truncated shape name";
                    return nullptr;
                }
                shape.lowerName = ToLower(shape.name);

                std::uint16_t morphCount = 0;
                if (!cursor.Read(&morphCount, 2)) {
                    a_error = "truncated morph count";
                    return nullptr;
                }

                shape.morphs.reserve(morphCount);
                for (std::uint16_t m = 0; m < morphCount; ++m) {
                    TriMorph morph;
                    if (!cursor.ReadString(morph.name)) {
                        a_error = "truncated morph name";
                        return nullptr;
                    }
                    morph.lowerName = ToLower(morph.name);

                    // `mult` scales the int16 displacements into mesh units.
                    float    mult = 0.0f;
                    std::uint16_t vertexCount = 0;
                    if (!cursor.Read(&mult, 4) || !cursor.Read(&vertexCount, 2)) {
                        a_error = "truncated morph header";
                        return nullptr;
                    }

                    morph.offsets.reserve(vertexCount);
                    for (std::uint16_t v = 0; v < vertexCount; ++v) {
                        std::uint16_t index = 0;
                        std::int16_t  x = 0;
                        std::int16_t  y = 0;
                        std::int16_t  z = 0;
                        if (!cursor.Read(&index, 2) || !cursor.Read(&x, 2) ||
                            !cursor.Read(&y, 2) || !cursor.Read(&z, 2)) {
                            a_error = "truncated morph offsets";
                            return nullptr;
                        }

                        MorphOffset offset{ index, x * mult, y * mult, z * mult };
                        if (offset.x != 0.0f || offset.y != 0.0f || offset.z != 0.0f) {
                            morph.offsets.push_back(offset);
                        }
                    }

                    if (!morph.offsets.empty()) {
                        shape.morphs.push_back(std::move(morph));
                    }
                }

                file->shapes.push_back(std::move(shape));
            }

            // The UV half of the file follows and is not needed here: it is
            // written after the position half and can be safely ignored.

            if (file->shapes.empty()) {
                a_error = "tri file has no shapes";
                return nullptr;
            }
            return file;
        }

        struct CacheEntry
        {
            std::filesystem::file_time_type stamp{};
            std::shared_ptr<const TriFile> file;
            std::uint64_t                  lastUse = 0;
        };

        std::mutex                                  g_cacheMutex;
        std::unordered_map<std::string, CacheEntry> g_cache;
        std::uint64_t                               g_cacheTick = 0;

        // Two would be exactly female+male, but a custom body or a garment tri
        // then evicts both and the next preset switch re-parses 5 MB. Four keeps
        // the working set plus one spare; eviction drops the least recently used
        // entry instead of the whole cache, which used to make the two-entry
        // limit cost a full re-parse every time a third file showed up.
        constexpr std::size_t kMaxCachedFiles = 4;

        void EvictOldestLocked()
        {
            if (g_cache.size() < kMaxCachedFiles) {
                return;
            }
            auto oldest = g_cache.begin();
            for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
                if (it->second.lastUse < oldest->second.lastUse) {
                    oldest = it;
                }
            }
            g_cache.erase(oldest);
        }
    }

    const TriMorph* TriShape::FindMorph(std::string_view a_name) const
    {
        const auto lower = ToLower(a_name);
        for (const auto& morph : morphs) {
            if (morph.lowerName == lower) {
                return &morph;
            }
        }
        return nullptr;
    }

    const TriShape* TriFile::FindShape(std::string_view a_name) const
    {
        const auto lower = ToLower(a_name);
        for (const auto& shape : shapes) {
            if (shape.lowerName == lower) {
                return &shape;
            }
        }
        return nullptr;
    }

    const TriShape* TriFile::BestShape() const
    {
        const TriShape* best = nullptr;
        std::size_t     bestWeight = 0;
        for (const auto& shape : shapes) {
            std::size_t weight = 0;
            for (const auto& morph : shape.morphs) {
                weight += morph.offsets.size();
            }
            if (weight > bestWeight) {
                bestWeight = weight;
                best = &shape;
            }
        }
        return best;
    }

    std::shared_ptr<const TriFile> LoadTriFile(const std::string& a_path)
    {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(a_path, ec);
        if (ec) {
            return nullptr;
        }

        {
            std::scoped_lock lock(g_cacheMutex);
            const auto it = g_cache.find(a_path);
            if (it != g_cache.end() && it->second.stamp == stamp && it->second.file) {
                it->second.lastUse = ++g_cacheTick;
                return it->second.file;
            }
        }

        std::ifstream stream(a_path, std::ios::binary);
        if (!stream.is_open()) {
            return nullptr;
        }
        stream.seekg(0, std::ios::end);
        const auto size = stream.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > kMaxTriBytes) {
            return nullptr;
        }
        stream.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(stream.gcount()) != bytes.size()) {
            return nullptr;
        }

        std::string error;
        auto        parsed = ParseTri(bytes, error);
        if (!parsed) {
            return nullptr;
        }

        std::shared_ptr<const TriFile> result = parsed;
        {
            std::scoped_lock lock(g_cacheMutex);
            EvictOldestLocked();
            g_cache[a_path] = CacheEntry{ stamp, result, ++g_cacheTick };
        }
        return result;
    }
}
