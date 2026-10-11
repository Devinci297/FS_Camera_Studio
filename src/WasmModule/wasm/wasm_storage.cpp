#include "wasm_storage.hpp"

#include <MSFS/MSFS.h>
#include <MSFS/MSFS_IO.h>

#include <memory>

namespace fscs::host {

namespace {

// Each operation owns its buffer until the SDK's completion callback; callbacks run on the module's thread.
struct WriteOp {
    std::string bytes;
    std::function<void(bool, std::string)> done;
    FsIOFile file = FS_IO_ERROR_FILE;
};

struct ReadOp {
    std::string buffer;
    std::function<void(std::optional<std::string>)> done;
};

void finishWrite(WriteOp* op, bool ok, const std::string& err) {
    std::unique_ptr<WriteOp> owned(op);
    if (op->file != FS_IO_ERROR_FILE) fsIOClose(op->file);
    op->done(ok, err);
}

void onWritten(FsIOFile, const char*, int, int bytesWritten, void* user) {
    auto* op = static_cast<WriteOp*>(user);
    const bool ok = bytesWritten == static_cast<int>(op->bytes.size()) && !fsIOHasError(op->file);
    finishWrite(op, ok, ok ? std::string() : "The simulator could not write Camera Studio's save file.");
}

void onOpenedForWrite(FsIOFile file, void* user) {
    auto* op = static_cast<WriteOp*>(user);
    op->file = file;
    if (fsIOHasError(file)) {
        finishWrite(op, false, "The simulator could not open Camera Studio's save file.");
        return;
    }
    if (fsIOWrite(file, op->bytes.data(), 0, static_cast<int>(op->bytes.size()), &onWritten, op) != FsIOErr_Success)
        finishWrite(op, false, "The simulator refused the save.");
}

void finishRead(ReadOp* op, FsIOFile file, std::optional<std::string> data) {
    std::unique_ptr<ReadOp> owned(op);
    if (file != FS_IO_ERROR_FILE) fsIOClose(file);
    op->done(std::move(data));
}

void onRead(FsIOFile file, char*, int, int bytesRead, void* user) {
    auto* op = static_cast<ReadOp*>(user);
    if (fsIOHasError(file) || bytesRead < 0) {
        finishRead(op, file, std::nullopt);
        return;
    }
    op->buffer.resize(static_cast<std::size_t>(bytesRead));
    finishRead(op, file, std::move(op->buffer));
}

void onOpenedForRead(FsIOFile file, void* user) {
    auto* op = static_cast<ReadOp*>(user);
    if (fsIOHasError(file)) {
        finishRead(op, file, std::nullopt);  // missing file: a normal first run
        return;
    }
    const unsigned long long size = fsIOGetFileSize(file);
    if (size == 0) {
        finishRead(op, file, std::string());
        return;
    }
    if (size > profiles::kMaxStoreBytes) {
        finishRead(op, file, std::nullopt);
        return;
    }
    op->buffer.resize(static_cast<std::size_t>(size));
    if (fsIORead(file, op->buffer.data(), 0, static_cast<int>(size), &onRead, op) != FsIOErr_Success) finishRead(op, file, std::nullopt);
}

}  // namespace

void WasmStorage::writeFile(const std::string& path, std::string bytes, std::function<void(bool, std::string)> done) {
    auto* op = new WriteOp{std::move(bytes), std::move(done)};
    const FsIOFile f = fsIOOpen(path.c_str(), FsIOOpenFlag_WRONLY | FsIOOpenFlag_CREAT | FsIOOpenFlag_TRUNC, &onOpenedForWrite, op);
    if (f == FS_IO_ERROR_FILE) {
        std::unique_ptr<WriteOp> owned(op);
        op->done(false, "The simulator could not open Camera Studio's save file.");
    }
}

void WasmStorage::readFile(const std::string& path, std::function<void(std::optional<std::string>)> done) {
    auto* op = new ReadOp{{}, std::move(done)};
    const FsIOFile f = fsIOOpen(path.c_str(), FsIOOpenFlag_RDONLY, &onOpenedForRead, op);
    if (f == FS_IO_ERROR_FILE) {
        std::unique_ptr<ReadOp> owned(op);
        op->done(std::nullopt);
    }
}

}  // namespace fscs::host
