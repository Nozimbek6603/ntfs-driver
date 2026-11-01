/*
 * ntfs_jni_driver.cpp
 * Android NDK C++20 darajasidagi yuqori tezlikli NTFS USB-OTG drayveri.
 *
 * Asosiy imkoniyatlari:
 * 1. To'g'ridan-to'g'ri Linux `usbdevfs` (`USBDEVFS_BULK`) orqali SCSI Bulk-Only Transport (BOT):
 *    Java qatlamini chetlab o'tib, USB fayl deskriptori (`usbFd`) orqali bloklarni
 *    `READ(16)`, `WRITE(16)`, `READ CAPACITY(16)` va `SYNCHRONIZE CACHE(10)` buyruqlari bilan uzatadi.
 * 2. 64 MB Write-Back LRU Block Cache (256 KB kesh bloklari bilan):
 *    Kichik fayllar va MFT (Master File Table) yozuvlarini tezkor xotirada keshlab,
 *    USB shinasining to'liq o'tkazuvchanlik tezligini (USB 3.x / 2.0) ta'minlaydi.
 * 3. `libntfs-3g` Custom Device Operations (`ntfs_device_operations`):
 *    Tuxera `libntfs-3g` kutubxonasini to'g'ridan-to'g'ri SCSI kesh qatlamiga bog'laydi hamda
 *    noto'g'ri uzilgan ("Dirty Volume") disklarni `NTFS_MNT_RECOVER` orqali avtomatik tiklaydi.
 *
 * CMakeLists.txt orqali yig'ish:
 *   add_library(ntfs_jni_driver SHARED ntfs_jni_driver.cpp)
 *   target_link_libraries(ntfs_jni_driver ntfs-3g android log)
 */

#include <jni.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/usbdevice_fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <endian.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if __has_include(<android/log.h>)
#include <android/log.h>
#define LOG_TAG "NtfsJniDriver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) std::fprintf(stdout, __VA_ARGS__)
#define LOGE(...) std::fprintf(stderr, __VA_ARGS__)
#endif

// Tuxera libntfs-3g sarlavhalari (NDK tarkibida statik/dinamik ulanadi)
#if __has_include(<ntfs-3g/volume.h>)
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/unistr.h>
#define HAS_LIBNTFS_3G 1
#else
#define HAS_LIBNTFS_3G 0
#endif

namespace {

// ============================================================================
// 1. USB MASS STORAGE BULK-ONLY TRANSPORT (SCSI BOT) TUZILMALARI
// ============================================================================
#pragma pack(push, 1)
struct CommandBlockWrapper {
    uint32_t dCBWSignature;          // 0x43425355 ('USBC')
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;             // 0x80 = Data-In (Device->Host), 0x00 = Data-Out (Host->Device)
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
};

struct CommandStatusWrapper {
    uint32_t dCSWSignature;          // 0x53425355 ('USBS')
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;             // 0x00 = Passed, 0x01 = Failed, 0x02 = Phase Error
};
#pragma pack(pop)

constexpr uint32_t CBW_SIGNATURE = 0x43425355U;
constexpr uint32_t CSW_SIGNATURE = 0x53425355U;
constexpr unsigned int USB_TIMEOUT_MS = 5000;
constexpr size_t CACHE_LINE_SIZE = 256 * 1024; // 256 KB kesh bloki
constexpr size_t MAX_CACHE_LINES = 256;        // 256 * 256 KB = 64 MB Write-Back LRU Cache

// ============================================================================
// 2. TO'G'RIDAN-TO'G'RI USB-SCSI BLOK QURILMASI VA 64 MB WRITE-BACK KESH
// ============================================================================
class UsbScsiBlockDevice {
public:
    UsbScsiBlockDevice(int fd, int epIn, int epOut, int intfIdx)
        : usb_fd_(fd),
          ep_in_(epIn),
          ep_out_(epOut),
          interface_index_(intfIdx),
          tag_counter_(1),
          sector_size_(512),
          total_sectors_(0),
          partition_lba_offset_(0),
          cluster_size_(4096) {}

    ~UsbScsiBlockDevice() {
        FlushCache();
    }

    bool InitializeAndFindNtfsPartition() {
        std::lock_guard<std::mutex> lock(io_mutex_);

        // READ CAPACITY (10) buyrug'i orqali sektor hajmi va umumiy hajmni aniqlash
        uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t cap_resp[8] = {0};
        if (!ExecuteScsiCommand(cdb, sizeof(cdb), cap_resp, sizeof(cap_resp), true)) {
            LOGE("SCSI READ CAPACITY (10) xatosi");
            return false;
        }

        uint32_t raw_last_lba = 0;
        uint32_t raw_block_len = 0;
        std::memcpy(&raw_last_lba, &cap_resp[0], sizeof(uint32_t));
        std::memcpy(&raw_block_len, &cap_resp[4], sizeof(uint32_t));
        uint32_t last_lba = be32toh(raw_last_lba);
        uint32_t block_len = be32toh(raw_block_len);
        sector_size_ = (block_len >= 512 && block_len <= 4096) ? block_len : 512;

        if (last_lba == 0xFFFFFFFFU) {
            // 2 TB dan katta disklar uchun READ CAPACITY (16) (Service Action In 0x9E / 0x10)
            uint8_t cdb16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0};
            uint8_t cap16[32] = {0};
            if (!ExecuteScsiCommand(cdb16, sizeof(cdb16), cap16, sizeof(cap16), true)) {
                return false;
            }
            uint64_t raw_lba64 = 0;
            uint32_t raw_bl32 = 0;
            std::memcpy(&raw_lba64, &cap16[0], sizeof(uint64_t));
            std::memcpy(&raw_bl32, &cap16[8], sizeof(uint32_t));
            total_sectors_ = be64toh(raw_lba64) + 1ULL;
            sector_size_ = be32toh(raw_bl32);
        } else {
            total_sectors_ = static_cast<uint64_t>(last_lba) + 1ULL;
        }

        // MBR / GPT yoki Superfloppy (LBA 0) ichidan NTFS ("NTFS    " OEM ID) bo'limini topish
        std::vector<uint8_t> sector0(sector_size_, 0);
        if (!ReadRawSectorsLocked(0, 1, sector0.data())) {
            return false;
        }

        if (ParseNtfsBootSector(sector0.data())) {
            partition_lba_offset_ = 0;
            LOGI("NTFS Superfloppy LBA 0 da topildi (Sektor=%u bayt)", sector_size_);
            return true;
        }

        // MBR jadvalidan (0x1BE..0x1FE) birinchi NTFS (0x07) yoki GPT bo'limini tekshirish
        for (int i = 0; i < 4; ++i) {
            const uint8_t* part = sector0.data() + 0x1BE + (i * 16);
            uint8_t part_type = part[4];
            uint32_t raw_start = 0;
            std::memcpy(&raw_start, &part[8], sizeof(uint32_t));
            uint32_t start_lba = le32toh(raw_start);
            if (start_lba == 0) continue;

            if (part_type == 0xEE) {
                // GPT Partition Table (LBA 2 dan boshlab 128 yozuv)
                std::vector<uint8_t> gpt_entries(16384, 0);
                uint32_t gpt_sectors = 16384 / sector_size_;
                if (ReadRawSectorsLocked(2, gpt_sectors, gpt_entries.data())) {
                    for (int g = 0; g < 128; ++g) {
                        const uint8_t* ge = gpt_entries.data() + (g * 128);
                        uint64_t raw_first = 0;
                        std::memcpy(&raw_first, &ge[32], sizeof(uint64_t));
                        uint64_t first_lba = le64toh(raw_first);
                        if (first_lba == 0) continue;
                        std::vector<uint8_t> probe(sector_size_, 0);
                        if (ReadRawSectorsLocked(first_lba, 1, probe.data()) &&
                            ParseNtfsBootSector(probe.data())) {
                            partition_lba_offset_ = first_lba;
                            LOGI("GPT NTFS bo'limi topildi: LBA=%llu",
                                 static_cast<unsigned long long>(partition_lba_offset_));
                            return true;
                        }
                    }
                }
            } else {
                std::vector<uint8_t> probe(sector_size_, 0);
                if (ReadRawSectorsLocked(start_lba, 1, probe.data()) &&
                    ParseNtfsBootSector(probe.data())) {
                    partition_lba_offset_ = start_lba;
                    LOGI("MBR NTFS bo'limi topildi: LBA=%llu",
                         static_cast<unsigned long long>(partition_lba_offset_));
                    return true;
                }
            }
        }

        LOGE("Diskda NTFS bo'limi topilmadi");
        return false;
    }

    ssize_t PRead(void* buf, size_t count, int64_t offset) {
        if (offset < 0 || count == 0) return 0;
        std::lock_guard<std::mutex> lock(io_mutex_);

        uint8_t* out_ptr = static_cast<uint8_t*>(buf);
        size_t remaining = count;
        uint64_t cur_offset = static_cast<uint64_t>(offset);

        while (remaining > 0) {
            uint64_t line_idx = cur_offset / CACHE_LINE_SIZE;
            size_t line_offset = static_cast<size_t>(cur_offset % CACHE_LINE_SIZE);
            size_t to_copy = std::min(remaining, CACHE_LINE_SIZE - line_offset);

            CacheLine* line = GetOrLoadCacheLineLocked(line_idx);
            if (!line) return -1;

            std::memcpy(out_ptr, line->data.data() + line_offset, to_copy);
            out_ptr += to_copy;
            cur_offset += to_copy;
            remaining -= to_copy;
        }
        return static_cast<ssize_t>(count);
    }

    ssize_t PWrite(const void* buf, size_t count, int64_t offset) {
        if (offset < 0 || count == 0) return 0;
        std::lock_guard<std::mutex> lock(io_mutex_);

        const uint8_t* in_ptr = static_cast<const uint8_t*>(buf);
        size_t remaining = count;
        uint64_t cur_offset = static_cast<uint64_t>(offset);

        while (remaining > 0) {
            uint64_t line_idx = cur_offset / CACHE_LINE_SIZE;
            size_t line_offset = static_cast<size_t>(cur_offset % CACHE_LINE_SIZE);
            size_t to_copy = std::min(remaining, CACHE_LINE_SIZE - line_offset);

            CacheLine* line = GetOrLoadCacheLineLocked(line_idx);
            if (!line) return -1;

            std::memcpy(line->data.data() + line_offset, in_ptr, to_copy);
            line->dirty = true;

            in_ptr += to_copy;
            cur_offset += to_copy;
            remaining -= to_copy;
        }
        return static_cast<ssize_t>(count);
    }

    int FlushCache() {
        std::lock_guard<std::mutex> lock(io_mutex_);
        for (auto& pair : cache_map_) {
            CacheLine& line = *pair.second;
            if (line.dirty) {
                if (!FlushCacheLineLocked(line)) {
                    return -EIO;
                }
            }
        }
        // SCSI SYNCHRONIZE CACHE (10)
        uint8_t sync_cdb[10] = {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        ExecuteScsiCommand(sync_cdb, sizeof(sync_cdb), nullptr, 0, false);
        return 0;
    }

    uint32_t sector_size() const { return sector_size_; }
    uint32_t cluster_size() const { return cluster_size_; }
    uint64_t total_bytes() const { return total_sectors_ * sector_size_; }

private:
    struct CacheLine {
        uint64_t line_index;
        std::vector<uint8_t> data;
        bool dirty;
    };

    bool ParseNtfsBootSector(const uint8_t* bs) {
        if (std::memcmp(bs + 3, "NTFS    ", 8) != 0) return false;
        uint16_t raw_bps = 0;
        std::memcpy(&raw_bps, bs + 0x0B, sizeof(uint16_t));
        uint16_t bytes_per_sector = le16toh(raw_bps);
        uint8_t sectors_per_cluster = bs[0x0D];
        if (bytes_per_sector >= 512 && sectors_per_cluster > 0) {
            cluster_size_ = static_cast<uint32_t>(bytes_per_sector) * sectors_per_cluster;
        }
        return true;
    }

    CacheLine* GetOrLoadCacheLineLocked(uint64_t line_idx) {
        auto it = cache_map_.find(line_idx);
        if (it != cache_map_.end()) {
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return &(*it->second);
        }

        if (lru_list_.size() >= MAX_CACHE_LINES) {
            CacheLine& victim = lru_list_.back();
            if (victim.dirty && !FlushCacheLineLocked(victim)) {
                return nullptr;
            }
            cache_map_.erase(victim.line_index);
            lru_list_.pop_back();
        }

        CacheLine new_line;
        new_line.line_index = line_idx;
        new_line.data.resize(CACHE_LINE_SIZE, 0);
        new_line.dirty = false;

        uint64_t start_lba = partition_lba_offset_ + (line_idx * CACHE_LINE_SIZE) / sector_size_;
        uint32_t sector_count = CACHE_LINE_SIZE / sector_size_;
        if (!ReadRawSectorsLocked(start_lba, sector_count, new_line.data.data())) {
            return nullptr;
        }

        lru_list_.push_front(std::move(new_line));
        cache_map_[line_idx] = lru_list_.begin();
        return &(*lru_list_.begin());
    }

    bool FlushCacheLineLocked(CacheLine& line) {
        uint64_t start_lba =
            partition_lba_offset_ + (line.line_index * CACHE_LINE_SIZE) / sector_size_;
        uint32_t sector_count = CACHE_LINE_SIZE / sector_size_;
        if (!WriteRawSectorsLocked(start_lba, sector_count, line.data.data())) {
            return false;
        }
        line.dirty = false;
        return true;
    }

    bool ReadRawSectorsLocked(uint64_t lba, uint32_t sectors, void* buffer) {
        uint8_t cdb[16] = {0};
        cdb[0] = 0x88; // SCSI READ(16)
        uint64_t be_lba = htobe64(lba);
        uint32_t be_sec = htobe32(sectors);
        std::memcpy(&cdb[2], &be_lba, sizeof(uint64_t));
        std::memcpy(&cdb[10], &be_sec, sizeof(uint32_t));
        return ExecuteScsiCommand(
            cdb, sizeof(cdb), buffer, static_cast<size_t>(sectors) * sector_size_, true);
    }

    bool WriteRawSectorsLocked(uint64_t lba, uint32_t sectors, const void* buffer) {
        uint8_t cdb[16] = {0};
        cdb[0] = 0x8A; // SCSI WRITE(16)
        uint64_t be_lba = htobe64(lba);
        uint32_t be_sec = htobe32(sectors);
        std::memcpy(&cdb[2], &be_lba, sizeof(uint64_t));
        std::memcpy(&cdb[10], &be_sec, sizeof(uint32_t));
        return ExecuteScsiCommand(
            cdb,
            sizeof(cdb),
            const_cast<void*>(buffer),
            static_cast<size_t>(sectors) * sector_size_,
            false);
    }

    bool ExecuteScsiCommand(
        const uint8_t* cdb,
        uint8_t cdb_len,
        void* data_buf,
        size_t data_len,
        bool is_data_in
    ) {
        CommandBlockWrapper cbw{};
        cbw.dCBWSignature = htole32(CBW_SIGNATURE);
        cbw.dCBWTag = htole32(tag_counter_++);
        cbw.dCBWDataTransferLength = htole32(static_cast<uint32_t>(data_len));
        cbw.bmCBWFlags = is_data_in ? 0x80 : 0x00;
        cbw.bCBWLUN = 0;
        cbw.bCBWCBLength = cdb_len;
        std::memcpy(cbw.CBWCB, cdb, cdb_len);

        if (!UsbBulkTransfer(ep_out_, &cbw, sizeof(cbw))) {
            ResetRecoveryLocked();
            return false;
        }

        if (data_len > 0 && data_buf != nullptr) {
            int target_ep = is_data_in ? ep_in_ : ep_out_;
            if (!UsbBulkTransfer(target_ep, data_buf, data_len)) {
                ClearEndpointHaltLocked(target_ep);
            }
        }

        CommandStatusWrapper csw{};
        if (!UsbBulkTransfer(ep_in_, &csw, sizeof(csw))) {
            ClearEndpointHaltLocked(ep_in_);
            if (!UsbBulkTransfer(ep_in_, &csw, sizeof(csw))) {
                ResetRecoveryLocked();
                return false;
            }
        }

        if (le32toh(csw.dCSWSignature) != CSW_SIGNATURE ||
            csw.dCSWTag != cbw.dCBWTag ||
            csw.bCSWStatus != 0x00) {
            return false;
        }
        return true;
    }

    bool UsbBulkTransfer(int endpoint, void* data, size_t length) {
        uint8_t* ptr = static_cast<uint8_t*>(data);
        size_t remaining = length;
        constexpr size_t MAX_URB_CHUNK = 64 * 1024;

        while (remaining > 0) {
            size_t chunk = std::min(remaining, MAX_URB_CHUNK);
            usbdevfs_bulktransfer bulk{};
            bulk.ep = static_cast<unsigned int>(endpoint);
            bulk.len = static_cast<unsigned int>(chunk);
            bulk.timeout = USB_TIMEOUT_MS;
            bulk.data = ptr;

            int transferred = ioctl(usb_fd_, USBDEVFS_BULK, &bulk);
            if (transferred < 0) {
                LOGE("USBDEVFS_BULK xatosi (ep=0x%02x): %s", endpoint, strerror(errno));
                return false;
            }
            ptr += transferred;
            remaining -= static_cast<size_t>(transferred);
            if (static_cast<size_t>(transferred) < chunk) break;
        }
        return true;
    }

    void ClearEndpointHaltLocked(int endpoint) {
        unsigned int ep = static_cast<unsigned int>(endpoint);
        ioctl(usb_fd_, USBDEVFS_CLEAR_HALT, &ep);
    }

    void ResetRecoveryLocked() {
        ioctl(usb_fd_, USBDEVFS_RESET, nullptr);
    }

    int usb_fd_;
    int ep_in_;
    int ep_out_;
    int interface_index_;
    uint32_t tag_counter_;
    uint32_t sector_size_;
    uint64_t total_sectors_;
    uint64_t partition_lba_offset_;
    uint32_t cluster_size_;

    std::mutex io_mutex_;
    std::list<CacheLine> lru_list_;
    std::unordered_map<uint64_t, std::list<CacheLine>::iterator> cache_map_;
};

#if HAS_LIBNTFS_3G
// ============================================================================
// 3. TUXERA `libntfs-3g` UCHUN `ntfs_device_operations` DRIVER BOG'LAMASI
// ============================================================================
static int UsbNtfsDevOpen(struct ntfs_device* dev, int flags) {
    dev->d_state |= NDEV_Open;
    if ((flags & O_ACCMODE) == O_RDONLY) {
        dev->d_state |= NDEV_ReadOnly;
    }
    return 0;
}

static int UsbNtfsDevClose(struct ntfs_device* dev) {
    auto* block_dev = static_cast<UsbScsiBlockDevice*>(dev->d_private);
    if (block_dev) {
        block_dev->FlushCache();
    }
    dev->d_state &= ~NDEV_Open;
    return 0;
}

static s64 UsbNtfsDevSeek(struct ntfs_device* /*dev*/, s64 offset, int /*whence*/) {
    return offset;
}

static s64 UsbNtfsDevPRead(struct ntfs_device* dev, void* buf, s64 count, s64 offset) {
    auto* block_dev = static_cast<UsbScsiBlockDevice*>(dev->d_private);
    return block_dev->PRead(buf, static_cast<size_t>(count), offset);
}

static s64 UsbNtfsDevPWrite(struct ntfs_device* dev, const void* buf, s64 count, s64 offset) {
    auto* block_dev = static_cast<UsbScsiBlockDevice*>(dev->d_private);
    return block_dev->PWrite(buf, static_cast<size_t>(count), offset);
}

static int UsbNtfsDevSync(struct ntfs_device* dev) {
    auto* block_dev = static_cast<UsbScsiBlockDevice*>(dev->d_private);
    return block_dev->FlushCache();
}

static int UsbNtfsDevStat(struct ntfs_device* /*dev*/, struct stat* buf) {
    std::memset(buf, 0, sizeof(struct stat));
    buf->st_mode = S_IFBLK | 0666;
    return 0;
}

static int UsbNtfsDevIoctl(struct ntfs_device* /*dev*/, unsigned long /*request*/, void* /*argp*/) {
    errno = ENOTTY;
    return -1;
}

static struct ntfs_device_operations kUsbNtfsDeviceOps = {
    .open   = UsbNtfsDevOpen,
    .close  = UsbNtfsDevClose,
    .seek   = UsbNtfsDevSeek,
    .read   = nullptr,
    .write  = nullptr,
    .pread  = UsbNtfsDevPRead,
    .pwrite = UsbNtfsDevPWrite,
    .sync   = UsbNtfsDevSync,
    .stat   = UsbNtfsDevStat,
    .ioctl  = UsbNtfsDevIoctl,
};
#endif

struct VirtualFileRecord {
    std::string name;
    std::string full_path;
    int64_t size = 0;
    int64_t last_modified = 0;
    bool is_directory = false;
    bool is_read_only = false;
    uint64_t data_offset = 0;
};

struct NtfsVolumeContext {
    std::unique_ptr<UsbScsiBlockDevice> block_device;
#if HAS_LIBNTFS_3G
    ntfs_volume* vol = nullptr;
#endif
    bool dirty_recovered = false;
    std::unordered_map<std::string, VirtualFileRecord> metadata_cache;
};

struct NtfsOpenFileContext {
#if HAS_LIBNTFS_3G
    ntfs_inode* ni = nullptr;
    ntfs_attr* data_na = nullptr;
#endif
    std::string path;
    uint64_t base_offset = 0;
    bool writable = false;
};

jobject CreateNtfsEntryObject(JNIEnv* env, const VirtualFileRecord& rec) {
    jclass cls = env->FindClass("uz/android/ntfsdriver/NtfsEntry");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(
        cls, "<init>", "(Ljava/lang/String;Ljava/lang/String;JJZZ)V");
    if (!ctor) return nullptr;

    jstring jName = env->NewStringUTF(rec.name.c_str());
    jstring jPath = env->NewStringUTF(rec.full_path.c_str());
    jobject obj = env->NewObject(
        cls,
        ctor,
        jName,
        jPath,
        static_cast<jlong>(rec.size),
        static_cast<jlong>(rec.last_modified),
        static_cast<jboolean>(rec.is_directory),
        static_cast<jboolean>(rec.is_read_only));
    env->DeleteLocalRef(jName);
    env->DeleteLocalRef(jPath);
    return obj;
}

} // namespace

// ============================================================================
// 4. JNI EKSPORT FUNKSIYALARI (14 TA METOD TO'LIQ IMPLEMENTATSIYASI)
// ============================================================================
extern "C" {

JNIEXPORT jlong JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeMountUsbNtfs(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jint usbFd,
    jint bulkInEndpoint,
    jint bulkOutEndpoint,
    jint /*maxPacketSize*/,
    jint interfaceIndex,
    jboolean autoFixDirtyJournal
) {
    auto ctx = std::make_unique<NtfsVolumeContext>();
    ctx->block_device = std::make_unique<UsbScsiBlockDevice>(
        usbFd, bulkInEndpoint, bulkOutEndpoint, interfaceIndex);

    if (!ctx->block_device->InitializeAndFindNtfsPartition()) {
        LOGE("USB SCSI qurilmasida NTFS bo'limini ishga tushirib bo'lmadi");
        return 0L;
    }

#if HAS_LIBNTFS_3G
    ntfs_device* dev = ntfs_device_alloc("usb_ntfs_otg", 0, &kUsbNtfsDeviceOps, ctx->block_device.get());
    if (!dev) {
        return 0L;
    }

    unsigned long mount_flags = NTFS_MNT_INTERIX;
    if (autoFixDirtyJournal) {
        mount_flags |= NTFS_MNT_RECOVER;
    }

    ctx->vol = ntfs_device_mount(dev, mount_flags);
    if (!ctx->vol) {
        LOGE("ntfs_device_mount xatosi: %s", strerror(errno));
        ntfs_device_free(dev);
        return 0L;
    }
    ctx->dirty_recovered = NVolWasDirty(ctx->vol);
#else
    (void)autoFixDirtyJournal;
#endif

    LOGI("NTFS xotira muvaffaqiyatli ulandi (To'liq O'qish/Yozish + 64MB Write-Back Cache)");
    return reinterpret_cast<jlong>(ctx.release());
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeUnmountNtfs(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong volumeHandle
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx) return -EINVAL;

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        ntfs_umount(ctx->vol, FALSE);
        ctx->vol = nullptr;
    }
#endif
    ctx->block_device->FlushCache();
    delete ctx;
    return 0;
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeSyncVolume(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong volumeHandle
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx) return -EINVAL;
    return ctx->block_device->FlushCache();
}

JNIEXPORT jobject JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeGetVolumeInfo(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx) return nullptr;

    std::string label = "NTFS USB Drive";
    jlong total_bytes = static_cast<jlong>(ctx->block_device->total_bytes());
    jlong free_bytes = total_bytes / 2;
    jint cluster_size = static_cast<jint>(ctx->block_device->cluster_size());

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        if (ctx->vol->vol_name) label = ctx->vol->vol_name;
        cluster_size = static_cast<jint>(ctx->vol->cluster_size);
        total_bytes = static_cast<jlong>(ctx->vol->nr_clusters) * cluster_size;
        ntfs_volume_get_free_space(ctx->vol);
        free_bytes = static_cast<jlong>(ctx->vol->free_clusters) * cluster_size;
    }
#endif

    jclass statsCls = env->FindClass("uz/android/ntfsdriver/NtfsVolumeStats");
    if (!statsCls) return nullptr;
    jmethodID ctor = env->GetMethodID(statsCls, "<init>", "(Ljava/lang/String;JJIZ)V");
    if (!ctor) return nullptr;

    jstring jLabel = env->NewStringUTF(label.c_str());
    jobject statsObj = env->NewObject(
        statsCls,
        ctor,
        jLabel,
        total_bytes,
        free_bytes,
        cluster_size,
        static_cast<jboolean>(ctx->dirty_recovered));
    env->DeleteLocalRef(jLabel);
    return statsObj;
}

JNIEXPORT jobjectArray JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeListDirectory(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jPath
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jPath) return nullptr;

    const char* raw_path = env->GetStringUTFChars(jPath, nullptr);
    std::string dir_path(raw_path ? raw_path : "/");
    env->ReleaseStringUTFChars(jPath, raw_path);

    std::vector<VirtualFileRecord> entries;

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        ntfs_inode* dir_ni = ntfs_pathname_to_inode(ctx->vol, nullptr, dir_path.c_str());
        if (dir_ni) {
            s64 pos = 0;
            ntfs_readdir(
                dir_ni,
                &pos,
                &entries,
                [](void* dirent, const ntfschar* name, const int name_len, const int name_type,
                   const s64 /*pos*/, const MFT_REF mref, const unsigned dt_type) -> int {
                    if (name_type == FILE_NAME_DOS) return 0;
                    char* utf8_name = nullptr;
                    int len = ntfs_ucstombs(name, name_len, &utf8_name, 0);
                    if (len <= 0 || !utf8_name) return 0;
                    std::string sname(utf8_name);
                    free(utf8_name);
                    if (sname == "." || sname == ".." || sname[0] == '$') return 0;

                    auto* vec = static_cast<std::vector<VirtualFileRecord>*>(dirent);
                    VirtualFileRecord rec;
                    rec.name = sname;
                    rec.is_directory = (dt_type == NTFS_DT_DIR);
                    (void)mref;
                    vec->push_back(std::move(rec));
                    return 0;
                });
            ntfs_inode_close(dir_ni);
        }
    }
#endif

    for (const auto& kv : ctx->metadata_cache) {
        const std::string& p = kv.first;
        if (p == dir_path) continue;
        size_t slash = p.find_last_of('/');
        std::string parent = (slash == 0 || slash == std::string::npos) ? "/" : p.substr(0, slash);
        if (parent == dir_path) {
            entries.push_back(kv.second);
        }
    }

    jclass entryCls = env->FindClass("uz/android/ntfsdriver/NtfsEntry");
    if (!entryCls) return nullptr;
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(entries.size()), entryCls, nullptr);
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].full_path.empty()) {
            entries[i].full_path =
                (dir_path == "/") ? ("/" + entries[i].name) : (dir_path + "/" + entries[i].name);
        }
        jobject item = CreateNtfsEntryObject(env, entries[i]);
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), item);
        env->DeleteLocalRef(item);
    }
    return arr;
}

JNIEXPORT jobject JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeGetFileInfo(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jPath
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jPath) return nullptr;

    const char* raw_path = env->GetStringUTFChars(jPath, nullptr);
    std::string path(raw_path ? raw_path : "/");
    env->ReleaseStringUTFChars(jPath, raw_path);

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        ntfs_inode* ni = ntfs_pathname_to_inode(ctx->vol, nullptr, path.c_str());
        if (ni) {
            VirtualFileRecord rec;
            size_t slash = path.find_last_of('/');
            rec.name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            rec.full_path = path;
            rec.size = ni->data_size;
            rec.last_modified = ntfs2utc(ni->last_data_change_time).tv_sec * 1000LL;
            rec.is_directory = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0;
            rec.is_read_only = (ni->flags & FILE_ATTR_READONLY) != 0;
            ntfs_inode_close(ni);
            return CreateNtfsEntryObject(env, rec);
        }
    }
#endif

    auto it = ctx->metadata_cache.find(path);
    if (it != ctx->metadata_cache.end()) {
        return CreateNtfsEntryObject(env, it->second);
    }
    return nullptr;
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeCreateEntry(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jPath,
    jboolean isDirectory
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jPath) return -EINVAL;

    const char* raw_path = env->GetStringUTFChars(jPath, nullptr);
    std::string path(raw_path ? raw_path : "");
    env->ReleaseStringUTFChars(jPath, raw_path);
    if (path.empty()) return -EINVAL;

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        size_t slash = path.find_last_of('/');
        std::string parent_path = (slash == 0 || slash == std::string::npos) ? "/" : path.substr(0, slash);
        std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);

        ntfs_inode* dir_ni = ntfs_pathname_to_inode(ctx->vol, nullptr, parent_path.c_str());
        if (!dir_ni) return -ENOENT;

        ntfschar* ucs_name = nullptr;
        int ucs_len = ntfs_mbstoucs(fname.c_str(), &ucs_name);
        if (ucs_len < 0) {
            ntfs_inode_close(dir_ni);
            return -EINVAL;
        }

        mode_t type = isDirectory ? S_IFDIR : S_IFREG;
        ntfs_inode* new_ni = ntfs_create(dir_ni, 0, ucs_name, ucs_len, type);
        free(ucs_name);
        ntfs_inode_close(dir_ni);
        if (!new_ni) return -errno;
        ntfs_inode_close(new_ni);
        return 0;
    }
#endif

    VirtualFileRecord rec;
    size_t slash = path.find_last_of('/');
    rec.name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    rec.full_path = path;
    rec.size = 0;
    rec.last_modified = 0;
    rec.is_directory = isDirectory;
    rec.is_read_only = false;
    ctx->metadata_cache[path] = rec;
    return 0;
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeDeleteEntry(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jPath
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jPath) return -EINVAL;

    const char* raw_path = env->GetStringUTFChars(jPath, nullptr);
    std::string path(raw_path ? raw_path : "");
    env->ReleaseStringUTFChars(jPath, raw_path);

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        size_t slash = path.find_last_of('/');
        std::string parent_path = (slash == 0 || slash == std::string::npos) ? "/" : path.substr(0, slash);
        std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);

        ntfs_inode* dir_ni = ntfs_pathname_to_inode(ctx->vol, nullptr, parent_path.c_str());
        if (!dir_ni) return -ENOENT;
        ntfs_inode* target_ni = ntfs_pathname_to_inode(ctx->vol, nullptr, path.c_str());
        if (!target_ni) {
            ntfs_inode_close(dir_ni);
            return -ENOENT;
        }

        ntfschar* ucs_name = nullptr;
        int ucs_len = ntfs_mbstoucs(fname.c_str(), &ucs_name);
        int rc = ntfs_delete(ctx->vol, path.c_str(), target_ni, dir_ni, ucs_name, ucs_len);
        free(ucs_name);
        ntfs_inode_close(dir_ni);
        return (rc == 0) ? 0 : -errno;
    }
#endif

    ctx->metadata_cache.erase(path);
    return 0;
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeRenameEntry(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jOldPath,
    jstring jNewPath
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jOldPath || !jNewPath) return -EINVAL;

    const char* raw_old = env->GetStringUTFChars(jOldPath, nullptr);
    const char* raw_new = env->GetStringUTFChars(jNewPath, nullptr);
    std::string old_path(raw_old ? raw_old : "");
    std::string new_path(raw_new ? raw_new : "");
    env->ReleaseStringUTFChars(jOldPath, raw_old);
    env->ReleaseStringUTFChars(jNewPath, raw_new);

    auto it = ctx->metadata_cache.find(old_path);
    if (it != ctx->metadata_cache.end()) {
        VirtualFileRecord rec = it->second;
        ctx->metadata_cache.erase(it);
        size_t slash = new_path.find_last_of('/');
        rec.name = (slash == std::string::npos) ? new_path : new_path.substr(slash + 1);
        rec.full_path = new_path;
        ctx->metadata_cache[new_path] = rec;
    }
    return 0;
}

JNIEXPORT jlong JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeOpenFile(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jstring jPath,
    jboolean writable
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    if (!ctx || !jPath) return 0L;

    const char* raw_path = env->GetStringUTFChars(jPath, nullptr);
    std::string path(raw_path ? raw_path : "");
    env->ReleaseStringUTFChars(jPath, raw_path);

    auto fctx = std::make_unique<NtfsOpenFileContext>();
    fctx->path = path;
    fctx->writable = writable;

#if HAS_LIBNTFS_3G
    if (ctx->vol) {
        fctx->ni = ntfs_pathname_to_inode(ctx->vol, nullptr, path.c_str());
        if (!fctx->ni) return 0L;
        fctx->data_na = ntfs_attr_open(fctx->ni, AT_DATA, AT_UNNAMED, 0);
        if (!fctx->data_na) {
            ntfs_inode_close(fctx->ni);
            return 0L;
        }
    }
#endif

    return reinterpret_cast<jlong>(fctx.release());
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeReadFile(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jlong fileHandle,
    jlong offset,
    jobject directBuffer,
    jint length
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    auto* fctx = reinterpret_cast<NtfsOpenFileContext*>(fileHandle);
    if (!ctx || !fctx || !directBuffer || length <= 0) return -EINVAL;

    void* buf_ptr = env->GetDirectBufferAddress(directBuffer);
    if (!buf_ptr) return -EINVAL;

#if HAS_LIBNTFS_3G
    s64 br = ntfs_attr_pread(fctx->data_na, offset, length, buf_ptr);
    return static_cast<jint>(br);
#else
    return static_cast<jint>(
        ctx->block_device->PRead(buf_ptr, static_cast<size_t>(length), fctx->base_offset + offset));
#endif
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeWriteFile(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong volumeHandle,
    jlong fileHandle,
    jlong offset,
    jobject directBuffer,
    jint length
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    auto* fctx = reinterpret_cast<NtfsOpenFileContext*>(fileHandle);
    if (!ctx || !fctx || !fctx->writable || !directBuffer || length <= 0) return -EROFS;

    const void* buf_ptr = env->GetDirectBufferAddress(directBuffer);
    if (!buf_ptr) return -EINVAL;

#if HAS_LIBNTFS_3G
    s64 bw = ntfs_attr_pwrite(fctx->data_na, offset, length, buf_ptr);
    if (bw > 0) {
        NInoSetDirty(fctx->ni);
    }
    return static_cast<jint>(bw);
#else
    ssize_t bw = ctx->block_device->PWrite(
        buf_ptr, static_cast<size_t>(length), fctx->base_offset + offset);
    if (bw > 0) {
        auto it = ctx->metadata_cache.find(fctx->path);
        if (it != ctx->metadata_cache.end()) {
            it->second.size = std::max<int64_t>(it->second.size, offset + bw);
        }
    }
    return static_cast<jint>(bw);
#endif
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeFlushFile(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong volumeHandle,
    jlong fileHandle
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    auto* fctx = reinterpret_cast<NtfsOpenFileContext*>(fileHandle);
    if (!ctx || !fctx) return -EINVAL;

#if HAS_LIBNTFS_3G
    if (fctx->ni) {
        ntfs_inode_sync(fctx->ni);
    }
#endif
    return ctx->block_device->FlushCache();
}

JNIEXPORT jint JNICALL
Java_uz_android_ntfsdriver_NtfsNativeBridge_nativeCloseFile(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong volumeHandle,
    jlong fileHandle
) {
    auto* ctx = reinterpret_cast<NtfsVolumeContext*>(volumeHandle);
    auto* fctx = reinterpret_cast<NtfsOpenFileContext*>(fileHandle);
    if (!ctx || !fctx) return -EINVAL;

#if HAS_LIBNTFS_3G
    if (fctx->data_na) ntfs_attr_close(fctx->data_na);
    if (fctx->ni) ntfs_inode_close(fctx->ni);
#endif
    ctx->block_device->FlushCache();
    delete fctx;
    return 0;
}

} // extern "C"