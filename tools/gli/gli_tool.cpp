//============================================================================================
// gli_tool - Export/Import von Bildern aus .gli Grafikbibliotheken als PNG
//
//   gli_tool list   <lib.gli> [filter]
//   gli_tool export <lib.gli> <outdir> [filter]
//   gli_tool import <lib.gli> <indir> <out.gli>
//
// Format siehe src/SBLib/source/GfxLib.cpp. Beim Import wird die Bibliothek unverändert
// kopiert; jedes ersetzte Bild wird ans Dateiende angehängt und nur der Offset im
// Verzeichniseintrag umgebogen. Dadurch dürfen sich die Bildmaße ändern.
//============================================================================================
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_image.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#pragma pack(push, 1)
struct ChunkInfo {
    uint32_t Size;
    char Type;
};

struct ChunkHeader {
    char Name[8];
    uint32_t Offset;
};

struct ChunkImage {
    uint32_t Length; // 76 bytes
    uint32_t Size;
    uint32_t Width;
    uint32_t Height;
    uint32_t Unknown0;
    uint32_t Flags;
    uint32_t BitDepth;
    uint32_t PlaneSize;
    uint32_t Rmask;
    uint32_t Gmask;
    uint32_t Bmask;
    uint32_t OffsetColor;
    uint32_t OffsetAlpha;
    uint32_t OffsetZ;
    uint32_t Unknown[5];
};
#pragma pack(pop)

static_assert(sizeof(ChunkImage) == 76, "ChunkImage must match the on-disk layout");

enum { CHUNK_GFX = 1 };

struct Entry {
    std::string Name;
    size_t OffsetFieldPos; // Position des Offset-Feldes im Verzeichnis (zum Patchen)
    size_t ImagePos;       // Position des Bild-Headers in der Datei
    ChunkImage Image;
};

static std::vector<uint8_t> ReadFile(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

template <typename T> static T Get(const std::vector<uint8_t> &data, size_t pos) {
    T value{};
    if (pos + sizeof(T) <= data.size()) {
        memcpy(&value, data.data() + pos, sizeof(T));
    }
    return value;
}

static bool ParseLib(const std::vector<uint8_t> &data, std::vector<Entry> &entries) {
    if (data.size() < 54 || memcmp(data.data(), "GLIB", 4) != 0) {
        fprintf(stderr, "Not a GLIB file\n");
        return false;
    }

    // Header: siehe GfxLibHeader (Files @34, Pos @38)
    const uint32_t files = Get<uint32_t>(data, 34);
    size_t pos = Get<uint32_t>(data, 38);

    for (uint32_t i = 0; i < files; i++) {
        const auto info = Get<ChunkInfo>(data, pos);
        if (info.Size == 0) {
            fprintf(stderr, "Corrupt directory at entry %u\n", i);
            return false;
        }

        if (info.Type == CHUNK_GFX) {
            const auto chunk = Get<ChunkHeader>(data, pos + sizeof(ChunkInfo));

            Entry e;
            e.Name.assign(chunk.Name, strnlen(chunk.Name, sizeof(chunk.Name)));
            e.OffsetFieldPos = pos + sizeof(ChunkInfo) + offsetof(ChunkHeader, Offset);
            e.Image = Get<ChunkImage>(data, chunk.Offset);
            e.ImagePos = chunk.Offset;
            entries.push_back(e);
        }

        pos += info.Size;
    }
    return true;
}

static bool NameMatches(const std::string &name, const char *filter) { return filter == nullptr || name.find(filter) != std::string::npos; }

static int CmdList(const fs::path &lib, const char *filter) {
    std::vector<Entry> entries;
    if (!ParseLib(ReadFile(lib), entries)) {
        return 1;
    }

    for (const auto &e : entries) {
        if (NameMatches(e.Name, filter)) {
            printf("%-8s %5ux%-5u %2u bpp  R=%04x G=%04x B=%04x\n", e.Name.c_str(), e.Image.Width, e.Image.Height, e.Image.BitDepth, e.Image.Rmask,
                   e.Image.Gmask, e.Image.Bmask);
        }
    }
    printf("%zu images\n", entries.size());
    return 0;
}

static int CmdExport(const fs::path &lib, const fs::path &outDir, const char *filter) {
    const auto data = ReadFile(lib);
    std::vector<Entry> entries;
    if (!ParseLib(data, entries)) {
        return 1;
    }

    fs::create_directories(outDir);

    int count = 0;
    for (const auto &e : entries) {
        if (!NameMatches(e.Name, filter)) {
            continue;
        }
        const ChunkImage &img = e.Image;
        if (img.BitDepth != 16 || img.Height == 0) {
            fprintf(stderr, "Skipping %s: unsupported %u bpp\n", e.Name.c_str(), img.BitDepth);
            continue;
        }

        const size_t pixelPos = e.ImagePos + sizeof(ChunkImage);
        if (pixelPos + img.Size > data.size()) {
            fprintf(stderr, "Skipping %s: truncated\n", e.Name.c_str());
            continue;
        }

        SDL_Surface *src = SDL_CreateRGBSurfaceFrom(const_cast<uint8_t *>(data.data() + pixelPos), static_cast<int>(img.Width), static_cast<int>(img.Height),
                                                    16, static_cast<int>(img.Size / img.Height), img.Rmask, img.Gmask, img.Bmask, 0);
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);

        // Farbe 0 ist der Color-Key des Spiels -> transparent
        for (uint32_t y = 0; y < img.Height; y++) {
            const auto *srcRow = reinterpret_cast<const uint16_t *>(static_cast<uint8_t *>(src->pixels) + y * src->pitch);
            auto *dstRow = static_cast<uint8_t *>(rgba->pixels) + y * rgba->pitch;
            for (uint32_t x = 0; x < img.Width; x++) {
                dstRow[x * 4 + 3] = srcRow[x] == 0 ? 0 : 255;
            }
        }

        const fs::path out = outDir / (e.Name + ".png");
        if (IMG_SavePNG(rgba, out.string().c_str()) != 0) {
            fprintf(stderr, "Failed to write %s: %s\n", out.string().c_str(), IMG_GetError());
        } else {
            count++;
        }
        SDL_FreeSurface(rgba);
        SDL_FreeSurface(src);
    }

    printf("Exported %d images to %s\n", count, outDir.string().c_str());
    return 0;
}

static int CmdImport(const fs::path &lib, const fs::path &inDir, const fs::path &outLib) {
    auto data = ReadFile(lib);
    std::vector<Entry> entries;
    if (!ParseLib(data, entries)) {
        return 1;
    }

    int count = 0;
    for (const auto &e : entries) {
        const fs::path png = inDir / (e.Name + ".png");
        if (!fs::exists(png)) {
            continue;
        }
        if (e.Image.BitDepth != 16) {
            fprintf(stderr, "Skipping %s: unsupported %u bpp\n", e.Name.c_str(), e.Image.BitDepth);
            continue;
        }

        SDL_Surface *loaded = IMG_Load(png.string().c_str());
        if (loaded == nullptr) {
            fprintf(stderr, "Failed to load %s: %s\n", png.string().c_str(), IMG_GetError());
            continue;
        }
        SDL_Surface *rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(loaded);

        // In das Originalformat der Bibliothek zurückwandeln
        SDL_PixelFormat *fmt = SDL_AllocFormat(SDL_MasksToPixelFormatEnum(16, e.Image.Rmask, e.Image.Gmask, e.Image.Bmask, 0));
        const auto w = static_cast<uint32_t>(rgba->w);
        const auto h = static_cast<uint32_t>(rgba->h);
        std::vector<uint16_t> pixels(static_cast<size_t>(w) * h);

        for (uint32_t y = 0; y < h; y++) {
            const auto *row = static_cast<uint8_t *>(rgba->pixels) + y * rgba->pitch;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                uint16_t value = 0;
                if (p[3] >= 128) {
                    value = static_cast<uint16_t>(SDL_MapRGB(fmt, p[0], p[1], p[2]));
                    if (value == 0) {
                        value = static_cast<uint16_t>(SDL_MapRGB(fmt, 0, 8, 0)); // Schwarz darf nicht zum Color-Key werden
                    }
                }
                pixels[static_cast<size_t>(y) * w + x] = value;
            }
        }
        SDL_FreeFormat(fmt);
        SDL_FreeSurface(rgba);

        ChunkImage img = e.Image;
        const uint32_t oldSize = img.Size;
        img.Width = w;
        img.Height = h;
        img.Size = w * h * 2;
        if (img.PlaneSize == oldSize) {
            img.PlaneSize = img.Size;
        }

        // Neues Bild anhängen und Verzeichniseintrag umbiegen
        const auto newOffset = static_cast<uint32_t>(data.size());
        const auto *imgBytes = reinterpret_cast<const uint8_t *>(&img);
        data.insert(data.end(), imgBytes, imgBytes + sizeof(img));
        const auto *pixBytes = reinterpret_cast<const uint8_t *>(pixels.data());
        data.insert(data.end(), pixBytes, pixBytes + img.Size);
        memcpy(data.data() + e.OffsetFieldPos, &newOffset, sizeof(newOffset));

        printf("Imported %s (%ux%u)\n", e.Name.c_str(), w, h);
        count++;
    }

    std::ofstream out(outLib, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    printf("Wrote %s with %d replaced images\n", outLib.string().c_str(), count);
    return out.good() ? 0 : 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage:\n"
                        "  gli_tool list   <lib.gli> [filter]\n"
                        "  gli_tool export <lib.gli> <outdir> [filter]\n"
                        "  gli_tool import <lib.gli> <indir> <out.gli>\n");
        return 1;
    }

    SDL_SetMainReady();
    IMG_Init(IMG_INIT_PNG);

    const std::string cmd = argv[1];
    int result = 1;
    if (cmd == "list") {
        result = CmdList(argv[2], argc > 3 ? argv[3] : nullptr);
    } else if (cmd == "export" && argc >= 4) {
        result = CmdExport(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
    } else if (cmd == "import" && argc >= 5) {
        if (fs::exists(argv[4]) && fs::equivalent(argv[2], argv[4])) {
            fprintf(stderr, "Refusing to overwrite the source library; write to a new file\n");
        } else {
            result = CmdImport(argv[2], argv[3], argv[4]);
        }
    } else {
        fprintf(stderr, "Unknown command or missing arguments\n");
    }

    IMG_Quit();
    return result;
}
