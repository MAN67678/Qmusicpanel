// QPanel — entry point.
//   qpanel [dir] [options]
// Zero-config: give it a directory, it starts playing. No daemons.
// Settings come from config.ini (see --print-config); flags override the file.
#include "app/App.hpp"
#include "app/Config.hpp"
#include "media/ChafaRender.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace qp {

namespace {

void setArtEngine(ArtConfig& ac, const std::string& v) {
    if (v == "block" || v == "half" || v == "builtin" || v == "ascii") ac.engine = ArtEngine::Block;
    else ac.engine = ArtEngine::Chafa;
}

void setSymbols(ArtConfig& ac, const std::string& v) {
    if (v == "block" || v == "half") ac.symbols = ChafaSymbols::Block;
    else if (v == "sextant") ac.symbols = ChafaSymbols::Sextant;
    else if (v == "braille") ac.symbols = ChafaSymbols::Braille;
    else if (v == "ascii") ac.symbols = ChafaSymbols::Ascii;
    else if (v == "ramp") ac.symbols = ChafaSymbols::Ramp;
    else ac.symbols = ChafaSymbols::All;
}

void setColors(ArtConfig& ac, const std::string& v) {
    if (v == "256") ac.mode = ChafaMode::Indexed256;
    else if (v == "16") ac.mode = ChafaMode::Indexed16;
    else ac.mode = ChafaMode::TrueColor;
}

void setDither(ArtConfig& ac, const std::string& v) {
    if (v == "ordered") ac.dither = ChafaDither::Ordered;
    else if (v == "diffusion" || v == "fs" || v == "atkinson") ac.dither = ChafaDither::Diffusion;
    else ac.dither = ChafaDither::None;
}

std::string defaultDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string d = fs::current_path(ec).string();
    for (const char* cand : { "/home/man1/Music", "/home/man1/Downloads" }) {
        std::error_code e2;
        if (fs::exists(cand, e2) && fs::is_directory(cand, e2)) { d = cand; break; }
    }
    return d;
}

} // namespace

int buildMain(int argc, char** argv) {
    // ---- config first, so flags can override it --------------------------
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--config" || a == "-c") && i + 1 < argc) configPath = argv[++i];
    }
    if (configPath.empty()) configPath = Config::defaultPath();

    std::vector<std::string> warnings;
    cfg().load(configPath, &warnings);          // a missing file is fine
    for (auto& w : warnings) std::fprintf(stderr, "qpanel: %s\n", w.c_str());

    std::string dir;
    int showHelp = 0, printConfig = 0, writeConfig = 0;
    bool forceWrite = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "qpanel: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--config" || a == "-c") { (void)need("--config"); }   // applied above
        else if (a == "--theme") cfg().theme = need("--theme");
        else if (a == "--icons") cfg().icons = need("--icons");
        else if (a == "--art") setArtEngine(cfg().art, need("--art"));
        else if (a == "--chafa-symbols") setSymbols(cfg().art, need("--chafa-symbols"));
        else if (a == "--chafa-colors") setColors(cfg().art, need("--chafa-colors"));
        else if (a == "--chafa-dither") setDither(cfg().art, need("--chafa-dither"));
        else if (a == "--print-config") printConfig = 1;
        else if (a == "--write-config") writeConfig = 1;
        else if (a == "--force") forceWrite = true;
        else if (a == "--help" || a == "-h") showHelp = 1;
        else if (a.rfind("--", 0) != 0 && dir.empty()) dir = a;
        else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "qpanel: unknown option %s\n", a.c_str());
            return 2;
        }
    }

    if (showHelp) {
        std::printf(
            "qpanel — terminal music player (standalone, 60fps TUI)\n"
            "\n"
            "  qpanel [DIR] [options]\n"
            "  --config PATH    config file (default ~/.config/qpanel/config.ini)\n"
            "  --print-config   print the default config file (commented)\n"
            "  --write-config   write it to the config path (--force to overwrite)\n"
            "  --theme  hyper | catppuccin-mocha | catppuccin-macchiato |\n"
            "           catppuccin-latte | tokyo-night | rose-pine | gruvbox-material\n"
            "  --icons  unicode (default) | ascii | nerd\n"
            "  --art    chafa (default) | block   cover-art renderer\n"
            "  --chafa-symbols  block (default) | ascii | ramp | sextant | braille | all\n"
            "  --chafa-colors   truecolor (default) | 256 | 16\n"
            "  --chafa-dither   none (default) | ordered | diffusion\n"
            "\n"
            "keys: space=play/pause  arrows=seek/tree nav  enter=play/expand\n"
            "      s=shuffle  r=repeat  +/-=volume  q=quit\n"
            "      ctrl+r (or ctrl+shift+r) = reload config.ini live\n"
            "mouse: click tree (single=select, double=expand/play)  scrub=drag seek\n");
        return 0;
    }

    if (printConfig) {
        std::fputs(Config::skeleton().c_str(), stdout);
        return 0;
    }

    if (writeConfig) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!forceWrite && fs::exists(configPath, ec)) {
            std::fprintf(stderr, "qpanel: %s already exists (use --force to overwrite)\n", configPath.c_str());
            return 2;
        }
        fs::path p(configPath);
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        std::ofstream out(configPath);
        if (!out) {
            std::fprintf(stderr, "qpanel: cannot write %s\n", configPath.c_str());
            return 2;
        }
        out << Config::skeleton();
        std::fprintf(stderr, "qpanel: wrote %s\n", configPath.c_str());
        return 0;
    }

    if (dir.empty()) dir = defaultDir();

    // hand the art section to the renderer (it owns the chafa settings)
    artConfig() = cfg().art;
    if (artConfig().engine == ArtEngine::Chafa && !chafaAvailable())
        std::fprintf(stderr, "qpanel: chafa unavailable at build time — using built-in art\n");

    App app(dir);
    app.run();
    return 0;
}

} // namespace qp

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    return qp::buildMain(argc, argv);
}
