#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <Service/Crc32.h>
#include <Service/KeeperCommon.h>
#include <Service/LogEntry.h>
#include <Service/NuRaftLogSegment.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <Poco/File.h>
#include <Common/ThreadPool.h>

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

namespace RK
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int CANNOT_READ_FROM_FILE_DESCRIPTOR;
    extern const int CANNOT_WRITE_TO_FILE_DESCRIPTOR;
}

using namespace nuraft;

int ftruncateUninterrupted(int fd, off_t length)
{
    int rc = 0;
    do
    {
        rc = ftruncate(fd, length);
    } while (rc == -1 && errno == EINTR);
    return rc;
}

bool compareSegment(ptr<NuRaftLogSegment> & seg1, ptr<NuRaftLogSegment> & seg2)
{
    return seg1->firstIndex() < seg2->firstIndex();
}

std::string NuRaftLogSegment::getOpenFileName()
{
    char buf[1024];
    snprintf(buf, 1024, LOG_OPEN_FILE_NAME, first_index, create_time.c_str());
    return std::string(buf);
}

std::string NuRaftLogSegment::getOpenPath()
{
    std::string path(log_dir);
    path += "/" + getOpenFileName();
    return path;
}

std::string NuRaftLogSegment::getFinishFileName()
{
    char buf[1024];
    snprintf(buf, 1024, LOG_FINISH_FILE_NAME, first_index, last_index.load(std::memory_order_relaxed), create_time.c_str());
    return std::string(buf);
}

std::string NuRaftLogSegment::getFinishPath()
{
    std::string path(log_dir);
    path += "/" + getFinishFileName();
    return path;
}

std::string NuRaftLogSegment::getFileName()
{
    if (!file_name.empty())
        return file_name;

    if (is_open)
        return getOpenFileName();
    else
        return getFinishFileName();
}

std::string NuRaftLogSegment::getPath()
{
    return log_dir + "/" + getFileName();
}

int NuRaftLogSegment::openFile()
{
    if (seg_fd > 0)
    {
        return 0;
    }
    std::string full_path = getPath();
    if (!Poco::File(full_path).exists())
    {
        LOG_ERROR(log, "File path {} is not exists.", full_path);
        return -1;
    }
    errno = 0;
    seg_fd = ::open(full_path.c_str(), O_RDWR);
    if (seg_fd < 0)
    {
        LOG_ERROR(log, "Fail to open {}, error:{}", full_path, strerror(errno));
        return -1;
    }
    LOG_INFO(log, "Open segment for read/write, path {}", full_path);
    return 0;
}

int NuRaftLogSegment::closeFile()
{
    if (seg_fd >= 0)
    {
        ::close(seg_fd);
        seg_fd = -1;
    }
    return 0;
}

int NuRaftLogSegment::create()
{
    if (!is_open)
    {
        LOG_WARNING(log, "Create on a closed segment at first_index={} in {}", first_index, log_dir);
        return -1;
    }
    std::lock_guard write_lock(log_mutex);
    BackendTimer::getCurrentTime(create_time);
    file_name = getOpenFileName();
    std::string full_path = getOpenPath();
    if (Poco::File(full_path).exists())
    {
        LOG_ERROR(log, "File {} is exists.", full_path);
        return -1;
    }
    errno = 0;
    seg_fd = ::open(full_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (seg_fd < 0)
    {
        LOG_WARNING(log, "Created new segment {} failed, fd {}, error:{}", full_path, seg_fd, strerror(errno));
        return -1;
    }
    LOG_INFO(log, "Created new segment {}, seg_fd {}, first index {}", full_path, seg_fd, first_index);
    return 0;
}

void NuRaftLogSegment::writeFileHeader()
{
    if (!is_open)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Log segment not open yet");

    if (seg_fd < 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "File not open yet");

    union
    {
        uint64_t magic_num;
        uint8_t magic_array[8] = {0, 'R', 'a', 'f', 't', 'L', 'o', 'g'};
    };

    std::lock_guard write_lock(log_mutex);
    auto version_uint8 = static_cast<uint8_t>(version);

    if (write(seg_fd, &magic_num, 8) != 8)
        throw Exception(ErrorCodes::CANNOT_WRITE_TO_FILE_DESCRIPTOR, "Cannot write magic to file descriptor");

    if (write(seg_fd, &version_uint8, 1) != 1)
        throw Exception(ErrorCodes::CANNOT_WRITE_TO_FILE_DESCRIPTOR, "Cannot write version to file descriptor");

    file_size.fetch_add(sizeof(uint64_t) + sizeof(uint8_t), std::memory_order_release);
}

int NuRaftLogSegment::load()
{
    int ret = 0;

    if (openFile() != 0)
        return -1;

    /// get file size
    struct stat st_buf;
    errno = 0;

    if (fstat(seg_fd, &st_buf) != 0)
    {
        LOG_ERROR(log, "Fail to get the stat, error:{}", strerror(errno));
        ::close(seg_fd);
        seg_fd = -1;
        return -1;
    }

    /// load entry index
    file_size = st_buf.st_size;

    size_t entry_off = loadVersion();
    UInt64 actual_last_index = first_index - 1;

    for (; entry_off < file_size;)
    {
        LogEntryHeader header;
        const int rc = loadLogEntryHeader(seg_fd, entry_off, &header);
        if (rc != 0)
        {
            ret = rc;
            break;
        }

        /// rc == 0
        const UInt64 skip_len = sizeof(LogEntryHeader) + header.data_length;

        if (entry_off + skip_len > file_size)
        {
            /// The last log was not completely written and it should be
            /// truncated
            ret = -1;
            break;
        }

        offset_term.push_back(std::make_pair(entry_off, header.term));
        ++actual_last_index;
        entry_off += skip_len;

        if (actual_last_index << 44 == 0)
        {
            LOG_DEBUG(
                log,
                "Load log segment, entry_off {}, skip_len {}, file_size {}, actual_last_index {}",
                entry_off,
                skip_len,
                file_size,
                actual_last_index);
        }
    }

    const UInt64 curr_last_index = last_index.load(std::memory_order_relaxed);

    if (ret == 0 && !is_open)
    {
        if (actual_last_index < curr_last_index)
        {
            LOG_ERROR(
                log,
                "Data lost in a full segment, directory {}, first index {}, expect last index {}, actual last index {}",
                log_dir,
                first_index,
                curr_last_index,
                actual_last_index);
            ret = -1;
        }
        else if (actual_last_index > curr_last_index)
        {
            LOG_ERROR(
                log,
                "Found garbage in a full segment, directory {}, first index {}, expect last index {}, actual last index {} ",
                log_dir,
                first_index,
                last_index,
                actual_last_index);
            ret = -1;
        }
    }

    if (ret != 0)
        return ret;

    if (is_open)
    {
        LOG_INFO(log, "Open segment last_index {}.", actual_last_index);
        last_index = actual_last_index;
    }

    /// truncate last uncompleted entry
    if (entry_off != file_size)
    {
        LOG_INFO(
            log,
            "Truncate last uncompleted write entry, directory {}, first_index {}, old size {}, new size {} ",
            log_dir,
            first_index,
            file_size,
            entry_off);
        ret = ftruncateUninterrupted(seg_fd, entry_off);
    }

    file_size = entry_off;

    if (is_open)
        ::lseek(seg_fd, entry_off, SEEK_SET);

    return ret;
}

size_t NuRaftLogSegment::loadVersion()
{
    if (seg_fd < 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "File not open yet");

    /// magic + version. 9 bytes
    ptr<buffer> buf = buffer::alloc(9);

    buf->pos(0);
    errno = 0;

    ssize_t ret = pread(seg_fd, buf->data(), 9, 0);
    if (ret != 9)
        throw Exception(ErrorCodes::CANNOT_READ_FROM_FILE_DESCRIPTOR, "Cannot read from file descriptor");

    buffer_serializer bs(buf);
    bs.pos(0);
    uint64_t magic = bs.get_u64();

    union
    {
        uint64_t magic_num;
        uint8_t magic_array[8] = {0, 'R', 'a', 'f', 't', 'L', 'o', 'g'};
    };

    if (magic == magic_num)
    {
        version = static_cast<LogVersion>(bs.get_u8());
        LOG_INFO(log, "Magic num is {}, version {}", magic_num, version);
        return 9;
    }
    else
    {
        LOG_INFO(log, "Not have magic num, set version V0");
        version = LogVersion::V0;
        return 0;
    }
}

int NuRaftLogSegment::close(bool is_full)
{
    std::lock_guard write_lock(log_mutex);

    if (!is_open)
        return 0;

    int ret = closeFile();

    if (ret)
        return ret;

    if (is_full)
    {
        std::string old_path = getOpenPath();
        std::string new_path = getFinishPath();

        LOG_INFO(
            log,
            "Close a full segment. Current first index {}, last index {}, renamed {} to {}.",
            first_index,
            last_index,
            old_path,
            new_path);

        is_open = false;
        Poco::File(old_path).renameTo(new_path);
        file_name = getFinishFileName();
        return 0;
    }
    return 0;
}

UInt64 NuRaftLogSegment::flush() const
{
    if (seg_fd >= 0)
    {
        std::lock_guard write_lock(log_mutex);

        int ret;
#if defined(OS_DARWIN)
        ret = ::fsync(seg_fd);
#else
        ret = ::fdatasync(seg_fd);
#endif
        if (ret == -1)
            LOG_ERROR(log, "log fsync error error no {}", errno);
        else if (ret == 0)
            return last_index; /// return last_index
    }
    return 0;
}

int NuRaftLogSegment::remove()
{
    std::lock_guard write_lock(log_mutex);
    closeFile();
    std::string full_path = getPath();
    Poco::File file_obj(full_path);
    if (file_obj.exists())
    {
        LOG_INFO(log, "Remove log segment {}", full_path);
        file_obj.remove();
    }
    return 0;
}

UInt64 NuRaftLogSegment::appendEntry(ptr<log_entry> entry, std::atomic<UInt64> & last_log_index)
{
    LogEntryHeader header;
    ptr<buffer> entry_buf;

    char * entry_str;
    size_t buf_size = 0;

    struct iovec vec[2];
    {
        if (!entry || !is_open)
            return -1;

        entry_str = LogEntry::serializeEntry(entry, entry_buf, buf_size);

        if (entry_str == nullptr || buf_size == 0)
        {
            LOG_ERROR(log, "Can't get entry string buffer, size is {}.", buf_size);
            return -1;
        }

        if (seg_fd < 0)
        {
            LOG_ERROR(log, "seg fs is null.");
            return -1;
        }

        header.term = entry->get_term();
        header.data_length = buf_size;
        header.data_crc = RK::getCRC32(entry_str, header.data_length);

        vec[0].iov_base = &header;
        vec[0].iov_len = LogEntryHeader::HEADER_SIZE;
        vec[1].iov_base = reinterpret_cast<void *>(entry_str);
        vec[1].iov_len = header.data_length;
    }

    errno = 0;

    {
        std::lock_guard write_lock(log_mutex);
        header.index = last_index.load(std::memory_order_acquire) + 1;
        ssize_t ret = writev(seg_fd, vec, 2);

        if (ret < 0 || ret != static_cast<ssize_t>(vec[0].iov_len + vec[1].iov_len))
        {
            LOG_WARNING(log, "Write {}, real size {}, error:{}", ret, vec[0].iov_len + vec[1].iov_len, strerror(errno));
            return -1;
        }

        offset_term.push_back(std::make_pair(file_size.load(std::memory_order_relaxed), entry->get_term()));
        file_size.fetch_add(LogEntryHeader::HEADER_SIZE + header.data_length, std::memory_order_release);

        last_index.fetch_add(1, std::memory_order_release);
        last_log_index.store(last_index, std::memory_order_release);
    }

    LOG_TRACE(
        log,
        "Append term {}, index {}, length {}, crc {}, file {}, entry type {}.",
        header.term,
        header.index,
        header.data_length,
        header.data_crc,
        file_size,
        entry->get_val_type());

    return header.index;
}

[[maybe_unused]] int NuRaftLogSegment::writeAt(UInt64 index, const ptr<log_entry> entry)
{
    LOG_TRACE(log, "Write at term {}, index {}", entry->get_term(), index);
    return 0;
}

int NuRaftLogSegment::getMeta(UInt64 index, LogMeta * meta) const
{
    if (last_index == first_index - 1 || index > last_index.load(std::memory_order_relaxed) || index < first_index)
    {
        LOG_WARNING(log, "current_index={}, last_index={}, first_index={}", index, last_index.load(std::memory_order_relaxed), first_index);
        return -1;
    }

    UInt64 meta_index = index - first_index;
    UInt64 entry_offset = offset_term[meta_index].first;

    UInt64 next_offset;

    if (index < last_index.load(std::memory_order_relaxed))
        next_offset = offset_term[meta_index + 1].first;
    else
        next_offset = file_size;

    meta->offset = entry_offset;
    meta->term = offset_term[meta_index].second;
    meta->length = next_offset - entry_offset;

    LOG_TRACE(log, "Get meta offset {}, term {}, length {}.", meta->offset, meta->term, meta->length);
    return 0;
}

int NuRaftLogSegment::loadLogEntryHeader(int fd, off_t offset, LogEntryHeader * header) const
{
    if (header == nullptr)
        return -1;

    ptr<buffer> buf = buffer::alloc(LogEntryHeader::HEADER_SIZE);
    buf->pos(0);

    errno = 0;
    ssize_t ret = pread(fd, buf->data(), LogEntryHeader::HEADER_SIZE, offset);

    if (ret != LogEntryHeader::HEADER_SIZE)
    {
        LOG_ERROR(
            log,
            "Read log entry header failed, offset {}, header size {}, ret:{}, error:{}.",
            offset,
            LogEntryHeader::HEADER_SIZE,
            ret,
            strerror(errno));
        return -1;
    }

    buffer_serializer bs(buf);
    bs.pos(0);

    header->term = bs.get_u64();
    header->index = bs.get_u64();

    header->data_length = bs.get_u32();
    header->data_crc = bs.get_u32();

    LOG_TRACE(log, "Offset {}, header data length {}, data crc {}", offset, header->data_length, header->data_crc);
    return 0;
}

int NuRaftLogSegment::loadLogEntry(int fd, off_t offset, LogEntryHeader * head, ptr<log_entry> & entry) const
{
    if (loadLogEntryHeader(fd, offset, head) != 0)
        return -1;

    LOG_TRACE(log, "Load entry header, length {}, crc {}.", head->data_length, head->data_crc);
    char * entry_str = new char[head->data_length];

    errno = 0;
    ssize_t ret = pread(fd, entry_str, head->data_length, offset + LogEntryHeader::HEADER_SIZE);

    if (ret < 0 || ret != head->data_length)
    {
        LOG_ERROR(log, "Can't read app data from log segment, ret:{}, error:{}.", ret, strerror(errno));
        delete[] entry_str;
        return -1;
    }

    LOG_TRACE(log, "Load entry body, length {}, crc {}.", head->data_length, head->data_crc);

    if (!verifyCRC32(entry_str, head->data_length, head->data_crc))
    {
        LOG_ERROR(
            log,
            "Found corrupted data at offset {}, term {}, index {}, length {}, crc {}, file {}",
            offset,
            head->term,
            head->index,
            head->data_length,
            head->data_crc,
            file_name);
        delete[] entry_str;
        return -1;
    }

    entry = LogEntry::parseEntry(entry_str, head->term, head->data_length);
    LOG_TRACE(log, "Alloc buffer, offset {}, length {}, crc {}, term {}.", offset, head->data_length, head->data_crc, entry->get_term());

    delete[] entry_str;
    return 0;
}

ptr<log_entry> NuRaftLogSegment::getEntry(UInt64 index)
{
    {
        std::lock_guard write_lock(log_mutex);
        if (openFile() != 0)
            return nullptr;
    }

    std::shared_lock read_lock(log_mutex);
    LogMeta meta;

    if (getMeta(index, &meta) != 0)
        return nullptr;

    bool ok = true;
    ptr<log_entry> entry;

    do
    {
        LogEntryHeader header;
        size_t offset = meta.offset;

        if (loadLogEntry(seg_fd, offset, &header, entry) != 0)
        {
            LOG_WARNING(log, "Get entry failed, path {}, index {}, offset {}.", getPath(), index, offset);
            ok = false;
            break;
        }
    } while (false);

    if (!ok && entry != nullptr)
        entry = nullptr;

    return entry;
}


UInt64 NuRaftLogSegment::getTerm(UInt64 index) const
{
    LogMeta meta;
    if (getMeta(index, &meta) != 0)
    {
        return 0;
    }
    return meta.term;
}

int NuRaftLogSegment::truncate(const UInt64 last_index_kept)
{
    UInt64 truncate_size = 0;
    UInt64 first_truncate_in_offset = 0;

    {
        std::lock_guard write_lock(log_mutex);
        if (last_index <= last_index_kept)
        {
            LOG_INFO(log, "truncate nothing, last_index {}, last_index_kept {}", last_index, last_index_kept);
            return 0;
        }

        first_truncate_in_offset = last_index_kept + 1 - first_index;
        truncate_size = offset_term[first_truncate_in_offset].first;

        LOG_INFO(
            log,
            "Truncating {}, offset {}, first_index {}, last_index from {} to {}, truncate_size to {} ",
            getFileName(),
            first_truncate_in_offset,
            first_index,
            last_index,
            last_index_kept,
            truncate_size);
    }

    /// Truncate on a full segment need to rename back to open segment again,
    /// because the node may crash before truncate.
    if (!is_open)
    {
        std::string old_path = getFinishPath();
        std::string new_path = getOpenPath();

        LOG_INFO(
            log,
            "Truncate segment closed and reopen. Current first index {}, last index {}, renamed {} to {}.",
            first_index,
            last_index,
            old_path,
            new_path);

        Poco::File(old_path).renameTo(new_path);
        file_name = getOpenFileName();

        is_open = true;
    }

    openFile();

    errno = 0;
    int ret = ftruncate(seg_fd, truncate_size);

    if (ret != 0)
    {
        LOG_INFO(log, "Truncate failed errno {}, msg {}", errno, strerror(errno));
        return ret;
    }

    LOG_INFO(log, "Truncate file {} descriptor {}, from {} to size {}", getOpenPath(), seg_fd, file_size, truncate_size);

    /// seek fd
    off_t ret_off = lseek(seg_fd, truncate_size, SEEK_SET);

    if (ret_off < 0)
    {
        LOG_ERROR(log, "Fail to lseek fd {} to size {}, path {}.", seg_fd, truncate_size, getOpenPath());
        ret = ret_off;
    }
    else
    {
        std::lock_guard write_lock(log_mutex);
        offset_term.resize(first_truncate_in_offset);
        last_index.store(last_index_kept, std::memory_order_release);
        file_size = truncate_size;
    }

    return ret;
}

ptr<LogSegmentStore> LogSegmentStore::segment_store = nullptr;

ptr<LogSegmentStore> LogSegmentStore::getInstance(const std::string & log_dir_, bool force_new)
{
    if (segment_store == nullptr || force_new)
        segment_store = cs_new<LogSegmentStore>(log_dir_);

    return segment_store;
}

int LogSegmentStore::init(UInt32 max_segment_file_size_, UInt32 max_segment_count_)
{
    LOG_INFO(
        log,
        "Begin init log segment store, max segment file size {} bytes, max segment count {}.",
        max_segment_file_size_,
        max_segment_count_);

    max_segment_file_size = max_segment_file_size_;
    max_segment_count = max_segment_count_;

    if (Directory::createDir(log_dir) != 0)
    {
        LOG_ERROR(log, "Fail to create directory {}", log_dir);
        return -1;
    }

    int ret = 0;

    first_log_index.store(1);
    last_log_index.store(0);

    open_segment = nullptr;

    do
    {
        ret = listSegments();
        if (ret != 0)
        {
            LOG_WARNING(log, "List segments failed, error code {}.", ret);
            break;
        }
        ret = loadSegments();
        if (ret != 0)
        {
            LOG_WARNING(log, "Load segments failed, error code {}.", ret);
            break;
        }
        ret = openSegment();
        if (ret != 0)
        {
            LOG_WARNING(log, "Open segment failed, error code {}", ret);
            break;
        }
    } while (false);

    return ret;
}

int LogSegmentStore::close()
{
    if (open_segment)
    {
        std::lock_guard write_lock(seg_mutex);
        open_segment->close(false);
        open_segment = nullptr;
    }
    return 0;
}

UInt64 LogSegmentStore::flush()
{
    if (open_segment)
    {
        std::lock_guard write_lock(seg_mutex);
        return open_segment->flush();
    }
    return 0;
}

int LogSegmentStore::openSegment()
{
    {
        std::shared_lock read_lock(seg_mutex);
        if (open_segment && open_segment->getFileSize() <= max_segment_file_size && open_segment->getVersion() >= CURRENT_LOG_VERSION)
            return 0;
    }

    std::lock_guard write_lock(seg_mutex);
    if (open_segment)
    {
        open_segment->close(true);
        segments.push_back(open_segment);
        open_segment = nullptr;
    }

    UInt64 next_idx = last_log_index.load(std::memory_order_acquire) + 1;
    ptr<NuRaftLogSegment> seg = cs_new<NuRaftLogSegment>(log_dir, next_idx);

    open_segment = seg;
    if (open_segment->create() != 0)
    {
        LOG_ERROR(log, "Create open segment directory {} index {} failed.", log_dir, next_idx);
        open_segment = nullptr;
        return -1;
    }

    try
    {
        open_segment->writeFileHeader();
    }
    catch (...)
    {
        open_segment = nullptr;
        return -1;
    }

    return 0;
}

int LogSegmentStore::getSegment(UInt64 index, ptr<NuRaftLogSegment> & seg)
{
    seg = nullptr;
    UInt64 first_index = first_log_index.load(std::memory_order_acquire);
    UInt64 last_index = last_log_index.load(std::memory_order_acquire);

    if (first_index == last_index + 1) // TODO Right?
    {
        LOG_WARNING(log, "Log segment store no data, entry index {}.", index);
        return -1;
    }

    if (index < first_index || index > last_index)
    {
        LOG_WARNING(log, "Attempted to access entry {} outside of log, index range [{}, {}].", index, first_index, last_index);
        return -1;
    }

    if (open_segment && index >= open_segment->firstIndex())
    {
        seg = open_segment;
    }
    else
    {
        for (auto & segment : segments)
        {
            ptr<NuRaftLogSegment> seg_it = segment;
            if (index >= seg_it->firstIndex() && index <= seg_it->lastIndex())
            {
                LOG_TRACE(log, "segment index range [{}, {}].", seg_it->firstIndex(), seg_it->lastIndex());
                seg = seg_it;
            }
        }
    }

    if (seg != nullptr)
        return 0;
    else
        return -1;
}

LogVersion LogSegmentStore::getVersion(UInt64 index)
{
    ptr<NuRaftLogSegment> seg;
    getSegment(index, seg);
    return seg->getVersion();
}

UInt64 LogSegmentStore::appendEntry(ptr<log_entry> entry)
{
    if (openSegment() != 0)
    {
        LOG_INFO(log, "Open segment failed.");
        return -1;
    }
    std::shared_lock read_lock(seg_mutex);
    return open_segment->appendEntry(entry, last_log_index);
}

UInt64 LogSegmentStore::writeAt(UInt64 index, ptr<log_entry> entry)
{
    truncateLog(index - 1);
    if (index == lastLogIndex() + 1)
        return appendEntry(entry);

    LOG_WARNING(log, "writeAt log index {} failed, firstLogIndex {}, lastLogIndex {}.", index, firstLogIndex(), lastLogIndex());
    return -1;
}

ptr<log_entry> LogSegmentStore::getEntry(UInt64 index)
{
    ptr<NuRaftLogSegment> seg;
    std::shared_lock read_lock(seg_mutex);
    if (getSegment(index, seg) != 0)
    {
        LOG_WARNING(log, "Can't find log segmtnt by index {}.", index);
        return nullptr;
    }
    return seg->getEntry(index);
}

void LogSegmentStore::getEntries(UInt64 start_index, UInt64 end_index, ptr<std::vector<ptr<log_entry>>> & entries)
{
    if (entries == nullptr)
    {
        LOG_ERROR(log, "Entry vector is nullptr.");
        return;
    }
    for (UInt64 index = start_index; index <= end_index; index++)
    {
        auto entry_pt = getEntry(index);
        entries->push_back(entry_pt);
    }
}


[[maybe_unused]] void LogSegmentStore::getEntriesExt(
    UInt64 start_index, UInt64 end_index, int64 batch_size_hint_in_bytes, ptr<std::vector<ptr<log_entry>>> & entries)
{
    if (entries == nullptr)
    {
        LOG_ERROR(log, "Entry vector is nullptr.");
        return;
    }

    int64 get_size = 0;
    int64 entry_size = 0;

    for (UInt64 index = start_index; index <= end_index; index++)
    {
        auto entry_pt = getEntry(index);
        entry_size = entry_pt->get_buf().size() + sizeof(ulong) + sizeof(char);

        if (get_size + entry_size > batch_size_hint_in_bytes)
            break;

        entries->push_back(entry_pt);
        get_size += entry_size;
    }
}

[[maybe_unused]] UInt64 LogSegmentStore::getTerm(UInt64 index)
{
    ptr<NuRaftLogSegment> seg;
    if (getSegment(index, seg) != 0)
    {
        return 0;
    }
    return seg->getTerm(index);
}

int LogSegmentStore::removeSegment(UInt64 first_index_kept)
{
    if (first_log_index.load(std::memory_order_acquire) >= first_index_kept)
    {
        LOG_INFO(
            log,
            "Nothing is going to happen since first_log_index {} >= first_index_kept {}",
            first_log_index.load(std::memory_order_relaxed),
            first_index_kept);
        return 0;
    }

    {
        std::lock_guard write_lock(seg_mutex);
        std::vector<ptr<NuRaftLogSegment>> to_be_removed;

        {
            first_log_index.store(first_index_kept, std::memory_order_release);
            for (auto it = segments.begin(); it != segments.end();)
            {
                ptr<NuRaftLogSegment> & segment = *it;
                if (segment->lastIndex() < first_index_kept)
                {
                    to_be_removed.push_back(segment);
                    it = segments.erase(it);
                }
                else
                {
                    if (segment->firstIndex() < first_log_index)
                    {
                        first_log_index.store(segment->firstIndex(), std::memory_order_release);
                        if (last_log_index == 0 || (last_log_index - 1) < first_log_index)
                            last_log_index.store(segment->lastIndex(), std::memory_order_release);
                    }
                    it++;
                }
            }
        }

        //remove open segment.
        // Because when adding a node, you may directly synchronize the snapshot and do log compaction.
        // At this time, the log of the new node is smaller than the last log index of the snapshot.
        // So remove the open segment.
        if (open_segment)
        {
            if (open_segment->lastIndex() < first_index_kept)
            {
                to_be_removed.push_back(open_segment);
                open_segment = nullptr;
            }
            else if (open_segment->firstIndex() < first_log_index)
            {
                first_log_index.store(open_segment->firstIndex(), std::memory_order_release);
                if (last_log_index == 0 || (last_log_index - 1) < first_log_index)
                    last_log_index.store(open_segment->lastIndex(), std::memory_order_release);
            }
        }

        for (auto & seg : to_be_removed)
        {
            seg->remove();
            LOG_INFO(log, "Remove segment, directory {}, file {}", log_dir, seg->getFileName());
        }

        /// reset last_log_index
        if (last_log_index == 0 || (last_log_index - 1) < first_log_index)
            last_log_index.store(first_log_index - 1, std::memory_order_release);
    }

    return 0;
}


int LogSegmentStore::removeSegment()
{
    UInt32 remove_count = segments.size() + 1 - max_segment_count;
    if (remove_count <= 0)
    {
        return 0;
    }

    std::lock_guard write_lock(seg_mutex);
    std::vector<ptr<NuRaftLogSegment>> remove_vec;

    {
        std::sort(segments.begin(), segments.end(), compareSegment);
        for (UInt32 i = 0; i < remove_count; i++)
        {
            ptr<NuRaftLogSegment> & segment = *(segments.begin());
            remove_vec.push_back(segment);
            first_log_index.store(segment->lastIndex() + 1, std::memory_order_release);
            segments.erase(segments.begin());
        }
    }

    for (auto & i : remove_vec)
    {
        i->remove();
        LOG_INFO(log, "Remove segment, directory {}, file {}", log_dir, i->getFileName());
        i = nullptr;
    }
    return 0;
}

int LogSegmentStore::truncateLog(UInt64 last_index_kept)
{
    if (last_log_index.load(std::memory_order_acquire) <= last_index_kept)
    {
        LOG_INFO(
            log,
            "Nothing is going to happen since last_log_index {} <= last_index_kept {}",
            last_log_index.load(std::memory_order_relaxed),
            last_index_kept);
        return 0;
    }

    std::vector<ptr<NuRaftLogSegment>> remove_vec;
    ptr<NuRaftLogSegment> last_segment = nullptr;

    {
        std::lock_guard write_lock(seg_mutex);
        /// remove finished segment
        for (auto it = segments.begin(); it != segments.end();)
        {
            ptr<NuRaftLogSegment> & segment = *it;
            if (segment->firstIndex() > last_index_kept)
            {
                remove_vec.push_back(segment);
                it = segments.erase(it);
            }

            /// Get the segment to last_index_kept belongs
            else if (last_index_kept >= segment->firstIndex() && last_index_kept <= segment->lastIndex())
            {
                last_segment = segment;
                it++;
            }
            else
                it++;
        }

        /// remove open segment
        if (open_segment)
        {
            if (open_segment->firstIndex() > last_index_kept)
            {
                remove_vec.push_back(open_segment);
                open_segment = nullptr;
            }
            else if (last_index_kept >= open_segment->firstIndex() && last_index_kept <= open_segment->lastIndex())
            {
                last_segment = open_segment;
            }
        }
    }

    ///remove files
    for (auto & i : remove_vec)
    {
        i->remove();
        LOG_INFO(log, "Remove segment, directory {}, file {}", log_dir, i->getFileName());
        i = nullptr;
    }

    if (last_segment)
    {
        bool closed = !last_segment->isOpen();
        const int ret = last_segment->truncate(last_index_kept);

        if (ret != 0)
        {
            LOG_ERROR(log, "Truncate error {}, last_index_kept {}", last_segment->getFileName(), last_index_kept);
            return ret;
        }

        if (closed && last_segment->isOpen())
        {
            std::lock_guard write_lock(seg_mutex);
            if (open_segment)
            {
                LOG_WARNING(log, "Open segment is not nullptr.");
            }
            open_segment.swap(last_segment);

            if (!segments.empty())
                segments.erase(segments.end() - 1);
        }
        if (ret == 0)
            last_log_index.store(last_index_kept, std::memory_order_release);

        return ret;
    }
    else
    {
        LOG_WARNING(log, "Truncate log not found last segment, last_index_kept {}.", last_index_kept);
    }

    last_log_index.store(last_index_kept, std::memory_order_release);
    return 0;
}

int LogSegmentStore::reset(UInt64 next_log_index)
{
    if (next_log_index <= 0)
    {
        /// LOG_ERROR << "Invalid next_log_index=" << next_log_index << " path: " << log_dir;
        return EINVAL;
    }

    std::vector<ptr<NuRaftLogSegment>> popped;
    std::unique_lock write_lock(seg_mutex);
    popped.reserve(segments.size());

    for (auto & segment : segments)
    {
        popped.push_back(segment);
    }

    segments.clear();

    if (open_segment)
    {
        popped.push_back(open_segment);
        open_segment = nullptr;
    }

    first_log_index.store(next_log_index, std::memory_order_release);
    last_log_index.store(next_log_index - 1, std::memory_order_release);

    write_lock.unlock();
    for (auto & i : popped)
    {
        i = nullptr;
    }
    return 0;
}

int LogSegmentStore::listSegments()
{
    Poco::File file_dir(log_dir);
    if (!file_dir.exists())
    {
        LOG_WARNING(log, "Log directory {} is not exists.", log_dir);
        return 0;
    }

    std::vector<std::string> files;
    file_dir.list(files);

    for (const auto& file_name : files)
    {
        if (file_name.find("log_") == std::string::npos)
            continue;

        LOG_INFO(log, "List log dir {}, file name {}", log_dir, file_name);

        int match = 0;
        UInt64 first_index = 0;
        UInt64 last_index = 0;

        char create_time[128];
        match = sscanf(file_name.c_str(), NuRaftLogSegment::LOG_FINISH_FILE_NAME, &first_index, &last_index, create_time);

        if (match == 3)
        {
            LOG_INFO(log, "Restore closed segment, directory {}, first index {}, last index {}", log_dir, first_index, last_index);
            ptr<NuRaftLogSegment> segment = cs_new<NuRaftLogSegment>(log_dir, first_index, last_index, file_name);
            segments.push_back(segment);
            continue;
        }

        match = sscanf(file_name.c_str(), NuRaftLogSegment::LOG_OPEN_FILE_NAME, &first_index, create_time);

        if (match == 2)
        {
            LOG_INFO(log, "Restore open segment, directory {}, first index {}, file name {}", log_dir, first_index, file_name);
            if (!open_segment)
            {
                open_segment = cs_new<NuRaftLogSegment>(log_dir, first_index, file_name, std::string(create_time));
                LOG_INFO(log, "Create open segment, directory {}, first index {}, file name {}", log_dir, first_index, file_name);
                continue;
            }
            else
            {
                LOG_WARNING(log, "Open segment conflict, directory {}, first index {}, file name {}", log_dir, first_index, file_name);
                return -1;
            }
        }
    }

    std::sort(segments.begin(), segments.end(), compareSegment);

    /// 0 close/open segment
    /// 1 open segment
    /// N close segment + 1 open segment
    if (open_segment)
    {
        if (!segments.empty())
            first_log_index.store((*segments.begin())->firstIndex(), std::memory_order_release);
        else
            first_log_index.store(open_segment->firstIndex(), std::memory_order_release);

        last_log_index.store(open_segment->lastIndex(), std::memory_order_release);
    }

    /// check segment
    /// last_log_index = 0;

    ptr<NuRaftLogSegment> prev_seg = nullptr;
    ptr<NuRaftLogSegment> segment;

    for (auto it = segments.begin(); it != segments.end();)
    {
        segment = *it;
        LOG_INFO(
            log,
            "first log index {}, last log index {}, current segment first index {}, last index {}",
            first_log_index.load(std::memory_order_relaxed),
            last_log_index.load(std::memory_order_relaxed),
            segment->firstIndex(),
            segment->lastIndex());

        if (segment->firstIndex() > segment->lastIndex())
        {
            LOG_WARNING(
                log,
                "Closed segment is bad, directory {}, current segment first index {}, last index {}",
                log_dir,
                segment->firstIndex(),
                segment->lastIndex());
            return -1;
        }

        if (prev_seg && segment->firstIndex() != prev_seg->lastIndex() + 1)
        {
            LOG_WARNING(
                log,
                "Closed segment not in order, directory {}, prev segment last index {}, current segment first index {}",
                log_dir,
                prev_seg->lastIndex(),
                segment->firstIndex());
            return -1;
        }
        ++it;
    }

    if (open_segment)
    {
        if (prev_seg && open_segment->firstIndex() != prev_seg->lastIndex() + 1)
        {
            LOG_WARNING(
                log,
                "Open segment has hole, directory {}, prev segment last index {}, open segment first index {}",
                log_dir,
                prev_seg->lastIndex(),
                open_segment->firstIndex());
        }
    }

    return 0;
}

int LogSegmentStore::loadSegments()
{
    /// closed segments
    ThreadPool load_thread_pool(LOAD_THREAD_NUM);

    for (UInt32 thread_idx = 0; thread_idx < LOAD_THREAD_NUM; thread_idx++)
    {
        load_thread_pool.trySchedule([this, thread_idx] {
            Poco::Logger * thread_log = &(Poco::Logger::get("LoadLogThread"));
            int ret = 0;
            for (size_t seg_idx = 0; seg_idx < this->getClosedSegments().size(); seg_idx++)
            {
                if (seg_idx % LOAD_THREAD_NUM == thread_idx)
                {
                    ptr<NuRaftLogSegment> segment = this->getClosedSegments()[seg_idx];
                    LOG_INFO(thread_log, "Load closed segment, first_index {}, last_index {}", segment->firstIndex(), segment->lastIndex());
                    ret = segment->load();
                    if (ret != 0)
                    {
                        LOG_WARNING(log, "Load closed segment {} failed {}", segment->firstIndex(), ret);
                        continue;
                    }
                    if (segment->lastIndex() > this->lastLogIndex())
                    {
                        LOG_INFO(log, "Close segment last index {}", segment->lastIndex());
                        this->setLastLogIndex(segment->lastIndex());
                    }
                }
            }
        });
    }

    load_thread_pool.wait();

    /// open segment
    if (open_segment)
    {
        LOG_INFO(log, "Load open segment, directory {}, file name {} ", log_dir, open_segment->getFileName());
        int ret = open_segment->load();

        if (ret != 0)
            return ret;

        if (first_log_index.load() > open_segment->lastIndex())
        {
            LOG_WARNING(
                log,
                "open segment need discard, file {}, first_log_index {}, first_index {}, last_index {} ",
                open_segment->getFileName(),
                first_log_index.load(),
                open_segment->firstIndex(),
                open_segment->lastIndex());
            open_segment = nullptr;
        }
        else
        {
            last_log_index.store(open_segment->lastIndex(), std::memory_order_release);
            LOG_INFO(log, "Open segment last index {} {}", open_segment->lastIndex(), last_log_index);
        }
    }

    if (last_log_index == 0)
        last_log_index = first_log_index - 1;

    return 0;
}

}

#ifdef __clang__
#    pragma clang diagnostic pop
#endif
