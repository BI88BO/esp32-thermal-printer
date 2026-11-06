package com.example.miniprinter

import android.Manifest
import android.bluetooth.BluetoothDevice
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.example.miniprinter.ble.PrinterBle
import com.example.miniprinter.ble.PrinterProtocol
import com.example.miniprinter.databinding.ActivityMainBinding
import com.example.miniprinter.image.BitmapConverter
import com.example.miniprinter.image.TextRenderer
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import kotlin.math.min
import java.util.zip.CRC32

/**
 * 一个界面三件事：连打印机、生成内容（文字/图片）、打印。
 * 没有编辑功能——需要修图在系统相册里做好再选进来。
 */
class MainActivity : AppCompatActivity(), PrinterBle.Listener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var ble: PrinterBle
    private val ui = Handler(Looper.getMainLooper())

    private var rows: ByteArray? = null
    private var totalLines = 0
    private var lastImageUri: Uri? = null      // 记住当前内容来源，滑块改动时重算

    /* 发送协程在 Dispatchers.Default，状态回调在主线程：跨线程读的标志必须 volatile */
    @Volatile private var sentLines = 0
    @Volatile private var doneLines = 0
    @Volatile private var printing = false
    @Volatile private var paused = false       // 固件过热暂停中，冷却后自动续打
    @Volatile private var paperOk = true       // 最近一次状态上报的缺纸标志

    /* ---------------- BLE OTA ---------------- */
    private val otaAcks = Channel<PrinterProtocol.OtaAck>(Channel.UNLIMITED)
    private val otaInfos = Channel<PrinterProtocol.OtaInfo>(Channel.UNLIMITED)
    @Volatile private var otaRunning = false
    @Volatile private var otaSucceeded = false  // 重启导致的断连不是「升级失败」

    /* 第一次选过 .bin 后把 URI 记住，以后点一下就升级；换固件用「重新选 .bin」。
     * SAF 的持久读权限让 App 重启后仍能打开这个文件，不需要存储权限。 */
    private val pickFirmware =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            if (uri == null) return@registerForActivityResult
            runCatching {
                contentResolver.takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            }
            prefs.edit().putString("ota_uri", uri.toString()).apply()
            startOtaFromUri(uri)
        }

    /* ---------------- 打印参数（持久化） ---------------- */
    private var heatUs = 3600          // 单组加热时间，决定浓度
    private var preheatUs = 0          // 预加热脉冲
    private var repeatN = 1            // 同一行加热遍数
    private var dotComp = 0            // 点密度补偿百分比
    private var drive2 = 0             // 0 = 1-2 相（4 拍/行）1 = 双相整步（2 拍/行）

    companion object {
        /* 浓度滑杆：1200~4000µs。
         * 规格书 3.3 表 4 P0=0.24W/dot（VH=7.2V）、3.2 表 3 E0=0.20mJ/dot
         *   => 标称上限 833µs，代入 3.4 公式验算完全自洽。
         * 本机静态 VH 实测 7.2V，所以这个折算适用：4000µs ≈ 4.95×E0，
         * 确实是超额定运行，不能免责。这里收掉原来 6000µs（7.2×E0）
         * 那段纯危险区，并把折算值直接显示出来由使用者自己判断。
         * 至于"为什么 7.2V 下仍要几千 µs 才出色"，尚未定量诊断，
         * 见 config.h 的加热能量一节（带载压降等候选原因）。 */
        private const val HEAT_MIN_US = 1200
        private const val HEAT_MAX_US = 4000
        private const val P0_MJ_PER_US = 0.00024   // 0.24 W/dot = 0.00024 mJ/µs
        private const val E0_MJ = 0.20
    }

    private val prefs by lazy { getSharedPreferences("quality", MODE_PRIVATE) }
    private val mainHandler = Handler(Looper.getMainLooper())
    private var scanHandler: ((BluetoothDevice, String) -> Unit)? = null

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { r ->
            if (r.values.all { it }) startScan() else toast("缺少蓝牙权限")
        }

    private val pickImage =
        registerForActivityResult(ActivityResultContracts.GetContent()) { uri: Uri? ->
            if (uri != null) loadImage(uri)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        ble = PrinterBle(this, this)

        binding.btnConnect.setOnClickListener {
            if (ble.isReady()) ble.disconnect() else ensurePermissions()
        }
        binding.btnText.setOnClickListener { renderText() }
        binding.btnPick.setOnClickListener { pickImage.launch("image/*") }
        binding.btnPrint.setOnClickListener { startPrint() }
        binding.btnOta.setOnClickListener { startOta(forcePick = false) }
        binding.btnOtaPick.setOnClickListener { startOta(forcePick = true) }

        binding.seekSize.setOnSeekBarChangeListener(SimpleSeek(onChange = { regenerate() }))
        binding.seekThreshold.setOnSeekBarChangeListener(SimpleSeek(onChange = { regenerate() }))

        setupQuality()

        status("未连接")
        renderText()
    }

    /* ---------------- 打印质量 ---------------- */

    private fun setupQuality() {
        /* 存的就是上次用的参数。打印质量是"调一次用很久"的东西，
         * 每次重开 App 都要重调会很烦。
         * 键名带 v2：早期版本存过 1800，而热敏纸是阈值型的，1800 可能完全不出色，
         * 换个键名让新默认值生效 */
        heatUs = prefs.getInt("v2_heat", 3600).coerceIn(HEAT_MIN_US, HEAT_MAX_US)
        preheatUs = prefs.getInt("v2_preheat", 0)
        repeatN = prefs.getInt("v2_repeat", 1)
        dotComp = prefs.getInt("v2_dotcomp", 0)
        drive2 = prefs.getInt("v2_drive", 0)

        binding.cbDrive.isChecked = drive2 == 1
        binding.cbDrive.setOnCheckedChangeListener { _, checked ->
            drive2 = if (checked) 1 else 0
            saveQuality()
            showQuality()
            pushParams()
        }

        binding.seekHeat.max = HEAT_MAX_US - HEAT_MIN_US       // 1200 ~ 4000µs
        /* 写入和读回必须用同一个零点：以前写 (heatUs-300)、读 (1200+progress)，
         * 一拖动就凭空多出 900µs，显示的浓度和下发给固件的值对不上 */
        binding.seekHeat.progress = (heatUs - HEAT_MIN_US).coerceIn(0, HEAT_MAX_US - HEAT_MIN_US)

        binding.seekHeat.setOnSeekBarChangeListener(SimpleSeek(
            onChange = {
                heatUs = HEAT_MIN_US + binding.seekHeat.progress
                saveQuality()
                showQuality()
            },
            /* 松手立刻下发。以前滑杆只改 App 本地变量和 SharedPreferences，
             * 只有点「打印」时才 setHeat —— 于是点检按钮永远用固件的旧值，
             * 把滑杆拉到 4000 也看不到任何变化。 */
            onStop = { pushParams() }
        ))

        /* 三档预设：都是常见场景，避免用户面对四个参数不知道从哪下手 */
        binding.rgQuality.setOnCheckedChangeListener { _, id ->
            when (id) {
                R.id.rbStd -> { preheatUs = 0;   repeatN = 1; dotComp = 0 }
                R.id.rbDeep -> { preheatUs = 260; repeatN = 1; dotComp = 60 }
                R.id.rbStrong -> { preheatUs = 260; repeatN = 2; dotComp = 80 }
            }
            saveQuality()
            showQuality()
            pushParams()
        }

        val preset = when {
            repeatN >= 2 -> R.id.rbStrong
            preheatUs > 0 -> R.id.rbDeep
            else -> R.id.rbStd
        }
        binding.rgQuality.check(preset)

        binding.btnDotCheck.setOnClickListener {
            if (!ble.isReady()) return@setOnClickListener toast("还没连打印机")
            /* 缺纸时诊断页既加热又走纸，没有意义还磨发热体。
             * 固件侧 build 91 起也会拒绝，这里先拦住给个明确提示 */
            if (!paperOk) return@setOnClickListener toast("缺纸：装好纸卷后再点检")
            /* 点检页四档的顶档取自固件 user_heat_us，不是 App 显示的文字。
             * 必须先同步参数，否则打的是「上次打印下发的旧浓度」，
             * 滑杆拉满也不会有任何变化。 */
            pushParams()
            ble.send(PrinterProtocol.selftest(1))
            status("已发送点检（基准 %dµs，四档按当前头温补偿后缩放）".format(heatUs))
        }

        showQuality()
    }

    /** 把当前参数同步给固件。连接、参数区改动、点检、正式打印都走这里。
     *  PrinterBle.send 是队列 + 6ms 节流，连发三帧不会堵蓝牙栈。 */
    private fun pushParams() {
        if (!ble.isReady()) return
        ble.send(PrinterProtocol.setHeat(heatUs))
        ble.send(PrinterProtocol.setQuality(preheatUs, repeatN, dotComp))
        ble.send(PrinterProtocol.setDrive(drive2))
    }

    private fun saveQuality() {
        prefs.edit()
            .putInt("v2_heat", heatUs)
            .putInt("v2_preheat", preheatUs)
            .putInt("v2_repeat", repeatN)
            .putInt("v2_dotcomp", dotComp)
            .putInt("v2_drive", drive2)
            .apply()
    }

    /** 与固件 printer_line_us() 同一套算法，返回「满行」保守估算，只用于界面显示。
     *  发送节奏不再依赖它，改由固件的 doneLines 流控决定（见 startPrint）。 */
    private fun lineUs(): Double {
        val per = repeatN * (heatUs + (if (preheatUs > 0) preheatUs + 180 else 0)) +
                  (repeatN - 1) * 150 + 200
        val minSlot = if (drive2 == 1) 1800 else 1200
        val slot = maxOf((if (drive2 == 1) 3 else 2) * per, minSlot)
        /* 1-2 相 = 3 个加热拍 + 1 个空拍；双相 = 2 个加热拍。
         * 旧公式按 4 个满拍算，白等约 26%；还漏了遍间 150µs，
         * 导致「最强 + 双相」显示 2.58mm/s、实际只有 2.46mm/s。 */
        return if (drive2 == 1) 2.0 * slot + 250 else 3.0 * slot + minSlot + 250
    }

    private fun showQuality() {
        val mmps = 0.125 * 1_000_000.0 / lineUs()
        val mode = if (drive2 == 1) "双相 2 拍" else "1-2 相 4 拍"
        binding.tvQuality.text =
            "%s · 加热 %dµs · 预加热 %dµs · 重复 %d 遍 · 补偿 %d%%   满行约 %.1fmm/s（空白行更快）"
                .format(mode, heatUs, preheatUs, repeatN, dotComp, mmps)

        /* 按规格书 3.3 表 4 的 P0（VH=7.2V 标称）折算，并直接给出相对
         * 表 3 E0 的倍数。本机静态 VH 就是 7.2V，折算前提成立；
         * 显示"超 E0"是事实，不是提示错误。实际是否真的超额定，取决于
         * 带载时头端电压与回路阻抗——那部分尚未实测，所以这里只标
         * "标称折算"，安全由固件 50℃ 熔断兜底。 */
        val mj = heatUs * P0_MJ_PER_US
        binding.tvHeatHint.text =
            "%dµs ≈ %.2f mJ/dot = E0 的 %.1f 倍（7.2V 标称折算）· 上限 %dµs · 调节即下发"
                .format(heatUs, mj, mj / E0_MJ, HEAT_MAX_US)
    }

    /* ---------------- 权限 ---------------- */

    private fun ensurePermissions() {
        val needed = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startScan() else permissionLauncher.launch(missing.toTypedArray())
    }

    /* ---------------- 扫描 ---------------- */

    private fun startScan() {
        /* 列表项和蓝牙设备按下标一一对应。
         * 之前用 Map 以 MAC 为键、却拿显示文字去查，永远查不到，点了没反应 */
        val devices = mutableListOf<BluetoothDevice>()
        val items = mutableListOf<String>()
        val adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, items)

        val builder = AlertDialog.Builder(this)
            /* 列表只会出现 PrinterBle 按设备名过滤后的打印机，
             * 不再把附近所有 BLE 设备都列进来 */
            .setTitle("选择 ${PrinterBle.TARGET_DEVICE_NAME}")
            .setNegativeButton("取消") { _, _ -> ble.stopScan() }
            .setAdapter(adapter) { _, which ->
                ble.stopScan()
                devices.getOrNull(which)?.let {
                    status("连接中…")
                    ble.connect(it)
                }
            }

        val dialog = builder.create()
        dialog.setOnDismissListener { ble.stopScan() }
        dialog.show()

        scanHandler = { dev, name ->
            if (devices.none { it.address == dev.address }) {
                devices.add(dev)
                items.add("$name  (${dev.address})")
                adapter.notifyDataSetChanged()
            }
        }

        ble.startScan()
        status("正在搜索 ${PrinterBle.TARGET_DEVICE_NAME}…")

        mainHandler.postDelayed({
            ble.stopScan()
            if (items.isEmpty()) {
                status("没找到 ${PrinterBle.TARGET_DEVICE_NAME}：确认打印机已上电且在广播")
                dialog.dismiss()
            }
        }, 6000)
    }

    /* ---------------- 内容 ---------------- */

    private fun renderText() {
        lastImageUri = null
        val text = binding.etText.text?.toString().orEmpty().ifBlank { "MiniPrinter" }
        val size = binding.seekSize.progress.coerceIn(12, 48).toFloat()
        val r = TextRenderer.render(text, size, false)

        /* 文字用纯阈值，线条比抖动更锐 */
        val conv = BitmapConverter.convert(
            r.bitmap, binding.seekThreshold.progress, dither = false
        )
        apply(conv)
    }

    private fun loadImage(uri: Uri) {
        lastImageUri = uri
        val bmp = MediaStore.Images.Media.getBitmap(contentResolver, uri)
        val conv = BitmapConverter.convert(
            bmp, binding.seekThreshold.progress, dither = true
        )
        apply(conv)
    }

    /** 滑块动了，按当前内容来源重新生成 */
    private fun regenerate() {
        val uri = lastImageUri
        if (uri != null) loadImage(uri) else renderText()
    }

    private fun apply(c: BitmapConverter.Result) {
        binding.ivPreview.setImageBitmap(c.preview)
        rows = c.rows
        totalLines = c.lines
        binding.tvInfo.text = "${c.lines} 行 · ${"%.1f".format(c.lines * 0.125f)} mm"
    }

    /* ---------------- 打印 ---------------- */

    private fun startPrint() {
        val data = rows
        if (data == null || totalLines == 0) return toast("先输入文字或选一张图")
        if (!ble.isReady()) return toast("还没连打印机")
        if (printing) return toast("正在打印")
        if (!paperOk) return toast("缺纸：请装好纸卷再打印")

        printing = true
        paused = false
        sentLines = 0
        doneLines = 0
        binding.btnPrint.isEnabled = false
        status("打印中…")

        lifecycleScope.launch(Dispatchers.Default) {
            pushParams()
            ble.send(PrinterProtocol.begin(totalLines))

            var seq = 0
            var offset = 0

            while (offset < data.size) {
                /* 打印机报错或断连就停手，别继续灌数据 */
                if (!printing) return@launch

                /* 唯一的节流：固件队列 48 行，保持未打印行数不超过 32。
                 * 不再按估算行时间空等，速度由打印机实际消费决定。
                 * guard 每次等待重新计数（累计计数会在长图中途耗尽）。
                 * 过热暂停时 doneLines 会停住：这时 guard 归零并持续等待，
                 * 固件保留队列和断点，降到 45℃ 会自己续打；绝不能判超时、
                 * 更不能重新发 BEGIN，否则长图每次过热都从头开始。 */
                var guard = 0
                while (sentLines - doneLines > 32) {
                    if (!printing) return@launch
                    if (paused) { guard = 0; delay(100); continue }
                    if (guard++ >= 1500) {
                        ui.post {
                            printing = false
                            binding.btnPrint.isEnabled = true
                            status("打印机 30 秒无进展，已停止发送")
                        }
                        return@launch
                    }
                    delay(20)
                }

                val remain = (data.size - offset) / BitmapConverter.LINE_BYTES
                val n = min(PrinterProtocol.LINES_PER_PACKET, remain)
                val chunk = data.copyOfRange(offset, offset + n * BitmapConverter.LINE_BYTES)

                ble.send(PrinterProtocol.data(seq++, chunk))
                sentLines += n
                offset += n * BitmapConverter.LINE_BYTES

                /* 旧版这里 delay(lineMs * n)：lineMs 是「估算行时间」，
                 * 1-2 相下高估约 26%（App 发得比打印机慢，打印机干等），
                 * 双相 + 最强档又低估约 5%（反而灌队列、周期性卡顿）。
                 * 去掉它，改由上面的 doneLines 流控决定节奏：固件 48 行队列
                 * 有足够缓冲，空白组被跳过的稀疏行也能立刻跑满。 */
                delay(2)
            }
            if (!printing) return@launch
            ble.send(PrinterProtocol.end())

            ui.post { status("已发送 $totalLines 行，等待打印机…") }

            /* 看门狗：3 秒内打印机一行都没动，说明数据没到或者被拒了。
             * 过热暂停不算「无响应」，固件正在冷却 */
            delay(3000)
            if (printing && !paused && doneLines == 0) {
                ui.post { status("打印机无响应：检查是否缺纸、电量低，或打开固件串口日志") }
            }
        }
    }

    /* ---------------- 固件升级（BLE OTA） ---------------- */

    private fun startOta(forcePick: Boolean) {
        if (!ble.isReady()) return toast("先连接打印机")
        if (printing) return toast("等当前打印结束再升级")
        if (otaRunning) return toast("升级进行中")

        val saved = if (forcePick) null else prefs.getString("ota_uri", null)
        if (saved == null) {
            runCatching { pickFirmware.launch(arrayOf("*/*")) }
                .onFailure { toast("打不开文件选择器：${it.message}") }
        } else {
            startOtaFromUri(Uri.parse(saved))
        }
    }

    private fun startOtaFromUri(uri: Uri) {
        val image = try {
            contentResolver.openInputStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            null
        }
        if (image == null || image.isEmpty()) {
            prefs.edit().remove("ota_uri").apply()
            return toast("固件文件打不开，点「重新选 .bin」再选一次")
        }
        /* ESP32 app 镜像第一字节是 0xE9；merged_firmware.bin 头部是 0xFF
         * （bootloader + 分区表），只能烧 0x0，不能拿来 OTA */
        if (image[0] != 0xE9.toByte()) {
            prefs.edit().remove("ota_uri").apply()
            status("选错了：要选 firmware.bin（纯 app 镜像），不是 merged_firmware.bin")
            return toast("不是 app 镜像，请选择 firmware.bin")
        }
        if (!ble.isReady()) return toast("先连接打印机")

        lifecycleScope.launch(Dispatchers.Default) {
            val info = queryOtaInfo()
            when {
                info == null -> ui.post {
                    status("板子里是旧固件（不响应 OTA 查询）：请先用串口烧一次 v86，之后就能一直 OTA")
                }
                info.build < 86 -> ui.post {
                    status("当前 v${info.build} 不支持 OTA，请先串口烧一次 v86")
                }
                image.size.toLong() > info.maxSize -> ui.post {
                    status("镜像 ${image.size / 1024}KB 超过备用分区 ${info.maxSize / 1024}KB")
                }
                else -> runOta(image)
            }
        }
    }

    private suspend fun queryOtaInfo(): PrinterProtocol.OtaInfo? {
        while (otaInfos.tryReceive().isSuccess) { /* drain */ }
        ble.send(PrinterProtocol.otaInfo())
        return try {
            withTimeout(4000) { otaInfos.receive() }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }

    private fun clearOtaAcks() {
        while (otaAcks.tryReceive().isSuccess) { /* drain */ }
    }

    private suspend fun awaitOtaAck(cmd: Int, timeoutMs: Long): PrinterProtocol.OtaAck {
        var found: PrinterProtocol.OtaAck? = null
        withTimeout(timeoutMs) {
            while (found == null) {
                val a = otaAcks.receive()
                if (a.cmd == cmd) found = a
            }
        }
        return found ?: throw Exception("没有收到打印机应答")
    }

    private suspend fun runOta(image: ByteArray) {
        otaRunning = true
        otaSucceeded = false
        ui.post { setOtaUi(true) }

        val crc = CRC32().apply { update(image) }.value
        val startedAt = System.currentTimeMillis()

        try {
            /* MTU 掉到 23 会把一帧切成 13 片，1.1MB 要 3 分钟以上还会反复重发，
             * 表现就是「好几分钟没成功」。先补一次请求并短暂等协商，
             * 仍失败就直接说明原因，不再让用户对着不动的进度条干等。 */
            var mtu = ble.currentMtu()
            if (mtu < 100) {
                ble.retryMtu()
                var wait = 0
                while (mtu < 100 && wait++ < 10) {
                    delay(200)
                    mtu = ble.currentMtu()
                }
            }
            if (mtu < 100) throw Exception(
                "蓝牙 MTU 只有 $mtu（需要 247）：请断开后重新连接打印机再升级"
            )

            /* 整帧 = block + 6 必须 <= mtu - 3，保证一次 BLE 写发完，不切片 */
            val block = PrinterProtocol.otaBlockForMtu(mtu)
            val totalBlocks = (image.size + block - 1) / block
            ui.post { status("开始升级 · MTU $mtu · 每块 ${block}B（请勿断电）") }

            ble.setFastWrites(true)

            clearOtaAcks()
            ble.send(PrinterProtocol.otaBegin(image.size, crc))
            val begin = awaitOtaAck(PrinterProtocol.CMD_OTA_BEGIN, 10000)
            if (begin.code != 0) throw Exception(PrinterProtocol.otaErrorText(begin.code))

            var seq = 0
            var stalled = 0
            while (seq < totalBlocks) {
                val batchEnd = minOf(seq + PrinterProtocol.OTA_WINDOW, totalBlocks)

                /* 发一批再停等 ACK。固件在队列排空 + 静默 40ms 后回 nextSeq，
                 * 它停在哪块就说明缺口在哪，从这里补发即可，不需要单独的 NACK。 */
                clearOtaAcks()
                for (s in seq until batchEnd) {
                    val off = s * block
                    val len = minOf(block, image.size - off)
                    ble.send(PrinterProtocol.otaData(s, image, off, len))
                }
                val ack = awaitOtaAck(PrinterProtocol.CMD_OTA_DATA, 20000)
                if (ack.code != 0) throw Exception(PrinterProtocol.otaErrorText(ack.code))

                val next = ack.nextSeq.coerceIn(seq, batchEnd)
                if (next <= seq) {
                    if (++stalled >= 4) throw Exception("打印机没有接收数据，检查蓝牙连接")
                } else {
                    stalled = 0
                    seq = next
                }

                val done = minOf(seq * block, image.size)
                val secs = (System.currentTimeMillis() - startedAt) / 1000.0
                val kbps = if (secs > 0.5) (done / 1024.0 / secs).toInt() else 0
                ui.post {
                    binding.pbOta.progress = done * 100 / image.size
                    status(
                        "升级中… ${done / 1024}/${image.size / 1024} KB · $kbps KB/s · " +
                            "%.0fs · 第 $seq/$totalBlocks 块".format(secs)
                    )
                }
            }

            clearOtaAcks()
            ble.send(PrinterProtocol.otaEnd())
            val end = awaitOtaAck(PrinterProtocol.CMD_OTA_END, 20000)
            if (end.code != 0) throw Exception("校验失败：" + PrinterProtocol.otaErrorText(end.code))

            otaSucceeded = true
            ui.post {
                binding.pbOta.progress = 100
                status(
                    "升级成功 · 用时 %.1fs，打印机正在重启，App 将自动重新连接"
                        .format((System.currentTimeMillis() - startedAt) / 1000.0)
                )
            }
        } catch (e: TimeoutCancellationException) {
            ble.send(PrinterProtocol.otaAbort())
            ui.post {
                status(
                    "升级超时（MTU ${ble.currentMtu()} · 已等 %.0fs），已取消；当前固件未改动"
                        .format((System.currentTimeMillis() - startedAt) / 1000.0)
                )
            }
        } catch (e: Exception) {
            ble.send(PrinterProtocol.otaAbort())
            ui.post { status("升级失败：${e.message}（当前固件未改动）") }
        } finally {
            ble.setFastWrites(false)
            otaRunning = false
            ui.post { setOtaUi(false) }
        }
    }

    private fun setOtaUi(busy: Boolean) {
        binding.btnOta.isEnabled = !busy
        binding.btnOtaPick.isEnabled = !busy
        binding.btnPrint.isEnabled = !busy && !printing
        binding.btnDotCheck.isEnabled = !busy
        binding.btnConnect.isEnabled = !busy
    }

    /* ---------------- 回调 ---------------- */

    override fun onScanResult(device: BluetoothDevice, name: String, rssi: Int) {
        ui.post { scanHandler?.invoke(device, name) }
    }

    /** 电量文字：有百分比就显示「85% · 3.85V」，旧固件（build<94）只有电压 */
    private fun batteryText(s: PrinterProtocol.Status): String =
        if (s.batteryPct in 0..100) "%d%% · %.2fV".format(s.batteryPct, s.batteryMv / 1000f)
        else "%.2fV".format(s.batteryMv / 1000f)

    override fun onReady() {
        ui.post {
            /* OTA 重启后的自动重连已经成功，别让之后的断连还说「正在重启」 */
            otaSucceeded = false
            binding.btnConnect.text = "断开"
            status("已连接")
            /* 刚完成 MTU 协商，GATT 还在收尾，这时候立刻 writeCharacteristic
             * 会返回 false（用户看到的「写入失败」就是这个，重试 3 次各 2ms 根本
             * 不够）。等 250ms 让链路彻底空闲，再同步参数和查版本。 */
            ui.postDelayed({
                if (ble.isReady()) {
                    /* 连接即同步：固件默认 3600 或上次作业残留的浓度，
                     * 和 App 里存的值可能不一样，不同步就会出现「一连上就变淡」 */
                    pushParams()
                    ble.send(PrinterProtocol.otaInfo())
                }
            }, 250)
        }
    }

    override fun onDisconnected() {
        ui.post {
            binding.btnConnect.text = "连接打印机"
            printing = false
            paused = false
            binding.btnPrint.isEnabled = true
            /* 重启断连时 finally 可能还没跑到，这里先把按钮放开 */
            binding.btnOta.isEnabled = true
            binding.btnOtaPick.isEnabled = true
            binding.btnDotCheck.isEnabled = true
            binding.btnConnect.isEnabled = true
            binding.tvOtaInfo.text = "当前固件：未查询"

            when {
                /* 升级成功 → 固件正在重启，BLE 必然断开。启动自动重连，
                 * 进度文字由 onReconnectProgress 负责，这里不要覆盖成一句
                 * 永远停在那儿的「约 2 秒后重新连接」。 */
                otaSucceeded -> {
                    if (!ble.isAutoReconnecting() && !ble.autoReconnect()) {
                        otaSucceeded = false
                        status("升级完成 · 未记住打印机，请手动点「连接打印机」")
                    }
                }
                otaRunning -> status("连接断开：升级已中止，当前固件未改动")
                else -> status("未连接")
            }
        }
    }

    override fun onReconnectProgress(msg: String) {
        ui.post { status(msg) }
    }

    override fun onReconnectFinished(ok: Boolean) {
        ui.post {
            otaSucceeded = false
            if (!ok) {
                binding.btnConnect.text = "连接打印机"
                status("自动重连没成功：请手动点「连接打印机」（固件已升级，无需重烧）")
            }
        }
    }

    override fun onEvent(event: PrinterProtocol.Event) {
        ui.post {
            when (event) {
                is PrinterProtocol.Event.StatusReport -> {
                    val s = event.status
                    doneLines = s.doneLines
                    paused = s.paused
                    paperOk = s.paperOk
                    /* 缺纸时禁掉两个会出纸的按钮：固件会拒绝正式打印，但按钮
                     * 点得动却毫无反应，很容易被当成卡死。有纸后自动恢复。
                     * 打印中 / OTA / 自动重连期间不改，避免和别的流程抢按钮状态。 */
                    if (!printing && !otaRunning && !ble.isAutoReconnecting()) {
                        binding.btnPrint.isEnabled = s.paperOk
                        binding.btnDotCheck.isEnabled = s.paperOk
                    }
                    /* OTA 传输和自动重连期间状态上报照常来，但别把进度文字覆盖掉 */
                    if (!otaRunning && !ble.isAutoReconnecting()) {
                        if (s.paused) {
                            /* 暂停期间 printing 保持 true、打印按钮保持禁用：
                             * 不能再点一次重新 BEGIN，否则断点就没了 */
                            status(
                                "打印头过热 %d℃ · 已暂停 · 降到 45℃ 自动从第 %d 行续打（请勿重复点击）"
                                    .format(s.temp, s.doneLines)
                            )
                        } else {
                            status(
                                "已连接   %s   %d℃   %s   ·   %d/%d 行".format(
                                    batteryText(s),
                                    s.temp,
                                    if (s.paperOk) "有纸" else "缺纸",
                                    s.doneLines, totalLines
                                )
                            )
                        }
                    }
                    if (printing && !s.printing && !s.paused && s.doneLines >= totalLines) {
                        printing = false
                        binding.btnPrint.isEnabled = true
                        status("打印完成")
                    }
                }
                is PrinterProtocol.Event.Error -> {
                    printing = false
                    paused = false
                    binding.btnPrint.isEnabled = true
                    status("故障：" + when (event.code) {
                        1 -> "缺纸"
                        2 -> "打印头过热（冷却超时或传感器故障，作业已中止）"
                        3 -> "电量不足"
                        4 -> "数据超时"
                        5 -> "数据帧错误（检查协议版本）"
                        else -> "代码 ${event.code}"
                    })
                }
                is PrinterProtocol.Event.OtaInfoReport -> {
                    val i = event.info
                    otaInfos.trySend(i)
                    binding.tvOtaInfo.text = "当前固件 v%d · 运行 %s · 备用分区 %d KB · MTU %d · %s"
                        .format(
                            i.build,
                            when (i.slot) { 0 -> "app0"; 1 -> "app1"; else -> "未知" },
                            i.maxSize / 1024,
                            ble.currentMtu(),
                            if (i.build >= 86) "支持 BLE OTA" else "不支持 OTA（需先串口烧 v86）"
                        )
                }
                is PrinterProtocol.Event.OtaAckReport -> otaAcks.trySend(event.ack)
                is PrinterProtocol.Event.Ack -> Unit
            }
        }
    }

    override fun onLog(msg: String) {
        ui.post {
            status(msg)
            /* MTU 是 OTA 速度的第一决定因素，把它常驻显示在固件信息行，
             * 别只一闪而过地写进状态栏 */
            val m = Regex("蓝牙 MTU = (\\d+)").find(msg)
            if (m != null) {
                val cur = binding.tvOtaInfo.text.toString()
                val next = "MTU ${m.groupValues[1]}"
                binding.tvOtaInfo.text = if (cur.contains("MTU "))
                    cur.replace(Regex("MTU \\d+"), next) else "$cur · $next"
            }
        }
    }

    /* ---------------- 小工具 ---------------- */

    private fun status(text: String) {
        binding.tvStatus.text = text
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    /** SeekBar 只要 onProgressChanged 就够；需要「松手才下发」的另传 onStop */
    private class SimpleSeek(
        private val onChange: () -> Unit,
        private val onStop: (() -> Unit)? = null
    ) :
        android.widget.SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: android.widget.SeekBar?, p: Int, fromUser: Boolean) {
            if (fromUser) onChange()
        }

        override fun onStartTrackingTouch(sb: android.widget.SeekBar?) = Unit
        override fun onStopTrackingTouch(sb: android.widget.SeekBar?) { onStop?.invoke() }
    }
}
