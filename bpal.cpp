#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QIODevice>
#include <QImage>
#include <QtGlobal>

#ifdef LIBOXI
#include "oxi.h"
#else
#include <boost/process.hpp>

namespace bp = boost::process;
#endif

class Error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Chunk {
    std::uint32_t type;
    std::vector<unsigned char> data;
};

struct BPalEntry {
    std::uint32_t palette;
    std::uint32_t requested;
    std::uint32_t id;
};

struct BlorbData {
    std::uint32_t size = 0;
    std::vector<Chunk> chunks;
    std::optional<std::vector<unsigned char>> exec;
    std::map<std::uint32_t, Chunk> picts;
    std::vector<BPalEntry> bpal;
};

static constexpr std::uint32_t be32(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d)
{
    return (a << 24) |
           (b << 16) |
           (c <<  8) |
           (d <<  0);
}

static constexpr std::uint32_t TypeID(const char (&id)[5])
{
    return be32(static_cast<unsigned char>(id[0]),
                static_cast<unsigned char>(id[1]),
                static_cast<unsigned char>(id[2]),
                static_cast<unsigned char>(id[3]));
}

static std::string idstr(std::uint32_t id)
{
    auto aschar = [](unsigned char c) -> char { return std::isprint(c) ? c : ' '; };

    return {
        aschar(id >> 24),
        aschar((id >> 16) & 0xff),
        aschar((id >>  8) & 0xff),
        aschar(id & 0xff)
    };
}

static QImage qimage_from_data(const std::span<unsigned char> data)
{
    QImage image;

    if (!image.loadFromData(data.data(), data.size(), "PNG")) {
        throw Error("unable to load PNG");
    }

    return image;
}

static std::vector<unsigned char> compress_png(const std::span<unsigned char> png)
{
#ifdef LIBOXI
    auto compressed = optimize_png(png.data(), png.size());
    if (compressed.data == nullptr) {
        throw Error("unable to compress image");
    }

    auto freefn = [](unsigned char *p) { std::free(p); };
    std::unique_ptr<unsigned char, decltype(freefn)> free_png(compressed.data, freefn);

    return {compressed.data, compressed.data + compressed.size};
#else
    bp::ipstream out;
    bp::opstream in;

    bp::child c("/usr/bin/oxipng", "-o6", "-q", "--stdout", "-", bp::std_in < in, bp::std_out > out);

    in.write(reinterpret_cast<const char *>(png.data()), png.size());
    in.flush();
    in.pipe().close();

    c.wait();
    if (c.exit_code() != 0) {
        throw Error(std::format("optipng exited {}", c.exit_code()));
    }

    return {std::istreambuf_iterator<char>(out), std::istreambuf_iterator<char>()};
#endif
}

static std::vector<unsigned char> convert_palette(QImage apal_image, const QImage &palette)
{
    if (palette.format() != QImage::Format_Indexed8) {
        throw Error("palette source not indexed");
    }

    auto dst = apal_image.colorTable();
    auto src = palette.colorTable();

    for (int i = 2; i < qMin(src.size(), dst.size()); i++) {
        dst[i] = src[i];
    }

    apal_image.setColorTable(dst);

    QByteArray ba;
    QBuffer buffer(&ba);

    if (!buffer.open(QIODevice::WriteOnly)) {
        throw Error("unable to open QBuffer");
    }

    if (!apal_image.save(&buffer, "PNG", 100)) {
        throw Error("unable to store image as PNG");
    }

    return {ba.begin(), ba.end()};
}

std::set<std::uint32_t> find_apal_images(const std::span<Chunk> chunks)
{
    auto apal = std::find_if(chunks.begin(), chunks.end(), [](const auto &chunk) {
        return chunk.type == TypeID("APal");
    });

    if (apal == chunks.end()) {
        throw Error("no APal chunk found");
    }

    if (apal->data.size() % 4 != 0) {
        throw Error(std::format("invalid APal size: {}", apal->data.size()));
    }

    std::set<std::uint32_t> apal_images;

    for (std::size_t i = 0; i < apal->data.size(); i += 4) {
        auto id = be32(apal->data[i + 0], apal->data[i + 1], apal->data[i + 2], apal->data[i + 3]);

        apal_images.insert(id);
    }

    return apal_images;
}

static BlorbData load_blorb_data(const std::string &filename)
{
    BlorbData blorb_data;

    std::ifstream file(filename, std::ios::binary);
    file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);

    auto read32 = [&file]() -> std::uint32_t {
        unsigned char a = file.get();
        unsigned char b = file.get();
        unsigned char c = file.get();
        unsigned char d = file.get();

        return be32(a, b, c, d);
    };

    if (read32() != TypeID("FORM")) {
        throw Error("not a blorb");
    }

    blorb_data.size = read32();

    if (read32() != TypeID("IFRS") || read32() != TypeID("RIdx")) {
        throw Error("not a blorb");
    }

    // Map offsets to IDs.
    std::map<std::streamoff, std::uint32_t> ids;
    auto n = read32();
    auto num = read32();
    if (n != ((num * 12) + 4)) {
        throw Error("RIdx mismatch");
    }

    // Converted IDs start at 1000, unless the Blorb file contains
    // larger IDs, at which point the converted IDs start at the largest
    // ID plus one.
    std::uint32_t converted_id = 1000;

    for (size_t i = 0; i < num; i++) {
        auto usage = read32();
        auto number = read32();
        auto start = read32();

        if (usage != TypeID("Pict")) {
            throw Error(std::format("unknown resource usage: {:x} ({})", usage, idstr(usage)));
        }

        converted_id = std::max(converted_id, number + 1);

        // This is legal but neither Arthur nor Zork Zero does it.
        if (!ids.emplace(start, number).second) {
            throw Error(std::format("duplicate offset {:x} for id {}", start, number));
        }
    }

    while (file.tellg() < blorb_data.size + 8) {
        std::streamoff pos = file.tellg();
        auto chunktype = read32();

        auto size = read32();
        std::vector<unsigned char> chunk(size);
        file.read(reinterpret_cast<char *>(chunk.data()), size);
        if (size % 2 == 1) {
            file.get();
        }

        switch (chunktype) {
        case TypeID("IFhd"): case TypeID("SNam"): case TypeID("(c) "): case TypeID("AUTH"):
        case TypeID("RelN"): case TypeID("Reso"): case TypeID("APal"):
            blorb_data.chunks.emplace_back(chunktype, std::move(chunk));
            break;
        case TypeID("PNG "): case TypeID("Rect"): {
            try {
                auto id = ids.at(pos);
                blorb_data.picts.emplace(id, Chunk{chunktype, std::move(chunk)});
            } catch (const std::out_of_range &) {
                throw Error(std::format("found {:x} ({}) chunk at offset {:x}, but no RIdx entries reference it", chunktype, idstr(chunktype), pos));
            }
            break;
        }
        case TypeID("BPal"):
            throw Error("this file already has a BPal chunk");
        default:
            throw Error(std::format("unknown chunk: {:x} ({}) @{:x}", chunktype, idstr(chunktype), pos));
        }
    }

    std::map<std::uint32_t, QImage> apal_images;
    for (const auto apal_id : find_apal_images(blorb_data.chunks)) {
        try {
            apal_images.insert({apal_id, qimage_from_data(blorb_data.picts.at(apal_id).data)});
        } catch (const std::out_of_range &) {
            throw Error(std::format("APal references image {}, which does not exist", apal_id));
        }
    }

    if (apal_images.empty()) {
        throw Error("no APal images found");
    }

    decltype(blorb_data.picts) converted_picts;

    std::map<std::vector<unsigned char>, std::uint32_t> image_cache;

    std::cout << "Converting images...\n";
    for (auto &[id, chunk] : blorb_data.picts) {
        if (chunk.type == TypeID("PNG ") && !apal_images.contains(id)) {
            QImage palette_image = qimage_from_data(chunk.data);
            for (const auto &[apal_id, apal_image] : apal_images) {
                auto converted = convert_palette(apal_image, palette_image);
                auto [it, inserted] = image_cache.try_emplace(converted, converted_id);
                if (inserted) {
                    converted_picts.emplace(converted_id++, Chunk{chunk.type, std::move(converted)});
                }

                blorb_data.bpal.emplace_back(id, apal_id, it->second);
            }
        }
    }

    std::cout << "Compressing images...\n";
    for (auto &[_, chunk] : converted_picts) {
        if (chunk.type == TypeID("PNG ")) {
            chunk.data = compress_png(chunk.data);
        }
    }

    blorb_data.picts.merge(converted_picts);

    return blorb_data;
}

static void write_blorb(const std::string &filename, const BlorbData &blorb_data)
{
    std::ofstream file(filename, std::ios::binary);
    file.exceptions(std::ofstream::badbit | std::ofstream::failbit | std::ofstream::eofbit);

    auto write32 = [&file](std::uint32_t n) {
        file.put(n >> 24);
        file.put((n >> 16) & 0xff);
        file.put((n >>  8) & 0xff);
        file.put(n & 0xff);
    };

    file << "FORM....IFRSRIdx";
    std::uint32_t ridx_size = blorb_data.picts.size();
    if (blorb_data.exec.has_value()) {
        ridx_size++;
    }
    write32(4 + (ridx_size * 12));
    write32(ridx_size);

    for (const auto &[id, _] : blorb_data.picts) {
        file << "Pict";
        write32(id);
        write32(0); // placeholder
    }

    if (blorb_data.exec.has_value()) {
        file << "Exec";
        write32(0);
        write32(0);
    }

    auto write_chunk = [&file, &write32](const Chunk &chunk) {
        write32(chunk.type);
        write32(chunk.data.size());
        file.write(reinterpret_cast<const char *>(chunk.data.data()), chunk.data.size());
        if (chunk.data.size() % 2 == 1) {
            file.put(0);
        }
    };

    for (const auto &chunk : blorb_data.chunks) {
        write_chunk(chunk);
    }

    std::vector<std::streamoff> offsets;
    for (const auto &[_, chunk] : blorb_data.picts) {
        offsets.push_back(file.tellp());
        write_chunk(chunk);
    }

    if (blorb_data.exec.has_value()) {
        offsets.push_back(file.tellp());
        write_chunk(Chunk{TypeID("ZCOD"), *blorb_data.exec});
    }

    if (blorb_data.bpal.empty()) {
        throw Error("BPal chunk is empty");
    }

    file.write("BPal", 4);
    write32(blorb_data.bpal.size() * 4 * 3);

    for (const auto &bpal_entry : blorb_data.bpal) {
        write32(bpal_entry.palette);
        write32(bpal_entry.requested);
        write32(bpal_entry.id);
    }

    for (const auto &[i, offset] : std::views::enumerate(offsets)) {
        file.seekp(0x20 + (i * 12));
        write32(offset);
    }

    file.seekp(0, std::ios::end);
    std::streamoff size = file.tellp();
    file.seekp(4);
    write32(size - 8);
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: bpal blorb.blb [story.z6]\n";
        std::exit(1);
    }

    std::optional<std::vector<unsigned char>> exec;

    if (argc == 3) {
        try {
            std::ifstream file(argv[2], std::ios::binary);
            file.exceptions(std::ifstream::badbit | std::ifstream::failbit | std::ifstream::eofbit);
            exec.emplace();
            exec->assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        } catch (const std::ios_base::failure &e) {
            std::cerr << std::format("error processing {}: {}\n", argv[2], e.code().message());
            std::exit(1);
        }
    }

    try {
        auto blorb_data = load_blorb_data(argv[1]);
        blorb_data.exec = exec;
        write_blorb("out.blb", blorb_data);
    } catch (const Error &e) {
        std::cerr << "error: " << e.what() << std::endl;
        std::exit(1);
    } catch (const std::ios_base::failure &e) {
        std::cerr << std::format("error processing {}: {}\n", argv[1], e.code().message());
        std::exit(1);
    }

    return 0;
}
