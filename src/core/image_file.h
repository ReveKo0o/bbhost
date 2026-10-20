#pragma once

// The image files the dumps write: an 8-bit RGB picture, a row at a time. A
// path ending in ".png" is written as PNG (deflated: a fifth of the size, and
// any image viewer opens it - F12 with BBHOST_F12_PNG=1), any other as binary
// PPM (P6), which costs nothing to write and the development tools read.

#include <cstdint>
#include <memory>
#include <string>

class ImageFile {
public:
    ImageFile();
    ~ImageFile();
    ImageFile(const ImageFile&) = delete;
    ImageFile& operator=(const ImageFile&) = delete;

    // Creates the file and writes its header; false when it cannot be created.
    bool open(const std::string& path, std::uint32_t width, std::uint32_t height);
    // The next row, top first: width * 3 bytes of R, G, B.
    bool row(const std::uint8_t* rgb);
    // Ends the file; false when a write failed or rows are missing.
    bool close();

private:
    struct State;
    std::unique_ptr<State> s_;
};
