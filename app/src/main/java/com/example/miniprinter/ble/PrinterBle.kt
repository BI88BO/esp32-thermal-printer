package com.example.miniprinter.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.util.UUID

/**
 * BLE 连接与数据收发。
 *
 * 设计要点：
 *  1. 用系统原生 BluetoothGatt，不引第三方蓝牙库——少一层依赖，出问题也知道去哪查
 *  2. 连接后先协商 MTU 247，一包能放 5 行，避免一帧一行带来的巨大开销
 *  3. 写操作用队列 + 6ms 节流。WRITE_NO_RESPONSE 没有回调，只能靠节流保证手机
 *     蓝牙栈不被塞满；打印机侧有 48 行缓冲，短暂突发不会丢
 */
@SuppressLint("MissingPermission")
class PrinterBle(
    private val context: Context,
    private val listener: Listener
) {

    interface Listener {
        fun onScanResult(device: BluetoothDevice, name: String, rssi: Int)
        fun onReady()
        fun onDisconnected()
        fun onEvent(event: PrinterProtocol.Event)
        fun onLog(msg: String)
        /** OTA 重启后自动重连的进度文字（显示在状态栏） */
        fun onReconnectProgress(msg: String)
        /** 自动重连结束：ok=true 已连上，false 超时需用户手动连接 */
        fun onReconnectFinished(ok: Boolean)
    }

    companion object {
        private const val TAG = "PrinterBle"
        val SERVICE_UUID: UUID = UUID.fromString("0f2df50d-25aa-40cb-93fe-b705d42a7736")
        val CHAR_RX_UUID: UUID = UUID.fromString("bab96b4d-f1e2-4c49-ad11-69151cb9af32")
        val CHAR_TX_UUID: UUID = UUID.fromString("082ed2c7-00c3-46ff-9f6a-d9694fa5d6f5")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val TARGET_MTU = 247
        private const val WRITE_INTERVAL_MS = 6L

        /* 与固件 config.h 的 DEVICE_NAME 一致。扫描列表只认这个名字，
         * OTA 自动重连也用它按广播名兜底（ESP32 重启后地址一般不变，
         * 万一变成随机地址还能靠名字找回来） */
        const val TARGET_DEVICE_NAME = "MiniPrinter"

        /* OTA 成功后固件 delay(350) 再 ESP.restart()，重启 + 重新广播约 1~2s */
        private const val RECONNECT_BOOT_DELAY_MS = 1200L
        private const val RECONNECT_TIMEOUT_MS = 25000L
        private const val RECONNECT_SCAN_MS = 3000L
        private const val RECONNECT_CONNECT_MS = 8000L
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val writeQueue = Channel<ByteArray>(Channel.UNLIMITED)
    private val rxBuffer = mutableListOf<Byte>()

    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var txChar: BluetoothGattCharacteristic? = null

    private val adapter: BluetoothAdapter? by lazy {
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    }

    @Volatile
    private var ready = false

    /* 实际协商到的 MTU。没协商成功前按默认 23 算，否则发出去的包会被栈悄悄丢掉 */
    @Volatile
    private var mtu = 23

    @Volatile
    private var mtuTries = 0

    /* ---------------- OTA 重启后的自动重连 ----------------
     * 记住上次连接过的设备；升级成功导致断连时，扫描到它重新广播就自动连回去。
     * 不直接盲连缓存的 BluetoothDevice：设备还在重启时 connectGatt 会挂 30 秒，
     * 期间无法发起第二次尝试。先看到广播再连，成功率最高。 */
    private var lastDevice: BluetoothDevice? = null
    private var lastDeviceName: String? = null
    @Volatile private var autoReconnecting = false
    @Volatile private var autoFound = false
    /* 扫描回调和重连协程不在同一线程，目标字段必须可见 */
    @Volatile private var autoTargetAddr: String? = null
    @Volatile private var autoTargetName: String? = null
    private var reconnectJob: Job? = null

    /** 自动重连是否进行中（MainActivity 用它避免重复启动 / 覆盖进度文字） */
    fun isAutoReconnecting(): Boolean = autoReconnecting

    /**
     * 设备名过滤。只在应用层筛，不用系统 ScanFilter：
     * 部分手机对广播里的 Local Name / 128 位 UUID 处理不一致，系统层过滤太严会
     * 出现「打印机明明在广播却扫不到」（之前 UUID 过滤就踩过这个坑）。
     * 允许 `MiniPrinter-86` 这类带后缀的固件名，方便以后改名带版本号。
     */
    private fun isNameMatch(name: String): Boolean {
        val n = name.trim()
        if (n.equals(TARGET_DEVICE_NAME, ignoreCase = true)) return true
        return n.startsWith(TARGET_DEVICE_NAME + "-", ignoreCase = true) ||
            n.startsWith(TARGET_DEVICE_NAME + "_", ignoreCase = true)
    }

    /* MTU 必须等服务发现 + CCCD 订阅完成后再请求。和 discoverServices 并发发
     * 会被部分手机的 GATT 操作队列丢掉，悄悄回落到 23：OTA 一帧要切 13 片，
     * 1.1MB 会从约 1 分钟变成 3 分钟以上，还伴随大量分片丢包重发。 */
    private fun requestMtuOnce(g: BluetoothGatt) {
        mtuTries++
        val ok = try {
            g.requestMtu(TARGET_MTU)
        } catch (e: Exception) {
            false
        }
        if (!ok) listener.onLog("requestMtu 调用失败（第 $mtuTries 次）")
    }

    /** 实际协商到的 MTU。OTA 开始前必须检查，低于 100 就不要硬传 */
    fun currentMtu(): Int = mtu

    /** OTA 前主动补一次 MTU 请求（连接早期协商失败时用） */
    fun retryMtu() {
        val g = gatt ?: return
        if (mtu >= 100) return
        mtuTries = 0
        requestMtuOnce(g)
    }

    /* OTA 期间把写节流从 6/4ms 收到 3/3ms：1.1MB 镜像约 4600 帧，
     * 按原节流光发送就要 60 多秒。打印时保持原速率不变。
     * 即使偶尔丢包，OTA 协议靠 seq 缺口补发自愈。 */
    @Volatile
    private var fastWrites = false

    init {
        /* 单一消费者。打印时保持 6ms 固定节流保护蓝牙栈；
         * OTA 期间不再按帧空等 3+3ms —— 4600 多帧累计约 40 秒纯浪费。
         * 仍保留 1ms 片间隔：同一个 characteristic 连续 setValue 不能零间隔。 */
        scope.launch {
            for (packet in writeQueue) {
                val g = gatt
                val c = rxChar
                if (g != null && c != null && ready) {
                    /* 一个协议帧可能比 MTU 长，必须切片。
                     * writeCharacteristic 超长是静默失败，不看 MTU 会以为发出去了 */
                    val maxPayload = (mtu - 3).coerceAtLeast(20)
                    var off = 0
                    while (off < packet.size) {
                        val len = minOf(maxPayload, packet.size - off)
                        val slice = packet.copyOfRange(off, off + len)
                        c.value = slice
                        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE

                        /* 栈缓冲满时 writeCharacteristic 返回 false：等一下重试，
                         * 不能直接 break —— 丢一片会让整帧校验失败、白白重发一整块 */
                        var ok = g.writeCharacteristic(c)
                        var tries = 0
                        var wait = 5L
                        /* 递增等待：刚建立连接时 GATT 还在处理服务发现 / MTU /
                         * CCCD 的收尾，writeCharacteristic 会短暂返回 false。
                         * 原来固定 2ms×3 次总停顿才 6ms，几乎必然放弃并打出
                         * 「写入失败」；改成 5/15/45ms，总共约 65ms，够 GATT 缓过来 */
                        while (!ok && tries < 3) {
                            delay(wait)
                            wait *= 3
                            tries++
                            ok = g.writeCharacteristic(c)
                        }
                        if (!ok) {
                            listener.onLog("写入失败（MTU=$mtu, len=${slice.size}）")
                            break
                        }
                        off += len
                        /* 只有「还有下一片」才等；单帧单片的 OTA 数据帧不等片间 */
                        if (off < packet.size) delay(if (fastWrites) 1L else 4L)
                    }
                }
                /* 同一个 characteristic 连续 setValue 不能零间隔：各厂商 BLE 栈
                 * 实现不一，完全不等可能把上一片的数据覆盖掉。OTA 保留 1ms 即可，
                 * 相比原来的 3+3ms 仍省掉约 5/6 的固定等待。 */
                delay(if (fastWrites) 1L else WRITE_INTERVAL_MS)
            }
        }
    }

    /* ---------------- 扫描 ---------------- */

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val dev = result.device ?: return
            val name = result.scanRecord?.deviceName ?: dev.name ?: "未知设备"

            /* 自动重连期间扫描结果不进选择列表，命中目标就立刻连接 */
            val target = autoTargetAddr
            if (target != null) {
                val hit = dev.address == target ||
                    (autoTargetName != null && isNameMatch(name))
                if (hit && !autoFound) {
                    autoFound = true
                    autoTargetAddr = null       // 只连一次，避免后续广播重复触发
                    stopScan()
                    listener.onReconnectProgress("发现打印机，正在重新连接…")
                    connect(dev)
                }
                return
            }
            /* 选择列表只显示目标打印机。某些手机读不到广播里的 Local Name，
             * 这时放上次成功连过的那台（按 MAC）进来，避免彻底扫不到 */
            if (!isNameMatch(name) && dev.address != lastDevice?.address) return
            listener.onScanResult(dev, name, result.rssi)
        }

        override fun onScanFailed(errorCode: Int) {
            /* 自动重连时这条日志会盖掉进度文字，交给重试循环自己汇报 */
            if (!autoReconnecting) listener.onLog("扫描失败: $errorCode")
        }
    }

    /** 用户主动发起扫描：此时取消 OTA 自动重连，避免两路连接打架 */
    fun startScan() {
        cancelAutoReconnect()
        startScanInternal()
    }

    private fun startScanInternal() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            listener.onLog("蓝牙不可用")
            return
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        /* 不加 UUID 过滤：不同手机对"广播里带 128 位 UUID"的处理不一致，
         * 过滤太严会出现设备明明在广播却扫不到。扫全部，列表里靠名字认 */
        runCatching { scanner.startScan(null, settings, scanCallback) }
            .onFailure { listener.onLog("扫描启动失败：${it.message}") }
    }

    fun stopScan() {
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
    }

    /* ---------------- 连接 ---------------- */

    fun connect(device: BluetoothDevice) {
        lastDevice = device
        lastDeviceName = device.name ?: TARGET_DEVICE_NAME
        stopScan()
        close()
        if (autoReconnecting) listener.onReconnectProgress("连接 ${device.address} …")
        else listener.onLog("连接 ${device.address} …")

        scope.launch {
            delay(6000)
            /* 自动重连有自己的重试与超时汇报，别在中间插一条「搜不到服务」 */
            if (!ready && !autoReconnecting)
                listener.onLog("连接超时：搜不到服务，检查固件是否在广播")
        }

        gatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(context, false, gattCallback)
        }
    }

    fun disconnect() {
        cancelAutoReconnect()
        close()
        listener.onDisconnected()
    }

    /**
     * OTA 成功后固件 ESP.restart()，BLE 必然断开。这里等它重新广播再自动连回去。
     *
     * 不直接对缓存的 BluetoothDevice 盲连：设备还在重启时 connectGatt 会挂约 30 秒，
     * 期间无法发起第二次尝试，用户只能干等。先扫描到广播再连，成功率最高。
     * 返回 false = 没有记住任何设备，调用方应提示手动连接。
     */
    fun autoReconnect(): Boolean {
        val dev = lastDevice
        if (dev == null) {
            listener.onReconnectFinished(false)
            return false
        }
        if (autoReconnecting) return true

        autoReconnecting = true
        autoFound = false
        autoTargetAddr = dev.address
        autoTargetName = lastDeviceName ?: TARGET_DEVICE_NAME
        stopScan()
        close()

        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            delay(RECONNECT_BOOT_DELAY_MS)
            val deadline = System.currentTimeMillis() + RECONNECT_TIMEOUT_MS
            var attempt = 0

            while (autoReconnecting && System.currentTimeMillis() < deadline) {
                attempt++
                listener.onReconnectProgress(
                    "固件升级完成 · 正在自动重连（第 $attempt 次）…"
                )
                autoFound = false
                autoTargetAddr = dev.address
                startScanInternal()

                var waited = 0L
                while (autoReconnecting && !autoFound && waited < RECONNECT_SCAN_MS &&
                    System.currentTimeMillis() < deadline
                ) {
                    delay(100)
                    waited += 100
                }
                stopScan()
                if (!autoReconnecting) return@launch

                if (autoFound) {
                    /* 等 connect → discoverServices → ready，最多 8 秒 */
                    var w = 0L
                    while (autoReconnecting && !ready && w < RECONNECT_CONNECT_MS) {
                        delay(100)
                        w += 100
                    }
                    if (ready) {
                        finishAutoReconnect(true)
                        return@launch
                    }
                    /* 连上了却没发现服务：释放 GATT，下一轮重新扫描 */
                    runCatching { gatt?.disconnect() }
                    runCatching { gatt?.close() }
                    gatt = null
                }
                delay(400)
            }
            if (autoReconnecting) finishAutoReconnect(false)
        }
        return true
    }

    private fun finishAutoReconnect(ok: Boolean) {
        if (!autoReconnecting) return          // 幂等：用户可能已经手动连上了
        stopScan()
        autoTargetAddr = null
        autoTargetName = null
        autoReconnecting = false
        if (ok) {
            listener.onReconnectFinished(true)
        } else {
            listener.onReconnectProgress("自动重连超时：请手动点「连接打印机」")
            listener.onReconnectFinished(false)
        }
    }

    /** 用户主动扫描/断开时取消自动重连，不弹「重连失败」 */
    fun cancelAutoReconnect() {
        if (!autoReconnecting) return
        reconnectJob?.cancel()
        reconnectJob = null
        autoTargetAddr = null
        autoTargetName = null
        autoReconnecting = false
        stopScan()
    }

    fun isReady(): Boolean = ready

    /** 入队一帧，立即返回，不阻塞 UI */
    fun send(packet: ByteArray) {
        writeQueue.trySend(packet)
    }

    /** 只在 OTA 传输期间开启，结束后必须关掉 */
    fun setFastWrites(on: Boolean) {
        fastWrites = on
    }

    /** 清空待发队列（作业中止时用） */
    fun clearQueue() {
        while (writeQueue.tryReceive().isSuccess) {
            /* drain */
        }
    }

    private fun close() {
        ready = false
        runCatching { gatt?.disconnect() }
        runCatching { gatt?.close() }
        gatt = null
        rxChar = null
        txChar = null
        rxBuffer.clear()
        clearQueue()
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            /* 失败也要说一声，不然界面上什么都没有，像点了没反应 */
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onLog("连接失败 status=$status")
                ready = false
                listener.onDisconnected()
                return
            }
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    listener.onLog("已连接，发现服务")
                    mtu = 23
                    mtuTries = 0
                    g.discoverServices()              // MTU 等发现完成后再请求，见 requestMtuOnce
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    ready = false
                    listener.onLog("连接断开")
                    listener.onDisconnected()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu > 23) {
                this@PrinterBle.mtu = mtu
                listener.onLog("蓝牙 MTU = $mtu")
            } else {
                listener.onLog("MTU 协商失败（status=$status），重试中")
            }
            /* 协商失败或只给了个小 MTU：隔一会儿再要一次，最多 3 次。
             * 不重试的后果是 OTA 静默走 23 字节分片，用户只看到「几分钟没反应」 */
            if (this@PrinterBle.mtu < 100 && mtuTries < 3) {
                scope.launch { delay(400); requestMtuOnce(g) }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE_UUID)
            if (service == null) {
                listener.onLog("找不到打印服务")
                return
            }

            /* 用局部变量接一下，Kotlin 对 var 属性做不了智能转换 */
            val rx = service.getCharacteristic(CHAR_RX_UUID)
            val tx = service.getCharacteristic(CHAR_TX_UUID)

            if (rx == null || tx == null) {
                listener.onLog("特征值缺失")
                return
            }

            rxChar = rx
            txChar = tx

            /* 打开通知，才能收到 STATUS / ACK / ERROR */
            g.setCharacteristicNotification(tx, true)
            tx.getDescriptor(CCCD_UUID)?.let { d ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(d)
                }
            }

            ready = true
            listener.onReady()
            /* 服务发现和 CCCD 都办完了再要 MTU，避开 GATT 操作队列竞争 */
            scope.launch { delay(200); requestMtuOnce(g) }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            @Suppress("DEPRECATION")
            val v = characteristic.value ?: return
            onNotify(v)
        }

        /* Android 13 起走这个重载，两个都实现以兼容新旧系统 */
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            onNotify(value)
        }
    }

    private fun onNotify(data: ByteArray) {
        synchronized(rxBuffer) {
            for (b in data) rxBuffer.add(b)
            while (true) {
                val ev = PrinterProtocol.parse(rxBuffer) ?: break
                listener.onEvent(ev)
            }
        }
    }
}
