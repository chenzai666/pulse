#pragma once
#include "../ipc/protocol.h"
#include "change_tracking.h"
#include "folder_size_protocol.h"
#include "index_endpoints.h"
#include <windows.h>
#include <string>
#include <vector>

namespace pulse::index {
inline constexpr uint32_t kFeedRequest = 7, kFeedResponse = 107, kFeedMagic = 0x58444950;
struct FileFeedPage {
    uint64_t epoch = 0, sequence = 0, next = 0;
    bool ready = false, done = false, gap = false;
    std::vector<ChangeRecord> records;
};
inline void PutFeedPage(ipc::PayloadWriter& w, const FileFeedPage& page) {
    w.PutU32(1); w.PutU64(page.epoch); w.PutU64(page.sequence); w.PutU64(page.next);
    w.PutU32((page.ready ? 1u : 0u) | (page.done ? 2u : 0u) | (page.gap ? 4u : 0u));
    w.PutU32(static_cast<uint32_t>(page.records.size()));
    for (const auto& record : page.records) {
        w.PutU64(record.id); w.PutU64(record.file_id); w.PutU64(record.time);
        w.PutU32(static_cast<uint32_t>(record.kind)); w.PutU32(record.is_dir ? 1u : 0u);
        w.PutString(record.path); w.PutString(record.old_path);
    }
}
// Dedicated connection, used only off the window thread. The event interrupts
// both connect/retry and overlapped transfers when scope or process changes.
class IndexFeedConnection {
public:
    explicit IndexFeedConnection(HANDLE cancel):cancel_(cancel) {}
    ~IndexFeedConnection() { Close(); }
    bool FolderSizes(const std::vector<std::wstring>& paths, std::vector<IndexedFolderSize>& values) {
        if (paths.empty() || paths.size() > kFolderSizeBatch) return false;
        if (pipe_ == INVALID_HANDLE_VALUE) {
            wchar_t override_name[256]{};
            const auto n = GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", override_name, 256);
            const std::wstring name = n && n < 256 ? override_name : kPipeName;
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ == INVALID_HANDLE_VALUE) return false;
#ifdef PULSE_CUSTOM_EDITION
            if (!agent::AuthorizedServer(pipe_)) { Close(); return false; }
#endif
        }
        ipc::PayloadWriter request;
        request.PutU32(1); request.PutU32(static_cast<uint32_t>(paths.size()));
        for (const auto& path : paths) { if (path.size() > 32768) return false; request.PutString(path); }
        if (request.data().size() > 256 * 1024) return false;
        ipc::MsgHeader header{}; header.magic = kFeedMagic; header.type = kFolderSizeRequest;
        header.payload_size = static_cast<uint32_t>(request.data().size());
        if (!Transfer(&header, sizeof(header), true, 750) ||
            !Transfer(const_cast<uint8_t*>(request.data().data()), header.payload_size, true, 750)) { Close(); return false; }
        const auto deadline = GetTickCount64() + 750;
        for (;;) {
            if (GetTickCount64() >= deadline || (cancel_ && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0)) { Close(); return false; }
            if (!Transfer(&header, sizeof(header), false, 750) || header.magic != kFeedMagic || header.payload_size > 65536) { Close(); return false; }
            std::vector<uint8_t> bytes(header.payload_size);
            if (!Transfer(bytes.data(), header.payload_size, false, 750)) { Close(); return false; }
            if (header.type == 101) continue;
            if (header.type != kFolderSizeResponse) { Close(); return false; }
            ipc::PayloadReader reader(bytes.data(), bytes.size());
            if (!ReadFolderSizes(reader, paths.size(), values)) { Close(); return false; }
            return true;
        }
    }
    bool Request(bool changes, const std::wstring& root, uint64_t epoch, uint64_t cursor, FileFeedPage& page) {
        if (pipe_ == INVALID_HANDLE_VALUE) {
            wchar_t override_name[256]{};
            const auto n = GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE",override_name,256);
            const std::wstring name = n && n < 256 ? override_name : kPipeName;
            pipe_ = CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            if (pipe_ == INVALID_HANDLE_VALUE) return false;
#ifdef PULSE_CUSTOM_EDITION
            if (!agent::AuthorizedServer(pipe_)) { Close(); return false; }
#endif
        }
        ipc::PayloadWriter request;
        request.PutU32(1); request.PutU32(changes ? 1u : 0u); request.PutString(root); request.PutU64(epoch); request.PutU64(cursor);
        ipc::MsgHeader header{}; header.magic=kFeedMagic; header.type=kFeedRequest; header.payload_size=static_cast<uint32_t>(request.data().size());
        if (!Transfer(&header,sizeof(header),true) || !Transfer(const_cast<uint8_t*>(request.data().data()),header.payload_size,true)) { Close(); return false; }
        for (;;) {
            if (!Transfer(&header,sizeof(header),false) || header.magic!=kFeedMagic || header.payload_size>8*1024*1024) { Close(); return false; }
            std::vector<uint8_t> bytes(header.payload_size);
            if (!Transfer(bytes.data(),header.payload_size,false)) { Close(); return false; }
            // Hosts also send status broadcasts to connected clients.
            if (header.type == 101) continue;
            if (header.type != kFeedResponse) { Close(); return false; }
            ipc::PayloadReader r(bytes.data(),bytes.size()); uint32_t version=0,flags=0,count=0;
            if(!r.GetU32(version)||version!=1||!r.GetU64(page.epoch)||!r.GetU64(page.sequence)||!r.GetU64(page.next)||!r.GetU32(flags)||!r.GetU32(count)||count>1024) return false;
            page.ready=(flags&1)!=0; page.done=(flags&2)!=0; page.gap=(flags&4)!=0; page.records.clear();
            for(uint32_t i=0;i<count;++i) {
                ChangeRecord record; uint32_t kind=0,directory=0;
                if(!r.GetU64(record.id)||!r.GetU64(record.file_id)||!r.GetU64(record.time)||!r.GetU32(kind)||kind>5||!r.GetU32(directory)||!r.GetString(record.path)||!r.GetString(record.old_path)) return false;
                record.kind=static_cast<ChangeKind>(kind); record.is_dir=directory!=0; page.records.push_back(std::move(record));
            }
            return r.remaining()==0;
        }
    }
private:
    bool Transfer(void* value,DWORD count,bool write, DWORD timeout = 5000) {
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr); bool ok=true;
        if (!event) return false;
        auto* bytes=static_cast<uint8_t*>(value);
        while(count) {
            if (cancel_ && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) { ok=false; break; }
            OVERLAPPED operation{}; operation.hEvent=event; ResetEvent(event); DWORD done=0;
            BOOL result=write ? WriteFile(pipe_,bytes,count,&done,&operation) : ReadFile(pipe_,bytes,count,&done,&operation);
            if(!result && GetLastError()==ERROR_IO_PENDING) {
                HANDLE events[]{event,cancel_};
                const DWORD wait=WaitForMultipleObjects(cancel_ ? 2u : 1u,events,FALSE,timeout);
                if(wait!=WAIT_OBJECT_0) { CancelIoEx(pipe_,&operation); GetOverlappedResult(pipe_,&operation,&done,TRUE); ok=false; break; }
                result=GetOverlappedResult(pipe_,&operation,&done,FALSE);
            }
            if(!result || !done) {ok=false;break;} bytes+=done;count-=done;
        }
        CloseHandle(event);return ok;
    }
    void Close() {if(pipe_!=INVALID_HANDLE_VALUE) {CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;}}
    HANDLE cancel_=nullptr,pipe_=INVALID_HANDLE_VALUE;
};
}
