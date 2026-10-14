// imslug — Slug-based GPU text rendering for Dear ImGui.
//
// imslug_gen_main.cpp — the offline generator CLI (M7):
//
//   imslug_gen font <font.ttf> --size-range 8:72 --chars a-z,0x20-0x7E -o font.imslug
//   imslug_gen font <font.ttf> --chars 0x20-0x7E --emit-header font_asset.h
//   imslug_gen info <font.imslug>
//   imslug_gen --help | --version
//
// The `font` subcommand builds a `.imslug` container v1 (docs/PACKED_FORMAT.md)
// through imslug_gen_core, which uses the same runtime core code as the
// renderer (font handle, band construction, packer). The `info` subcommand
// validates an asset and prints its identity/pool summary. Everything is
// deterministic: stable ordering, zeroed alignment padding, no timestamps.
//
// Charset spec (`--chars`): comma/space-separated items; each item is a
// literal UTF-8 character, a literal `a-z` range, a hex codepoint `0x20` /
// `U+20`, a hex range `0x20-0x7E` / `U+20-U+7E`, or the keyword `all` (every
// codepoint the font's cmap maps). Codepoints missing from the font are
// reported and skipped.

#include "imslug_gen_writer.h"

#include "imslug_c.h"  // IMSLUG_VERSION (no imgui.h dependency)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    using namespace ImSlug;  // the tool consumes the core's implementation API directly

    // ---------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------
    bool IsSeparator(char c)
    {
        return c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    bool IsHexDigit(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return c - 'A' + 10;
    }

    // Decodes one UTF-8 character; returns false on malformed input.
    bool DecodeUtf8(const char* text, int length, uint32_t* outCodepoint, int* outConsumed)
    {
        const unsigned char* bytes = (const unsigned char*)text;
        if (length <= 0)
            return false;
        if (bytes[0] < 0x80)
        {
            *outCodepoint = bytes[0];
            *outConsumed = 1;
            return true;
        }
        int continuation = 0;
        uint32_t value = 0;
        if ((bytes[0] & 0xE0) == 0xC0)
        {
            continuation = 1;
            value = bytes[0] & 0x1F;
        }
        else if ((bytes[0] & 0xF0) == 0xE0)
        {
            continuation = 2;
            value = bytes[0] & 0x0F;
        }
        else if ((bytes[0] & 0xF8) == 0xF0)
        {
            continuation = 3;
            value = bytes[0] & 0x07;
        }
        else
        {
            return false;
        }
        if (length < 1 + continuation)
            return false;
        for (int i = 1; i <= continuation; ++i)
        {
            if ((bytes[i] & 0xC0) != 0x80)
                return false;
            value = (value << 6) | (bytes[i] & 0x3F);
        }
        *outCodepoint = value;
        *outConsumed = 1 + continuation;
        return true;
    }

    // Parses one side of a charset item: a hex codepoint (`0xHH..`/`U+HH..`)
    // or exactly one literal UTF-8 character.
    bool ParseCodepoint(const char* text, int length, uint32_t* outCodepoint)
    {
        if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            uint32_t value = 0;
            if (length <= 2 || length > 2 + 6)
                return false;
            for (int i = 2; i < length; ++i)
            {
                if (!IsHexDigit(text[i]))
                    return false;
                value = (value << 4) | (uint32_t)HexValue(text[i]);
            }
            *outCodepoint = value;
            return true;
        }
        if (length >= 2 && (text[0] == 'U' || text[0] == 'u') && text[1] == '+')
        {
            uint32_t value = 0;
            if (length <= 2 || length > 2 + 6)
                return false;
            for (int i = 2; i < length; ++i)
            {
                if (!IsHexDigit(text[i]))
                    return false;
                value = (value << 4) | (uint32_t)HexValue(text[i]);
            }
            *outCodepoint = value;
            return true;
        }
        int consumed = 0;
        if (!DecodeUtf8(text, length, outCodepoint, &consumed) || consumed != length)
            return false;
        return true;
    }

    struct CodepointList
    {
        uint32_t* Values;
        int Count;
        int Capacity;
    };

    bool AppendCodepoint(CodepointList* list, uint32_t codepoint)
    {
        if (list->Count >= list->Capacity)
        {
            const int newCapacity = list->Capacity > 0 ? list->Capacity * 2 : 256;
            uint32_t* grown = (uint32_t*)realloc(list->Values, (size_t)newCapacity * sizeof(uint32_t));
            if (grown == nullptr)
                return false;
            list->Values = grown;
            list->Capacity = newCapacity;
        }
        list->Values[list->Count++] = codepoint;
        return true;
    }

    int CompareU32(const void* a, const void* b)
    {
        const uint32_t left = *(const uint32_t*)a;
        const uint32_t right = *(const uint32_t*)b;
        return left < right ? -1 : (left > right ? 1 : 0);
    }

    // ---------------------------------------------------------------------
    // Charset parsing
    // ---------------------------------------------------------------------
    bool ParseCharset(const char* spec, ImSlugFont* font, CodepointList* out, const char** outError)
    {
        *outError = nullptr;
        if (spec == nullptr || spec[0] == '\0')
        {
            *outError = "--chars must not be empty (e.g. --chars a-z,0x20-0x7E)";
            return false;
        }

        const int specLength = (int)strlen(spec);
        int i = 0;
        while (i < specLength)
        {
            while (i < specLength && IsSeparator(spec[i]))
                ++i;
            if (i >= specLength)
                break;
            int start = i;
            while (i < specLength && !IsSeparator(spec[i]))
                ++i;
            const int itemLength = i - start;
            const char* item = spec + start;

            if (itemLength == 3 && strncmp(item, "all", 3) == 0)
            {
                for (uint32_t codepoint = 0; codepoint <= 0x10FFFF; ++codepoint)
                {
                    if (ImSlugGetGlyphIndex(font, codepoint) != 0)
                    {
                        if (!AppendCodepoint(out, codepoint))
                        {
                            *outError = "out of memory while expanding 'all'";
                            return false;
                        }
                    }
                }
                continue;
            }

            // A single codepoint (including the literal '-' character); ranges
            // split on a single dash whose two sides both parse.
            uint32_t first = 0, last = 0;
            if (ParseCodepoint(item, itemLength, &first))
            {
                last = first;
            }
            else
            {
                int dash = -1;
                for (int k = 0; k < itemLength; ++k)
                {
                    if (item[k] == '-')
                    {
                        if (dash >= 0)
                        {
                            dash = -2;
                            break;
                        }
                        dash = k;
                    }
                }
                if (dash < 0 || dash == 0 || dash == itemLength - 1 || !ParseCodepoint(item, dash, &first) ||
                    !ParseCodepoint(item + dash + 1, itemLength - dash - 1, &last))
                {
                    *outError = "invalid --chars item (expected a char, a-z, 0xHH, 0xHH-0xHH or 'all')";
                    return false;
                }
                if (last < first)
                {
                    *outError = "invalid --chars range (end before start)";
                    return false;
                }
            }

            for (uint32_t codepoint = first; codepoint <= last; ++codepoint)
            {
                if (!AppendCodepoint(out, codepoint))
                {
                    *outError = "out of memory while expanding --chars";
                    return false;
                }
            }
        }
        if (out->Count == 0)
        {
            *outError = "--chars selected no codepoints";
            return false;
        }
        qsort(out->Values, (size_t)out->Count, sizeof(uint32_t), CompareU32);
        return true;
    }

    // ---------------------------------------------------------------------
    // File helpers
    // ---------------------------------------------------------------------
    unsigned char* ReadFile(const char* path, int* outSize)
    {
        FILE* file = fopen(path, "rb");
        if (file == nullptr)
            return nullptr;
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, 0, SEEK_SET);
        if (size <= 0)
        {
            fclose(file);
            return nullptr;
        }
        unsigned char* data = (unsigned char*)malloc((size_t)size);
        if (data == nullptr)
        {
            fclose(file);
            return nullptr;
        }
        const size_t read = fread(data, 1, (size_t)size, file);
        fclose(file);
        if (read != (size_t)size)
        {
            free(data);
            return nullptr;
        }
        *outSize = (int)size;
        return data;
    }

    const char* BaseName(const char* path)
    {
        const char* base = path;
        for (const char* p = path; *p != '\0'; ++p)
        {
            if (*p == '/' || *p == '\\')
                base = p + 1;
        }
        return base;
    }

    // ---------------------------------------------------------------------
    // Subcommands
    // ---------------------------------------------------------------------
    int Usage()
    {
        printf("imslug_gen %s - offline generator for .imslug packed assets (M7)\n", IMSLUG_VERSION);
        printf("\n");
        printf("usage:\n");
        printf("  imslug_gen font <font.ttf> --chars <spec> [-o <out.imslug>] [--size-range <min:max>]\n");
        printf("                [--emit-header <out.h>] [--header-symbol <name>]\n");
        printf("  imslug_gen info <asset.imslug>\n");
        printf("  imslug_gen --help | --version\n");
        printf("\n");
        printf("  --chars <spec>       codepoints to bake: comma/space-separated items, each a\n");
        printf("                       literal char, a-z, 0xHH, 0xHH-0xHH, U+HH, U+HH-U+HH, or 'all'\n");
        printf("  --size-range a:b     recorded size range in px (default 8:72; diagnostics only)\n");
        printf("  --emit-header <h>    also emit a C header embedding the asset\n");
        printf("  --header-symbol <s>  symbol base name for --emit-header (default: output base name)\n");
        printf("  -o <file>            output asset path (default: <font base>.imslug)\n");
        return 0;
    }

    bool EmitHeader(const char* headerPath, const char* symbol, const unsigned char* data, int size,
                    const char* sourceName, uint32_t sourceChecksum, uint32_t generatorVersion)
    {
        FILE* file = fopen(headerPath, "wb");
        if (file == nullptr)
        {
            fprintf(stderr, "imslug_gen: cannot write header %s\n", headerPath);
            return false;
        }
        fprintf(file, "// Generated by imslug_gen %s - do not edit by hand.\n", IMSLUG_VERSION);
        fprintf(file, "// .imslug container v1: %d bytes; source %s (checksum 0x%08X); generator version %u.\n", size,
                sourceName, sourceChecksum, generatorVersion);
        fprintf(file, "// Load it with ImSlug::AddFontFromMemoryTTF / imslug_add_font_from_memory_ttf\n");
        fprintf(file, "// (both auto-detect the container by its magic).\n");
        fprintf(file, "#pragma once\n");
        fprintf(file, "#include <stddef.h>\n\n");
        fprintf(file, "static const unsigned char %s_data[] = {", symbol);
        for (int i = 0; i < size; ++i)
        {
            if (i % 16 == 0)
                fprintf(file, "\n    ");
            fprintf(file, "0x%02X,", data[i]);
        }
        fprintf(file, "\n};\n");
        fprintf(file, "static const size_t %s_size = %d;\n", symbol, size);
        const bool ok = ferror(file) == 0;
        fclose(file);
        if (!ok)
        {
            fprintf(stderr, "imslug_gen: write error on %s\n", headerPath);
            return false;
        }
        return true;
    }

    int CommandFont(int argc, char** argv)
    {
        const char* fontPath = nullptr;
        const char* outputPath = nullptr;
        const char* headerPath = nullptr;
        const char* headerSymbol = nullptr;
        const char* charsSpec = nullptr;
        const char* sourceNameOverride = nullptr;
        float sizeMin = 8.0f;
        float sizeMax = 72.0f;

        for (int i = 2; i < argc; ++i)
        {
            const char* arg = argv[i];
            const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
            if (strcmp(arg, "--chars") == 0 && value != nullptr)
            {
                charsSpec = value;
                ++i;
            }
            else if (strcmp(arg, "-o") == 0 && value != nullptr)
            {
                outputPath = value;
                ++i;
            }
            else if (strcmp(arg, "--size-range") == 0 && value != nullptr)
            {
                if (sscanf(value, "%f:%f", &sizeMin, &sizeMax) != 2 || !(sizeMin > 0.0f) || !(sizeMax >= sizeMin) ||
                    sizeMax > 1024.0f)
                {
                    fprintf(stderr, "imslug_gen: invalid --size-range '%s' (expected min:max in px)\n", value);
                    return 1;
                }
                ++i;
            }
            else if (strcmp(arg, "--emit-header") == 0 && value != nullptr)
            {
                headerPath = value;
                ++i;
            }
            else if (strcmp(arg, "--header-symbol") == 0 && value != nullptr)
            {
                headerSymbol = value;
                ++i;
            }
            else if (strcmp(arg, "--source-name") == 0 && value != nullptr)
            {
                // Overrides the stored source name (defaults to the base name
                // of the input path); useful for reproducible scripted builds.
                sourceNameOverride = value;
                ++i;
            }
            else if (fontPath == nullptr)
            {
                fontPath = arg;
            }
            else
            {
                fprintf(stderr, "imslug_gen: unexpected argument '%s'\n", arg);
                return 1;
            }
        }
        if (fontPath == nullptr)
        {
            fprintf(stderr, "imslug_gen: missing input font path (see --help)\n");
            return 1;
        }
        if (charsSpec == nullptr)
        {
            fprintf(stderr, "imslug_gen: --chars is required for 'font' (e.g. --chars 0x20-0x7E)\n");
            return 1;
        }

        char defaultOutput[512];
        if (outputPath == nullptr)
        {
            snprintf(defaultOutput, sizeof(defaultOutput), "%s.imslug", fontPath);
            outputPath = defaultOutput;
        }

        int fontDataSize = 0;
        unsigned char* fontData = ReadFile(fontPath, &fontDataSize);
        if (fontData == nullptr)
        {
            fprintf(stderr, "imslug_gen: cannot read font %s\n", fontPath);
            return 1;
        }
        ImSlugFont* font = ImSlugCreateFontFromMemory(fontData, fontDataSize, 0, true);
        if (font == nullptr)
        {
            fprintf(stderr, "imslug_gen: %s is not a usable TTF/OTF (stb_truetype)\n", fontPath);
            free(fontData);
            return 1;
        }

        CodepointList codepoints = {};
        const char* charsetError = nullptr;
        if (!ParseCharset(charsSpec, font, &codepoints, &charsetError))
        {
            fprintf(stderr, "imslug_gen: %s\n", charsetError != nullptr ? charsetError : "invalid --chars");
            free(codepoints.Values);
            ImSlugDestroyFont(font);
            free(fontData);
            return 1;
        }

        ImSlugAssetOptions options;
        options.SizeMinPx = sizeMin;
        options.SizeMaxPx = sizeMax;
        ImSlugAssetStats stats;
        unsigned char* assetData = nullptr;
        int assetSize = 0;
        char error[512];
        const char* sourceName = sourceNameOverride != nullptr ? sourceNameOverride : BaseName(fontPath);
        if (!ImSlugWritePackedAsset(font, codepoints.Values, codepoints.Count, options, sourceName, fontData,
                                    fontDataSize, &assetData, &assetSize, &stats, error, (int)sizeof(error)))
        {
            fprintf(stderr, "imslug_gen: %s\n", error[0] != '\0' ? error : "asset generation failed");
            free(codepoints.Values);
            ImSlugDestroyFont(font);
            free(fontData);
            return 1;
        }
        ImSlugDestroyFont(font);
        free(codepoints.Values);

        FILE* out = fopen(outputPath, "wb");
        if (out == nullptr)
        {
            fprintf(stderr, "imslug_gen: cannot write %s\n", outputPath);
            free(assetData);
            free(fontData);
            return 1;
        }
        const size_t written = fwrite(assetData, 1, (size_t)assetSize, out);
        fclose(out);
        if (written != (size_t)assetSize)
        {
            fprintf(stderr, "imslug_gen: short write on %s\n", outputPath);
            free(assetData);
            free(fontData);
            return 1;
        }

        ImSlugPackedInfo info;
        char validation[256];
        if (!ImSlugValidatePackedMemory(assetData, assetSize, &info, validation, (int)sizeof(validation)))
        {
            fprintf(stderr, "imslug_gen: internal error: %s\n", validation);
            free(assetData);
            free(fontData);
            return 1;
        }

        printf("imslug_gen: wrote %s (%d bytes)\n", outputPath, assetSize);
        printf("imslug_gen: %d glyph(s), %d codepoint(s), %u curve row(s) + %u band row(s)\n", stats.GlyphCount,
               stats.CodepointCount, info.CurveRows, info.BandRows);
        printf("imslug_gen: source %s (%u bytes, checksum 0x%08X), generator version %u\n", info.SourceName,
               info.SourceSize, info.SourceChecksum, info.GeneratorVersion);
        if (stats.SkippedCodepoints > 0)
            printf("imslug_gen: %d codepoint(s) skipped (no glyph in the font)\n", stats.SkippedCodepoints);
        if (stats.FailedGlyphs > 0)
            printf("imslug_gen: %d glyph(s) skipped (runtime build limits; they fall back at runtime)\n",
                   stats.FailedGlyphs);

        if (headerPath != nullptr)
        {
            char defaultSymbol[512];
            if (headerSymbol == nullptr)
            {
                // Sanitize the output file's base name (without extension).
                char base[512];
                snprintf(base, sizeof(base), "%s", BaseName(outputPath));
                char* dot = strrchr(base, '.');
                if (dot != nullptr)
                    *dot = '\0';
                int w = 0;
                for (int r = 0; base[r] != '\0' && w < (int)sizeof(defaultSymbol) - 8; ++r)
                {
                    const char c = base[r];
                    defaultSymbol[w++] =
                        ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') ? c
                                                                                                                 : '_';
                }
                defaultSymbol[w] = '\0';
                headerSymbol = defaultSymbol;
            }
            if (!EmitHeader(headerPath, headerSymbol, assetData, assetSize, info.SourceName, info.SourceChecksum,
                            info.GeneratorVersion))
            {
                free(assetData);
                free(fontData);
                return 1;
            }
            printf("imslug_gen: wrote header %s (symbol %s_data / %s_size)\n", headerPath, headerSymbol, headerSymbol);
        }

        free(assetData);
        free(fontData);
        return 0;
    }

    int CommandInfo(const char* assetPath)
    {
        int size = 0;
        unsigned char* data = ReadFile(assetPath, &size);
        if (data == nullptr)
        {
            fprintf(stderr, "imslug_gen: cannot read %s\n", assetPath);
            return 1;
        }
        ImSlugPackedInfo info;
        char error[512];
        if (!ImSlugValidatePackedMemory(data, size, &info, error, (int)sizeof(error)))
        {
            fprintf(stderr, "imslug_gen: %s is not a valid .imslug v1 asset: %s\n", assetPath, error);
            free(data);
            return 1;
        }
        printf("%s: .imslug container v1, %u bytes\n", assetPath, info.AssetSize);
        printf("  generator version: %u (this build: %s, %d)\n", info.GeneratorVersion, IMSLUG_VERSION,
               IMSLUG_VERSION_NUM);
        printf("  source font:       %s (%u bytes, FNV-1a checksum 0x%08X)\n", info.SourceName, info.SourceSize,
               info.SourceChecksum);
        printf("  font metrics:      unitsPerEm %u, hhea ascent %d, descent %d\n", info.UnitsPerEm, info.HheaAscent,
               info.HheaDescent);
        printf("  glyphs:            %u entries, %u codepoints\n", info.GlyphCount, info.CodepointCount);
        printf("  pools:             %u curve rows (%llu texels), %u band rows (%llu texels)\n", info.CurveRows,
               (unsigned long long)info.CurveRows * 4096, info.BandRows, (unsigned long long)info.BandRows * 4096);
        printf("  size range:        %.1f .. %.1f px (diagnostics)\n", info.SizeMinPx, info.SizeMaxPx);
        free(data);
        return 0;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return Usage();
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        return Usage();
    if (strcmp(argv[1], "--version") == 0)
    {
        printf("imslug_gen %s (%d)\n", IMSLUG_VERSION, IMSLUG_VERSION_NUM);
        return 0;
    }
    if (strcmp(argv[1], "font") == 0)
        return CommandFont(argc, argv);
    if (strcmp(argv[1], "info") == 0)
    {
        if (argc != 3)
        {
            fprintf(stderr, "imslug_gen: usage: imslug_gen info <asset.imslug>\n");
            return 1;
        }
        return CommandInfo(argv[2]);
    }
    fprintf(stderr, "imslug_gen: unknown command '%s' (see --help)\n", argv[1]);
    return 1;
}
