#include "native_files.hpp"
#include "quant/storage/file_integrity.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
namespace lexongine::files {
namespace {
struct Fd{int value=-1;~Fd(){if(value>=0){static_cast<void>(::close(value));}}};
}
Result<Sha256> digest(std::span<const std::byte> bytes) noexcept{return guarded([&]()->Result<Sha256>{const auto hex=quant::storage::files::sha256(bytes);Sha256 result{};for(std::size_t i=0;i<32;++i){unsigned v=0;auto parsed=std::from_chars(hex.data()+2*i,hex.data()+2*i+2,v,16);if(parsed.ec!=std::errc{}){return diagnostic(FailureCode::CacheCorrupt,"digest conversion failed");}result[i]=static_cast<std::byte>(v);}return result;});}
Result<Sha256> digest(std::string_view bytes) noexcept{return digest(std::as_bytes(std::span{bytes.data(),bytes.size()}));}
Result<Bytes> read(const std::filesystem::path& path,std::shared_ptr<ResourceAccount> account,std::uint64_t maximum) noexcept{return guarded([&]()->Result<Bytes>{
    if(!account||!path.is_absolute()){return diagnostic(FailureCode::InvalidConfig,"bounded file read requires account and absolute path");}
    Fd fd{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};if(fd.value<0){return diagnostic(FailureCode::CacheIO,"required file cannot be opened");}
    struct stat before{};if(::fstat(fd.value,&before)!=0||!S_ISREG(before.st_mode)||before.st_size<0){return diagnostic(FailureCode::CacheCorrupt,"required artifact is not a regular file");}
    const auto size=static_cast<std::uint64_t>(before.st_size);if(size>maximum||size>std::string{}.max_size()){return diagnostic(FailureCode::ResourcePlanExceeded,"file exceeds declared byte bound");}
    auto amount=checked_multiply(size,4);if(auto* e=std::get_if<Diagnostic>(&amount)){return *e;}amount=checked_add(std::get<std::uint64_t>(amount),4096);if(auto* e=std::get_if<Diagnostic>(&amount)){return *e;}
    auto charged=account->reserve(ResourceKind::Scratch,std::get<std::uint64_t>(amount));if(auto* e=std::get_if<Diagnostic>(&charged)){return *e;}
    std::string bytes(static_cast<std::size_t>(size),'\0');std::size_t offset=0;
    while(offset<bytes.size()){const auto count=::read(fd.value,bytes.data()+offset,bytes.size()-offset);if(count<0&&errno==EINTR){continue;}if(count<=0){return diagnostic(FailureCode::CacheIO,"short artifact read");}offset+=static_cast<std::size_t>(count);}
    struct stat after{};if(::fstat(fd.value,&after)!=0||after.st_size!=before.st_size||after.st_mtimespec.tv_sec!=before.st_mtimespec.tv_sec||after.st_mtimespec.tv_nsec!=before.st_mtimespec.tv_nsec){return diagnostic(FailureCode::SourceChanged,"file changed during bounded read");}
    if(::close(std::exchange(fd.value,-1))!=0){return diagnostic(FailureCode::CacheIO,"artifact read close failed");}
    return Bytes{std::get<Reservation>(std::move(charged)),std::move(bytes)};
});}
Result<EvidenceReference> write_new(std::string name,const std::filesystem::path& path,std::string_view bytes,std::shared_ptr<ResourceAccount> account,std::uint64_t maximum,FailureCode failure) noexcept{return guarded([&]()->Result<EvidenceReference>{
    if(!account||!path.is_absolute()||bytes.size()>maximum){return diagnostic(bytes.size()>maximum?FailureCode::EvidenceQuota:FailureCode::InvalidConfig,"new file path/byte bound is invalid");}
    Fd fd{::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600)};if(fd.value<0){return diagnostic(failure,"could not exclusively create required file");}
    std::size_t offset=0;while(offset<bytes.size()){const auto n=::write(fd.value,bytes.data()+offset,bytes.size()-offset);if(n<0&&errno==EINTR){continue;}if(n<=0){return diagnostic(failure,"required file write failed");}offset+=static_cast<std::size_t>(n);}
    if(::close(std::exchange(fd.value,-1))!=0){return diagnostic(failure,"required file close failed");}
    auto actual=read(path,account,maximum);if(auto* e=std::get_if<Diagnostic>(&actual)){auto cause=*e;cause.code=failure;return cause;}
    const auto& stored=std::get<Bytes>(actual).value;if(stored!=bytes){return diagnostic(failure,"closed file differs from written bytes");}
    auto hash=digest(stored);if(auto* e=std::get_if<Diagnostic>(&hash)){return *e;}
    return EvidenceReference{std::move(name),path.string(),bytes.size(),std::get<Sha256>(hash),true};
});}
Result<void> verify(const EvidenceReference& reference,std::shared_ptr<ResourceAccount> account,std::uint64_t maximum) noexcept{return guarded([&]()->Result<void>{
    if(!reference.finalized||reference.digest==Sha256{}||reference.byte_count>maximum){return diagnostic(FailureCode::CacheCorrupt,"invalid finalized evidence reference");}
    auto bytes=read(reference.absolute_path,account,maximum);if(auto* e=std::get_if<Diagnostic>(&bytes)){return *e;}
    const auto& value=std::get<Bytes>(bytes).value;auto hash=digest(value);if(auto* e=std::get_if<Diagnostic>(&hash)){return *e;}
    if(value.size()!=reference.byte_count||std::get<Sha256>(hash)!=reference.digest){return diagnostic(FailureCode::CacheCorrupt,"referenced evidence length or digest mismatch");}return ok();
});}
Result<EvidenceReference> finish_open(int& fd,std::string name,const std::filesystem::path& path,std::string_view bytes,std::shared_ptr<ResourceAccount> account,std::uint64_t maximum,FailureCode failure) noexcept{return guarded([&]()->Result<EvidenceReference>{
    if(fd<0||bytes.size()>maximum){return diagnostic(fd<0?failure:FailureCode::EvidenceQuota,"required output descriptor/size invalid");}
    std::size_t offset=0;while(offset<bytes.size()){auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);if(n<0&&errno==EINTR){continue;}if(n<=0){return diagnostic(failure,"required output write failed");}offset+=static_cast<std::size_t>(n);}
    if(::close(std::exchange(fd,-1))!=0){return diagnostic(failure,"required output close failed");}
    auto readback=read(path,account,maximum);if(auto* e=std::get_if<Diagnostic>(&readback)){return *e;}const auto& value=std::get<Bytes>(readback).value;
    if(value!=bytes){return diagnostic(failure,"closed required output differs from emitted bytes");}auto hash=digest(value);if(auto* e=std::get_if<Diagnostic>(&hash)){return *e;}
    return EvidenceReference{std::move(name),path.string(),bytes.size(),std::get<Sha256>(hash),true};
});}
std::string unique_id(){std::array<unsigned char,16> bytes{};::arc4random_buf(bytes.data(),bytes.size());constexpr std::string_view hex="0123456789abcdef";std::string result;result.reserve(32);for(auto b:bytes){result.push_back(hex[b>>4]);result.push_back(hex[b&15]);}return result;}
Result<std::shared_ptr<DirectoryLock>> DirectoryLock::acquire(const std::filesystem::path& directory,std::shared_ptr<ResourceAccount> account,std::uint64_t metadata_bytes) noexcept{return guarded([&]()->Result<std::shared_ptr<DirectoryLock>>{
    if(!account||!directory.is_absolute()||metadata_bytes<8192){return diagnostic(FailureCode::InvalidConfig,"store directory/metadata bound is invalid");}
    auto charged=account->reserve(ResourceKind::Graph,metadata_bytes);if(auto* e=std::get_if<Diagnostic>(&charged)){return *e;}
    if(::mkdir(directory.c_str(),0700)!=0&&errno!=EEXIST){return diagnostic(FailureCode::CacheIO,"store directory cannot be created");}
    struct stat status{};if(::lstat(directory.c_str(),&status)!=0||!S_ISDIR(status.st_mode)||S_ISLNK(status.st_mode)){return diagnostic(FailureCode::CacheIO,"store root must be a real directory");}
    auto path=std::filesystem::canonical(directory);Fd lock{::open((path/".lexongine.lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600)};
    if(lock.value<0){return diagnostic(FailureCode::CacheIO,"store lock cannot be opened");}
    if(::fstat(lock.value,&status)!=0||!S_ISREG(status.st_mode)||status.st_nlink!=1){return diagnostic(FailureCode::CacheIO,"store lock must be an unaliased regular file");}
    if(::flock(lock.value,LOCK_EX|LOCK_NB)!=0){return diagnostic(errno==EWOULDBLOCK?FailureCode::CacheBusy:FailureCode::CacheIO,"store writer lock is unavailable");}
    return std::shared_ptr<DirectoryLock>(new DirectoryLock{std::get<Reservation>(std::move(charged)),std::move(path),std::exchange(lock.value,-1),metadata_bytes});
});}
DirectoryLock::~DirectoryLock() noexcept{if(fd_>=0){static_cast<void>(::close(fd_));}}
Result<std::vector<std::pair<std::string,std::uint64_t>>> DirectoryLock::inventory(std::uint64_t maximum_files) const noexcept{return guarded([&]()->Result<std::vector<std::pair<std::string,std::uint64_t>>>{
    // The retained directory reservation covers the vector, bounded names and
    // the store's simultaneous ID set. Iteration is synchronous under its lock.
    if(maximum_files>(metadata_bytes_-8192)/1024){return diagnostic(FailureCode::ResourcePlanExceeded,"directory inventory exceeds reserved metadata");}
    std::vector<std::pair<std::string,std::uint64_t>> result;result.reserve(static_cast<std::size_t>(maximum_files));
    for(const auto& entry:std::filesystem::directory_iterator(path_)){const auto name=entry.path().filename().string();if(name==".lexongine.lock"){continue;}
        if(result.size()==maximum_files||name.size()>255){return diagnostic(FailureCode::ResourcePlanExceeded,"store inventory exceeds file-count/name limit");}
        const auto status=entry.symlink_status();if(!std::filesystem::is_regular_file(status)){return diagnostic(FailureCode::CacheCorrupt,"store contains unsupported file type");}
        result.emplace_back(name,entry.file_size());}
    std::sort(result.begin(),result.end());return result;
});}
Result<void> publish(const std::filesystem::path& temporary,const std::filesystem::path& final) noexcept {
    if(::renameatx_np(AT_FDCWD,temporary.c_str(),AT_FDCWD,final.c_str(),RENAME_EXCL)!=0){return diagnostic(FailureCode::CommitUnconfirmed,"acceptance-marker rename requires reconciliation");}return ok();
}
} // namespace lexongine::files
