#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace journal {
class error : public std::runtime_error { using std::runtime_error::runtime_error; };
enum class recovery { reject_incomplete_tail, truncate_incomplete_tail };

// Single-threaded owner. OS-level exclusion prevents simultaneous writers.
// Callbacks must not re-enter the journal. Payloads are opaque bytes.
class log {
public:
    static constexpr std::uint32_t max_payload = 16 * 1024 * 1024;
    explicit log(const std::filesystem::path& path,
                 recovery mode = recovery::reject_incomplete_tail) {
#ifdef _WIN32
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw error("Cannot open/exclusively lock journal");
#else
        file_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (file_ < 0) throw error("Cannot open journal");
        if (::flock(file_, LOCK_EX | LOCK_NB) < 0) { close(); throw error("Journal already locked"); }
#endif
        try { scan({}, mode); } catch (...) { close(); throw; }
    }
    ~log() { close(); }
    log(const log&) = delete;
    log& operator=(const log&) = delete;
    log(log&&) = delete;
    log& operator=(log&&) = delete;

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t repaired_bytes() const noexcept { return repaired_; }

    // Flushes each complete frame before acknowledging it. After any write error,
    // this instance is poisoned: close and reopen to inspect/recover the tail.
    std::uint64_t append(std::string_view payload) {
        if (poisoned_) throw error("Journal requires reopening after an I/O failure");
        if (payload.size() > max_payload) throw error("Payload exceeds 16 MiB");
        if (count_ == std::numeric_limits<std::uint64_t>::max()) throw error("Sequence exhausted");
        std::array<unsigned char,24> h{};
        std::memcpy(h.data(), "DJ01", 4);
        put(h.data()+4, payload.size(), 4); put(h.data()+8, count_+1, 8);
        put(h.data()+16, checksum(h, payload), 4);
        try {
            seek(end_); write(h.data(), h.size()); write(payload.data(), payload.size()); sync();
            end_ += h.size()+payload.size(); return ++count_;
        } catch (...) { poisoned_=true; throw; }
    }
    void replay(const std::function<void(std::uint64_t,std::string_view)>& visitor) {
        if (poisoned_) throw error("Journal requires reopening");
        scan(visitor, recovery::reject_incomplete_tail);
    }
private:
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
#else
    int file_ = -1;
#endif
    std::uint64_t count_=0, end_=0, repaired_=0;
    bool poisoned_=false;
    static void put(unsigned char* p, std::uint64_t v, unsigned n) {
        for (unsigned i=0;i<n;++i) p[i]=static_cast<unsigned char>(v>>(i*8));
    }
    static std::uint64_t get(const unsigned char* p, unsigned n) {
        std::uint64_t v=0; for(unsigned i=0;i<n;++i) v|=std::uint64_t(p[i])<<(i*8); return v;
    }
    static std::uint32_t checksum(const std::array<unsigned char,24>& header, std::string_view data) {
        std::uint32_t crc=0xffffffffu;
        auto update=[&](unsigned char b) {
            crc ^= b; for(int j=0;j<8;++j) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));
        };
        // Covers the format, payload length, sequence and payload. Reserved bytes must be zero.
        for(unsigned i=0;i<16;++i) update(header[i]);
        for(unsigned char b:data) update(b);
        return ~crc;
    }
    void close() noexcept {
#ifdef _WIN32
        if(file_!=INVALID_HANDLE_VALUE) { CloseHandle(file_); file_=INVALID_HANDLE_VALUE; }
#else
        if(file_>=0) { ::close(file_); file_=-1; }
#endif
    }
    void seek(std::uint64_t offset) {
        if(offset>std::uint64_t(std::numeric_limits<std::int64_t>::max())) throw error("File offset overflow");
#ifdef _WIN32
        LARGE_INTEGER p; p.QuadPart=static_cast<LONGLONG>(offset);
        if(!SetFilePointerEx(file_,p,nullptr,FILE_BEGIN)) throw error("Seek failed");
#else
        if(::lseek(file_,static_cast<off_t>(offset),SEEK_SET)<0) throw error("Seek failed");
#endif
    }
    std::uint64_t size() {
#ifdef _WIN32
        LARGE_INTEGER n; if(!GetFileSizeEx(file_,&n)) throw error("Stat failed"); return static_cast<std::uint64_t>(n.QuadPart);
#else
        struct stat st{}; if(::fstat(file_,&st)<0) throw error("Stat failed"); return static_cast<std::uint64_t>(st.st_size);
#endif
    }
    void read(void* buffer,std::size_t bytes) {
        auto* p=static_cast<unsigned char*>(buffer);
        while(bytes) {
#ifdef _WIN32
            DWORD n=0; if(!ReadFile(file_,p,static_cast<DWORD>(bytes),&n,nullptr)) throw error("Read failed");
#else
            auto n=::read(file_,p,bytes); if(n<0 && errno==EINTR) continue; if(n<0) throw error("Read failed");
#endif
            if(n==0) throw error("Unexpected EOF"); p+=n; bytes-=static_cast<std::size_t>(n);
        }
    }
    void write(const void* buffer,std::size_t bytes) {
        auto* p=static_cast<const unsigned char*>(buffer);
        while(bytes) {
#ifdef _WIN32
            DWORD n=0; if(!WriteFile(file_,p,static_cast<DWORD>(bytes),&n,nullptr)) throw error("Write failed");
#else
            auto n=::write(file_,p,bytes); if(n<0 && errno==EINTR) continue; if(n<0) throw error("Write failed");
#endif
            if(n==0) throw error("Zero-length write"); p+=n; bytes-=static_cast<std::size_t>(n);
        }
    }
    void sync() {
#ifdef _WIN32
        if(!FlushFileBuffers(file_)) throw error("Flush failed");
#else
        if(::fsync(file_)<0) throw error("Flush failed");
#endif
    }
    void truncate(std::uint64_t offset) {
        seek(offset);
#ifdef _WIN32
        if(!SetEndOfFile(file_)) throw error("Truncate failed");
#else
        if(::ftruncate(file_,static_cast<off_t>(offset))<0) throw error("Truncate failed");
#endif
        sync();
    }
    void scan(const std::function<void(std::uint64_t,std::string_view)>& visitor,recovery mode) {
        const auto total=size(); std::uint64_t offset=0, sequence=0;
        seek(0);
        while(offset<total) {
            std::array<unsigned char,24> h{};
            const auto remaining=total-offset;
            const auto available=static_cast<std::size_t>(remaining<24?remaining:24);
            read(h.data(),available);
            // A partial magic prefix must still match. Never silently "repair" another file format.
            if(std::memcmp(h.data(),"DJ01",available<4?available:4)!=0) throw error("Invalid frame magic");
            if(remaining<24) break;
            auto length=get(h.data()+4,4), id=get(h.data()+8,8);
            if(length>max_payload || id!=sequence+1 || get(h.data()+20,4)!=0) throw error("Corrupt frame header");
            if(length>remaining-24) break;
            std::string payload(static_cast<std::size_t>(length),'\0');
            read(payload.data(),payload.size());
            if(get(h.data()+16,4)!=checksum(h,payload)) throw error("Frame checksum mismatch");
            if(visitor) visitor(id,payload);
            ++sequence; offset+=24+length;
        }
        if(offset!=total) {
            if(mode==recovery::reject_incomplete_tail) throw error("Incomplete final frame; explicit recovery required");
            truncate(offset); repaired_=total-offset;
        }
        count_=sequence; end_=offset;
    }
};
} // namespace journal
