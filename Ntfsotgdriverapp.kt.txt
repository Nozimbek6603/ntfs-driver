/*
 * NtfsOtgDriverApp.kt
 * Android uchun yuqori tezlikdagi va barqaror NTFS USB-OTG drayveri, MainActivity boshqaruv oynasi
 * va DocumentsProvider tizimi (Yaxlit o'rnatiladigan dastur).
 */

package uz.android.ntfsdriver

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.database.MatrixCursor
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.ProxyFileDescriptorCallback
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.DocumentsContract.Root
import android.provider.DocumentsProvider
import android.system.ErrnoException
import android.system.OsConstants
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import android.webkit.MimeTypeMap
import java.io.FileNotFoundException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

// ============================================================================
// 0. ASOSIY DASTUR OYNASI (MAIN ACTIVITY - TELEFON EKRANIDAGI ILOVA)
// ============================================================================
class MainActivity : Activity() {
    private lateinit var statusTextView: TextView
    private val uiHandler = Handler(Looper.getMainLooper())
    private val refreshRunnable = object : Runnable {
        override fun run() {
            updateDriveStatusUi()
            uiHandler.postDelayed(this, 2000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val scrollView = ScrollView(this).apply {
            setBackgroundColor(Color.parseColor("#0F172A"))
            isFillViewport = true
        }

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val pad = dpToPx(20)
            setPadding(pad, pad, pad, pad)
        }

        val titleView = TextView(this).apply {
            text = "NTFS USB-OTG Drayver"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f)
            typeface = Typeface.DEFAULT_BOLD
        }

        val subtitleView = TextView(this).apply {
            text = "To'liq O'qish va Yozish (Read/Write) • 64 MB Kesh • Cheklanmagan Tezlik"
            setTextColor(Color.parseColor("#94A3B8"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            setPadding(0, dpToPx(6), 0, dpToPx(20))
        }

        statusTextView = TextView(this).apply {
            setTextColor(Color.parseColor("#E2E8F0"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            val pad = dpToPx(16)
            setPadding(pad, pad, pad, pad)
            background = GradientDrawable().apply {
                setColor(Color.parseColor("#1E293B"))
                cornerRadius = dpToPx(12).toFloat()
            }
        }

        val scanMountBtn = createStyledButton(
            text = "1. USB Xotirani Tekshirish va Ulash",
            bgColor = "#2563EB"
        ) {
            scanAndConnectUsbDevices()
        }

        val openFilesBtn = createStyledButton(
            text = "2. NTFS Diskdagi Fayllarni Ochish",
            bgColor = "#059669"
        ) {
            openSystemFileManager()
        }

        val safeEjectBtn = createStyledButton(
            text = "3. Xavfsiz Uzish (Keshni Saqlash)",
            bgColor = "#DC2626"
        ) {
            safelyEjectAllVolumes()
        }

        container.addView(titleView)
        container.addView(subtitleView)
        container.addView(statusTextView)
        container.addView(scanMountBtn)
        container.addView(openFilesBtn)
        container.addView(safeEjectBtn)
        scrollView.addView(container)
        setContentView(scrollView)

        updateDriveStatusUi()
    }

    override fun onResume() {
        super.onResume()
        uiHandler.post(refreshRunnable)
    }

    override fun onPause() {
        uiHandler.removeCallbacks(refreshRunnable)
        super.onPause()
    }

    private fun updateDriveStatusUi() {
        val usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        val connectedUsb = usbManager.deviceList.values.filter {
            NtfsUsbReceiver.isMassStorageDevice(it)
        }
        val mountedVolumes = NtfsVolumeRegistry.getAllVolumes()

        val sb = StringBuilder()
        sb.append("Joriy holat:\n\n")
        sb.append("• Ulangan USB Mass Storage qurilmalar: ${connectedUsb.size} ta\n")
        sb.append("• Faol NTFS bo'limlar (Read/Write): ${mountedVolumes.size} ta\n\n")

        if (mountedVolumes.isNotEmpty()) {
            for (vol in mountedVolumes) {
                val totalGb = String.format("%.2f GB", vol.stats.totalBytes / 1073741824.0)
                val freeGb = String.format("%.2f GB", vol.stats.freeBytes / 1073741824.0)
                sb.append("Disk: ${vol.deviceName}\n")
                sb.append("Hajmi: $freeGb bo'sh / $totalGb jami\n")
                sb.append("Klaster: ${vol.stats.clusterSize} bayt\n")
                sb.append("Rejim: To'liq O'qish/Yozish (64 MB Write-Back Cache faol)\n")
            }
        } else if (connectedUsb.isNotEmpty()) {
            sb.append("USB xotira topildi! '1. USB Xotirani Tekshirish va Ulash' tugmasini bosing.")
        } else {
            sb.append("Telefonga OTG orqali NTFS fleshka yoki tashqi SSD/HDD ulang.")
        }

        statusTextView.text = sb.toString()
    }

    private fun scanAndConnectUsbDevices() {
        val usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        val storageDevices = usbManager.deviceList.values.filter {
            NtfsUsbReceiver.isMassStorageDevice(it)
        }

        if (storageDevices.isEmpty()) {
            Toast.makeText(this, "USB xotira qurilmasi topilmadi. OTG ulanishini tekshiring.", Toast.LENGTH_LONG).show()
            return
        }

        for (device in storageDevices) {
            if (usbManager.hasPermission(device)) {
                val serviceIntent = Intent(this, NtfsDriverService::class.java).apply {
                    action = NtfsDriverService.ACTION_MOUNT_DEVICE
                    putExtra(UsbManager.EXTRA_DEVICE, device)
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    startForegroundService(serviceIntent)
                } else {
                    startService(serviceIntent)
                }
                Toast.makeText(this, "NTFS xotira ulanmoqda: ${device.productName ?: device.deviceName}", Toast.LENGTH_SHORT).show()
            } else {
                val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                } else {
                    PendingIntent.FLAG_UPDATE_CURRENT
                }
                val permIntent = PendingIntent.getBroadcast(
                    this,
                    device.deviceId,
                    Intent(NtfsUsbReceiver.ACTION_USB_PERMISSION).setPackage(packageName),
                    flags
                )
                usbManager.requestPermission(device, permIntent)
            }
        }
    }

    private fun openSystemFileManager() {
        try {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    val vols = NtfsVolumeRegistry.getAllVolumes()
                    if (vols.isNotEmpty()) {
                        val rootUri = DocumentsContract.buildRootUri(
                            NtfsVolumeRegistry.AUTHORITY,
                            "ntfs_usb_${vols.first().deviceId}"
                        )
                        putExtra(DocumentsContract.EXTRA_INITIAL_URI, rootUri)
                    }
                }
            }
            startActivity(intent)
        } catch (e: Exception) {
            val fallback = Intent(Intent.ACTION_GET_CONTENT).apply {
                type = "*/*"
                addCategory(Intent.CATEGORY_OPENABLE)
            }
            startActivity(Intent.createChooser(fallback, "NTFS Fayl Menejerni Ochish"))
        }
    }

    private fun safelyEjectAllVolumes() {
        val vols = NtfsVolumeRegistry.getAllVolumes().toList()
        if (vols.isEmpty()) {
            Toast.makeText(this, "Faol ulangan NTFS xotira yo'q", Toast.LENGTH_SHORT).show()
            return
        }
        for (vol in vols) {
            NtfsVolumeRegistry.unregisterAndClose("ntfs_usb_${vol.deviceId}")
        }
        contentResolver.notifyChange(
            DocumentsContract.buildRootsUri(NtfsVolumeRegistry.AUTHORITY),
            null
        )
        updateDriveStatusUi()
        Toast.makeText(this, "Barcha ma'lumotlar saqlandi. USB xotirani uzishingiz mumkin!", Toast.LENGTH_LONG).show()
    }

    private fun createStyledButton(text: String, bgColor: String, onClick: () -> Unit): Button {
        return Button(this).apply {
            this.text = text
            setTextColor(Color.WHITE)
            isAllCaps = false
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            background = GradientDrawable().apply {
                setColor(Color.parseColor(bgColor))
                cornerRadius = dpToPx(10).toFloat()
            }
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                topMargin = dpToPx(14)
            }
            layoutParams = lp
            setPadding(dpToPx(16), dpToPx(14), dpToPx(16), dpToPx(14))
            setOnClickListener { onClick() }
        }
    }

    private fun dpToPx(dp: Int): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp.toFloat(),
            resources.displayMetrics
        ).toInt()
    }
}

// ============================================================================
// 1. JNI / NDK BRIDGE (C++ `libntfs-3g` + Direct USB SCSI BOT)
// ============================================================================
object NtfsNativeBridge {
    init {
        System.loadLibrary("ntfs_jni_driver")
    }

    @JvmStatic
    external fun nativeMountUsbNtfs(
        usbFd: Int,
        bulkInEndpoint: Int,
        bulkOutEndpoint: Int,
        maxPacketSize: Int,
        interfaceIndex: Int,
        autoFixDirtyJournal: Boolean
    ): Long

    @JvmStatic
    external fun nativeUnmountNtfs(volumeHandle: Long): Int

    @JvmStatic
    external fun nativeSyncVolume(volumeHandle: Long): Int

    @JvmStatic
    external fun nativeGetVolumeInfo(volumeHandle: Long): NtfsVolumeStats?

    @JvmStatic
    external fun nativeListDirectory(volumeHandle: Long, path: String): Array<NtfsEntry>?

    @JvmStatic
    external fun nativeGetFileInfo(volumeHandle: Long, path: String): NtfsEntry?

    @JvmStatic
    external fun nativeCreateEntry(volumeHandle: Long, path: String, isDirectory: Boolean): Int

    @JvmStatic
    external fun nativeDeleteEntry(volumeHandle: Long, path: String): Int

    @JvmStatic
    external fun nativeRenameEntry(volumeHandle: Long, oldPath: String, newPath: String): Int

    @JvmStatic
    external fun nativeOpenFile(volumeHandle: Long, path: String, writable: Boolean): Long

    @JvmStatic
    external fun nativeReadFile(
        volumeHandle: Long,
        fileHandle: Long,
        offset: Long,
        directBuffer: ByteBuffer,
        length: Int
    ): Int

    @JvmStatic
    external fun nativeWriteFile(
        volumeHandle: Long,
        fileHandle: Long,
        offset: Long,
        directBuffer: ByteBuffer,
        length: Int
    ): Int

    @JvmStatic
    external fun nativeFlushFile(volumeHandle: Long, fileHandle: Long): Int

    @JvmStatic
    external fun nativeCloseFile(volumeHandle: Long, fileHandle: Long): Int
}

data class NtfsVolumeStats(
    val volumeLabel: String,
    val totalBytes: Long,
    val freeBytes: Long,
    val clusterSize: Int,
    val isDirtyRecovered: Boolean
)

data class NtfsEntry(
    val name: String,
    val fullPath: String,
    val size: Long,
    val lastModified: Long,
    val isDirectory: Boolean,
    val isReadOnly: Boolean
)

// ============================================================================
// 2. ACTIVE VOLUME REGISTRY (Thread-Safe Manager)
// ============================================================================
object NtfsVolumeRegistry {
    private const val TAG = "NtfsVolumeRegistry"
    const val AUTHORITY = "uz.android.ntfsdriver.documents"

    data class MountedVolume(
        val deviceId: Int,
        val deviceName: String,
        val volumeHandle: Long,
        val connection: UsbDeviceConnection,
        val usbInterface: UsbInterface,
        val stats: NtfsVolumeStats,
        val rwLock: ReentrantReadWriteLock = ReentrantReadWriteLock()
    )

    private val volumes = ConcurrentHashMap<String, MountedVolume>()

    fun registerVolume(rootId: String, volume: MountedVolume) {
        volumes[rootId] = volume
    }

    fun getVolume(rootId: String): MountedVolume? = volumes[rootId]

    fun getAllVolumes(): Collection<MountedVolume> = volumes.values

    fun unregisterAndClose(rootId: String) {
        val vol = volumes.remove(rootId) ?: return
        vol.rwLock.write {
            try {
                NtfsNativeBridge.nativeSyncVolume(vol.volumeHandle)
                NtfsNativeBridge.nativeUnmountNtfs(vol.volumeHandle)
            } catch (e: Exception) {
                Log.e(TAG, "Unmount xatosi: ${e.message}", e)
            } finally {
                try {
                    vol.connection.releaseInterface(vol.usbInterface)
                    vol.connection.close()
                } catch (ignored: Exception) {
                }
            }
        }
    }
}

// ============================================================================
// 3. USB OTG ATTACH / DETACH BROADCAST RECEIVER
// ============================================================================
class NtfsUsbReceiver : BroadcastReceiver() {
    companion object {
        const val ACTION_USB_PERMISSION = "uz.android.ntfsdriver.USB_PERMISSION"
        private const val TAG = "NtfsUsbReceiver"

        fun isMassStorageDevice(device: UsbDevice): Boolean {
            for (i in 0 until device.interfaceCount) {
                val intf = device.getInterface(i)
                if (intf.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                    intf.interfaceSubclass == 0x06 &&
                    (intf.interfaceProtocol == 0x50 || intf.interfaceProtocol == 0x62)
                ) {
                    return true
                }
            }
            return false
        }
    }

    override fun onReceive(context: Context, intent: Intent) {
        val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        when (intent.action) {
            UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                val device = getUsbDeviceFromIntent(intent) ?: return
                if (!isMassStorageDevice(device)) return

                if (usbManager.hasPermission(device)) {
                    startDriverService(context, device, NtfsDriverService.ACTION_MOUNT_DEVICE)
                } else {
                    val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
                    } else {
                        PendingIntent.FLAG_UPDATE_CURRENT
                    }
                    val permIntent = PendingIntent.getBroadcast(
                        context,
                        device.deviceId,
                        Intent(ACTION_USB_PERMISSION).setPackage(context.packageName),
                        flags
                    )
                    usbManager.requestPermission(device, permIntent)
                }
            }

            ACTION_USB_PERMISSION -> {
                val device = getUsbDeviceFromIntent(intent) ?: return
                val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                if (granted && isMassStorageDevice(device)) {
                    startDriverService(context, device, NtfsDriverService.ACTION_MOUNT_DEVICE)
                } else {
                    Log.w(TAG, "USB ruxsati berilmadi: ${device.deviceName}")
                }
            }

            UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                val device = getUsbDeviceFromIntent(intent) ?: return
                startDriverService(context, device, NtfsDriverService.ACTION_UNMOUNT_DEVICE)
            }
        }
    }

    private fun getUsbDeviceFromIntent(intent: Intent): UsbDevice? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }
    }

    private fun startDriverService(context: Context, device: UsbDevice, action: String) {
        val serviceIntent = Intent(context, NtfsDriverService::class.java).apply {
            this.action = action
            putExtra(UsbManager.EXTRA_DEVICE, device)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(serviceIntent)
        } else {
            context.startService(serviceIntent)
        }
    }
}

// ============================================================================
// 4. FOREGROUND DRIVER SERVICE (Auto-Mount, Root Fast-Path & Periodic Sync)
// ============================================================================
class NtfsDriverService : Service() {
    companion object {
        const val ACTION_MOUNT_DEVICE = "uz.android.ntfsdriver.ACTION_MOUNT_DEVICE"
        const val ACTION_UNMOUNT_DEVICE = "uz.android.ntfsdriver.ACTION_UNMOUNT_DEVICE"
        private const val CHANNEL_ID = "ntfs_driver_channel"
        private const val NOTIFICATION_ID = 4096
        private const val TAG = "NtfsDriverService"
    }

    private lateinit var workerThread: HandlerThread
    private lateinit var workerHandler: Handler

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, buildNotification("NTFS drayver xizmati tayyor"))

        workerThread = HandlerThread("NtfsMountWorker").apply { start() }
        workerHandler = Handler(workerThread.looper)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val action = intent?.action ?: return START_STICKY
        val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        } ?: return START_STICKY

        workerHandler.post {
            when (action) {
                ACTION_MOUNT_DEVICE -> mountDevice(device)
                ACTION_UNMOUNT_DEVICE -> unmountDevice(device)
            }
        }
        return START_STICKY
    }

    private fun mountDevice(device: UsbDevice) {
        val rootId = "ntfs_usb_${device.deviceId}"
        if (NtfsVolumeRegistry.getVolume(rootId) != null) {
            Log.i(TAG, "Qurilma allaqachon ulangan: $rootId")
            return
        }

        if (tryKernelNtfs3MountIfRooted(device)) {
            updateNotification("NTFS (Kernel ntfs3) ulandi: ${device.productName ?: device.deviceName}")
            return
        }

        val usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        var massStorageInterface: UsbInterface? = null
        var bulkIn: UsbEndpoint? = null
        var bulkOut: UsbEndpoint? = null

        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            if (intf.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
                intf.interfaceSubclass == 0x06
            ) {
                var inEp: UsbEndpoint? = null
                var outEp: UsbEndpoint? = null
                for (epIdx in 0 until intf.endpointCount) {
                    val ep = intf.getEndpoint(epIdx)
                    if (ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                        if (ep.direction == UsbConstants.USB_DIR_IN) inEp = ep
                        else if (ep.direction == UsbConstants.USB_DIR_OUT) outEp = ep
                    }
                }
                if (inEp != null && outEp != null) {
                    massStorageInterface = intf
                    bulkIn = inEp
                    bulkOut = outEp
                    break
                }
            }
        }

        if (massStorageInterface == null || bulkIn == null || bulkOut == null) {
            Log.e(TAG, "USB Mass Storage Bulk endpointlari topilmadi")
            return
        }

        val connection = usbManager.openDevice(device) ?: run {
            Log.e(TAG, "USB qurilmani ochib bo'lmadi")
            return
        }

        if (!connection.claimInterface(massStorageInterface, true)) {
            Log.e(TAG, "USB interfeysni band qilib bo'lmadi")
            connection.close()
            return
        }

        val volumeHandle = NtfsNativeBridge.nativeMountUsbNtfs(
            usbFd = connection.fileDescriptor,
            bulkInEndpoint = bulkIn.address,
            bulkOutEndpoint = bulkOut.address,
            maxPacketSize = bulkOut.maxPacketSize,
            interfaceIndex = massStorageInterface.id,
            autoFixDirtyJournal = true
        )

        if (volumeHandle == 0L) {
            Log.e(TAG, "NTFS bo'limini ulash muvaffaqiyatsiz tugadi")
            connection.releaseInterface(massStorageInterface)
            connection.close()
            return
        }

        val stats = NtfsNativeBridge.nativeGetVolumeInfo(volumeHandle)
            ?: NtfsVolumeStats(
                volumeLabel = device.productName ?: "NTFS USB Drive",
                totalBytes = 0L,
                freeBytes = 0L,
                clusterSize = 4096,
                isDirtyRecovered = false
            )

        val mounted = NtfsVolumeRegistry.MountedVolume(
            deviceId = device.deviceId,
            deviceName = stats.volumeLabel.ifBlank { device.productName ?: "NTFS Drive" },
            volumeHandle = volumeHandle,
            connection = connection,
            usbInterface = massStorageInterface,
            stats = stats
        )

        NtfsVolumeRegistry.registerVolume(rootId, mounted)
        notifyRootsChanged()
        updateNotification("NTFS To'liq (R/W) ulandi: ${mounted.deviceName}")
    }

    private fun unmountDevice(device: UsbDevice) {
        val rootId = "ntfs_usb_${device.deviceId}"
        NtfsVolumeRegistry.unregisterAndClose(rootId)
        notifyRootsChanged()

        if (NtfsVolumeRegistry.getAllVolumes().isEmpty()) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        } else {
            updateNotification("NTFS faol: ${NtfsVolumeRegistry.getAllVolumes().size} ta xotira ulangan")
        }
    }

    private fun tryKernelNtfs3MountIfRooted(device: UsbDevice): Boolean {
        return try {
            val checkSu = Runtime.getRuntime().exec(arrayOf("su", "-c", "id"))
            if (checkSu.waitFor() != 0) return false

            val mountPoint = "/mnt/runtime/default/ntfs_${device.deviceId}"
            val script = """
                mkdir -p $mountPoint
                for blk in /dev/block/sd*[0-9]; do
                    if blkid "${'$'}blk" | grep -qi 'TYPE="ntfs"'; then
                        mount -t ntfs3 -o rw,noatime,iocharset=utf8,prealloc,umask=0000 "${'$'}blk" $mountPoint || \
                        ntfs-3g -o rw,noatime,big_writes,utf8,umask=0000 "${'$'}blk" $mountPoint
                        exit 0
                    fi
                done
                exit 1
            """.trimIndent()
            val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", script))
            proc.waitFor() == 0
        } catch (e: Exception) {
            false
        }
    }

    private fun notifyRootsChanged() {
        contentResolver.notifyChange(
            DocumentsContract.buildRootsUri(NtfsVolumeRegistry.AUTHORITY),
            null
        )
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "NTFS USB OTG Drayver",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "NTFS xotira qurilmalarining barqaror ishlashini ta'minlaydi"
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(text: String): Notification {
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("NTFS USB-OTG Drayver")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_usb)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIFICATION_ID, buildNotification(text))
    }

    override fun onDestroy() {
        for (vol in NtfsVolumeRegistry.getAllVolumes()) {
            NtfsVolumeRegistry.unregisterAndClose("ntfs_usb_${vol.deviceId}")
        }
        workerThread.quitSafely()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}

// ============================================================================
// 5. DOCUMENTS PROVIDER + ZERO-COPY PROXY FILE DESCRIPTOR (SAF INTEGRATION)
// ============================================================================
class NtfsDocumentsProvider : DocumentsProvider() {
    companion object {
        private val DEFAULT_ROOT_PROJECTION = arrayOf(
            Root.COLUMN_ROOT_ID,
            Root.COLUMN_FLAGS,
            Root.COLUMN_ICON,
            Root.COLUMN_TITLE,
            Root.COLUMN_SUMMARY,
            Root.COLUMN_DOCUMENT_ID,
            Root.COLUMN_AVAILABLE_BYTES,
            Root.COLUMN_CAPACITY_BYTES
        )

        private val DEFAULT_DOCUMENT_PROJECTION = arrayOf(
            Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_MIME_TYPE,
            Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_LAST_MODIFIED,
            Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE
        )
    }

    private lateinit var ioThread: HandlerThread
    private lateinit var ioHandler: Handler

    override fun onCreate(): Boolean {
        ioThread = HandlerThread("NtfsProxyIoThread").apply { start() }
        ioHandler = Handler(ioThread.looper)
        return true
    }

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)
        for (vol in NtfsVolumeRegistry.getAllVolumes()) {
            val rootId = "ntfs_usb_${vol.deviceId}"
            val stats = NtfsNativeBridge.nativeGetVolumeInfo(vol.volumeHandle) ?: vol.stats
            result.newRow().apply {
                add(Root.COLUMN_ROOT_ID, rootId)
                add(
                    Root.COLUMN_FLAGS,
                    Root.FLAG_SUPPORTS_CREATE or
                        Root.FLAG_SUPPORTS_IS_CHILD or
                        Root.FLAG_LOCAL_ONLY
                )
                add(Root.COLUMN_ICON, android.R.drawable.stat_sys_data_usb)
                add(Root.COLUMN_TITLE, vol.deviceName)
                add(Root.COLUMN_SUMMARY, "NTFS (To'liq O'qish/Yozish)")
                add(Root.COLUMN_DOCUMENT_ID, "$rootId:/")
                add(Root.COLUMN_AVAILABLE_BYTES, stats.freeBytes)
                add(Root.COLUMN_CAPACITY_BYTES, stats.totalBytes)
            }
        }
        return result
    }

    override fun queryDocument(documentId: String, projection: Array<out String>?): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val (vol, path) = resolveDocumentId(documentId)
        val entry = vol.rwLock.read {
            if (path == "/") {
                NtfsEntry(
                    name = vol.deviceName,
                    fullPath = "/",
                    size = 0L,
                    lastModified = System.currentTimeMillis(),
                    isDirectory = true,
                    isReadOnly = false
                )
            } else {
                NtfsNativeBridge.nativeGetFileInfo(vol.volumeHandle, path)
            }
        } ?: throw FileNotFoundException("Fayl topilmadi: $documentId")

        includeEntry(result, "ntfs_usb_${vol.deviceId}", entry)
        return result
    }

    override fun queryChildDocuments(
        parentDocumentId: String,
        projection: Array<out String>?,
        sortOrder: String?
    ): Cursor {
        val result = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        val (vol, parentPath) = resolveDocumentId(parentDocumentId)
        val rootId = "ntfs_usb_${vol.deviceId}"

        val entries = vol.rwLock.read {
            NtfsNativeBridge.nativeListDirectory(vol.volumeHandle, parentPath)
        } ?: emptyArray()

        for (entry in entries) {
            includeEntry(result, rootId, entry)
        }
        return result
    }

    override fun createDocument(
        parentDocumentId: String,
        mimeType: String,
        displayName: String
    ): String {
        val (vol, parentPath) = resolveDocumentId(parentDocumentId)
        val isDir = Document.MIME_TYPE_DIR == mimeType
        val cleanParent = if (parentPath.endsWith("/")) parentPath.dropLast(1) else parentPath
        val newPath = "$cleanParent/$displayName"

        val status = vol.rwLock.write {
            NtfsNativeBridge.nativeCreateEntry(vol.volumeHandle, newPath, isDir)
        }
        if (status != 0) {
            throw FileNotFoundException("Yangi fayl/papka yaratib bo'lmadi: $newPath (kod=$status)")
        }
        return "ntfs_usb_${vol.deviceId}:$newPath"
    }

    override fun deleteDocument(documentId: String) {
        val (vol, path) = resolveDocumentId(documentId)
        val status = vol.rwLock.write {
            NtfsNativeBridge.nativeDeleteEntry(vol.volumeHandle, path)
        }
        if (status != 0) {
            throw FileNotFoundException("O'chirib bo'lmadi: $path (kod=$status)")
        }
    }

    override fun renameDocument(documentId: String, displayName: String): String {
        val (vol, oldPath) = resolveDocumentId(documentId)
        val parentDir = oldPath.substringBeforeLast('/', "").ifEmpty { "/" }
        val newPath = if (parentDir == "/") "/$displayName" else "$parentDir/$displayName"

        val status = vol.rwLock.write {
            NtfsNativeBridge.nativeRenameEntry(vol.volumeHandle, oldPath, newPath)
        }
        if (status != 0) {
            throw FileNotFoundException("Nomini o'zgartirib bo'lmadi: $oldPath -> $newPath")
        }
        return "ntfs_usb_${vol.deviceId}:$newPath"
    }

    override fun isChildDocument(parentDocumentId: String, documentId: String): Boolean {
        val (parentVol, parentPath) = resolveDocumentId(parentDocumentId)
        val (childVol, childPath) = resolveDocumentId(documentId)
        if (parentVol.deviceId != childVol.deviceId) return false
        return childPath.startsWith(if (parentPath.endsWith("/")) parentPath else "$parentPath/")
    }

    override fun openDocument(
        documentId: String,
        mode: String,
        signal: CancellationSignal?
    ): ParcelFileDescriptor {
        val (vol, path) = resolveDocumentId(documentId)
        val parcelMode = ParcelFileDescriptor.parseMode(mode)
        val isWritable = mode.contains("w") || mode.contains("r+")

        val fileHandle = vol.rwLock.write {
            NtfsNativeBridge.nativeOpenFile(vol.volumeHandle, path, isWritable)
        }
        if (fileHandle == 0L) {
            throw FileNotFoundException("Faylni ochib bo'lmadi: $path")
        }

        val storageManager = context!!.getSystemService(Context.STORAGE_SERVICE) as StorageManager
        return storageManager.openProxyFileDescriptor(
            parcelMode,
            object : ProxyFileDescriptorCallback() {
                private var directBuf: ByteBuffer =
                    ByteBuffer.allocateDirect(1024 * 1024).order(ByteOrder.nativeOrder())

                private fun ensureBufferCapacity(size: Int): ByteBuffer {
                    if (directBuf.capacity() < size) {
                        directBuf = ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder())
                    }
                    directBuf.clear()
                    return directBuf
                }

                override fun onGetSize(): Long {
                    return vol.rwLock.read {
                        NtfsNativeBridge.nativeGetFileInfo(vol.volumeHandle, path)?.size ?: 0L
                    }
                }

                override fun onRead(offset: Long, size: Int, data: ByteArray): Int {
                    if (size <= 0) return 0
                    val buf = ensureBufferCapacity(size)
                    val bytesRead = vol.rwLock.read {
                        NtfsNativeBridge.nativeReadFile(
                            vol.volumeHandle,
                            fileHandle,
                            offset,
                            buf,
                            size
                        )
                    }
                    if (bytesRead < 0) {
                        throw ErrnoException("onRead", OsConstants.EIO)
                    }
                    buf.position(0)
                    buf.get(data, 0, bytesRead)
                    return bytesRead
                }

                override fun onWrite(offset: Long, size: Int, data: ByteArray): Int {
                    if (!isWritable) throw ErrnoException("onWrite", OsConstants.EROFS)
                    if (size <= 0) return 0
                    val buf = ensureBufferCapacity(size)
                    buf.put(data, 0, size)
                    buf.position(0)

                    val bytesWritten = vol.rwLock.write {
                        NtfsNativeBridge.nativeWriteFile(
                            vol.volumeHandle,
                            fileHandle,
                            offset,
                            buf,
                            size
                        )
                    }
                    if (bytesWritten < 0) {
                        throw ErrnoException("onWrite", OsConstants.EIO)
                    }
                    return bytesWritten
                }

                override fun onFsync() {
                    val rc = vol.rwLock.write {
                        NtfsNativeBridge.nativeFlushFile(vol.volumeHandle, fileHandle)
                    }
                    if (rc != 0) {
                        throw ErrnoException("onFsync", OsConstants.EIO)
                    }
                }

                override fun onRelease() {
                    vol.rwLock.write {
                        NtfsNativeBridge.nativeCloseFile(vol.volumeHandle, fileHandle)
                    }
                }
            },
            ioHandler
        )
    }

    private fun resolveDocumentId(documentId: String): Pair<NtfsVolumeRegistry.MountedVolume, String> {
        val idx = documentId.indexOf(':')
        if (idx <= 0) throw FileNotFoundException("Noto'g'ri documentId: $documentId")
        val rootId = documentId.substring(0, idx)
        val path = documentId.substring(idx + 1).ifEmpty { "/" }
        val vol = NtfsVolumeRegistry.getVolume(rootId)
            ?: throw FileNotFoundException("NTFS xotira uzilgan: $rootId")
        return vol to path
    }

    private fun includeEntry(cursor: MatrixCursor, rootId: String, entry: NtfsEntry) {
        var flags = Document.FLAG_SUPPORTS_DELETE or Document.FLAG_SUPPORTS_RENAME
        if (entry.isDirectory) {
            flags = flags or Document.FLAG_DIR_SUPPORTS_CREATE
        } else if (!entry.isReadOnly) {
            flags = flags or Document.FLAG_SUPPORTS_WRITE
        }

        val mimeType = if (entry.isDirectory) {
            Document.MIME_TYPE_DIR
        } else {
            val ext = entry.name.substringAfterLast('.', "").lowercase()
            MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
        }

        cursor.newRow().apply {
            add(Document.COLUMN_DOCUMENT_ID, "$rootId:${entry.fullPath}")
            add(Document.COLUMN_MIME_TYPE, mimeType)
            add(Document.COLUMN_DISPLAY_NAME, entry.name)
            add(Document.COLUMN_LAST_MODIFIED, entry.lastModified)
            add(Document.COLUMN_FLAGS, flags)
            add(Document.COLUMN_SIZE, entry.size)
        }
    }
}