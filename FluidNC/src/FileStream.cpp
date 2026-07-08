// Copyright (c) 2021 -	Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "FileStream.h"
#include "Machine/MachineConfig.h"  // config->

std::string FileStream::path() {
    return _fpath.c_str();
}

std::string FileStream::name() {
    return path();
}

int FileStream::available() {
    return size() - position();
}

int FileStream::read() {
    char   data;
    size_t res = fread(&data, 1, 1, _fd);
    return res == 1 ? data : -1;
}

int FileStream::peek() {
    return -1;
}

void FileStream::flush() {}

size_t FileStream::read(char* buffer, size_t length) {
    return fread(buffer, 1, length, _fd);
}

size_t FileStream::write(uint8_t c) {
    return FileStream::write(&c, 1);
}

size_t FileStream::write(const uint8_t* buffer, size_t length) {
    return fwrite(buffer, 1, length, _fd);
}

size_t FileStream::size() {
    return _size;
}

size_t FileStream::position() {
    return ftell(_fd);
}

void FileStream::setup(const char* mode) {
    _fd = fopen(_fpath.c_str(), mode);

    if (!_fd) {
        bool opening = strcmp(mode, "w");
        log_verbose("Cannot " << (opening ? "open" : "create") << " file " << _fpath.c_str());
        throw opening ? Error::FsFailedOpenFile : Error::FsFailedCreateFile;
    }
    // Newlib's default stdio buffer (128B) makes FATFS read-modify-write
    // sectors on nearly every fwrite. A 4KB buffer coalesces the odd-sized
    // web-upload chunks into whole-sector sequential writes — the difference
    // between minutes and tens of seconds for a multi-MB pattern upload.
    if (strchr(mode, 'w') || strchr(mode, 'a')) {
        setvbuf(_fd, nullptr, _IOFBF, 4096);
    }
    // error_code overload: on a marginal SD card fopen can succeed while the
    // following stat fails; the throwing overload then raises a
    // filesystem_error that several callers only catch as Error -> uncaught
    // exception -> panic.  A 0 size on error is harmless (size() is advisory).
    std::error_code ec;
    _size = stdfs::file_size(_fpath, ec);
    if (ec) {
        _size = 0;
    }
}

FileStream::FileStream(const char* filename, const char* mode, const char* fs) : Channel(filename), _fpath(filename, fs), _mode(mode) {
    setup(mode);
}

FileStream::FileStream(FluidPath fpath, const char* mode) : Channel("file"), _mode(mode) {
    std::swap(_fpath, fpath);
    setup(mode);
}

void FileStream::set_position(size_t pos) {
    fseek(_fd, pos, SEEK_SET);
}

void FileStream::save() {
    _saved_position = position();
    fclose(_fd);
    _fd = nullptr;
}

void FileStream::restore() {
    _fd = fopen(_fpath.c_str(), _mode);
    if (_fd) {
        fseek(_fd, _saved_position, SEEK_SET);
    } else {
        // XXX need to unwind the job stack somehow
    }
}

FileStream::~FileStream() {
    if (_fd) {
        fclose(_fd);
    }
}
