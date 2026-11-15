// falcon_pack: puts files at the end of an executable (core/bundle.h), so it carries them instead
// of folders beside it. The release build packs the UI's page bundles and the engine's content
// into falcon.exe this way.
//
//   falcon_pack <exe> [<prefix>=<dir>]...   the files of each dir under prefix ("ui=C:/src/ui"),
//                                           in place of what the executable carried
//   falcon_pack <exe> --list                what it carries
//
// With no folders it takes the bundle off again.

#include "core/bundle.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static fs::path utf8Path(const char* s) { return fs::path(std::u8string(s, s + std::strlen(s))); }

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: falcon_pack <exe> [<prefix>=<dir>]... | --list\n");
        return 2;
    }
    const fs::path exe = utf8Path(argv[1]);
    if (argc == 3 && std::strcmp(argv[2], "--list") == 0) {
        fx::bundle::Bundle b;
        if (!b.open(exe)) {
            std::printf("%s carries no files\n", argv[1]);
            return 0;
        }
        uint64_t total = 0;
        for (const fx::bundle::File& f : b.files()) {
            std::printf("%12llu  %s\n", static_cast<unsigned long long>(f.size), f.path.c_str());
            total += f.size;
        }
        std::printf("%zu files, %llu bytes after a %llu-byte program (id %016llx)\n", b.files().size(),
                    static_cast<unsigned long long>(total), static_cast<unsigned long long>(b.programSize()),
                    static_cast<unsigned long long>(b.id()));
        return 0;
    }
    std::vector<fx::bundle::Source> files;
    for (int i = 2; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (!eq) {
            std::fprintf(stderr, "falcon_pack: expected <prefix>=<dir>, got %s\n", argv[i]);
            return 2;
        }
        const fs::path dir = utf8Path(eq + 1);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            std::fprintf(stderr, "falcon_pack: %s is not a folder\n", eq + 1);
            return 1;
        }
        fx::bundle::addDirectory(files, dir, std::string(static_cast<const char*>(argv[i]), eq));
    }
    std::string error;
    if (!fx::bundle::write(exe, exe, files, error)) {
        std::fprintf(stderr, "falcon_pack: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
