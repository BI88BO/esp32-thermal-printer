package com.example.miniprinter.ble

/**
 * 与固件对接的帧协议，见 newproject/PROTOCOL.md
 *
 * 帧格式：0xA5 | TYPE | LEN | PAYLOAD | CHK
 * CHK = TYPE ^ LEN ^ 所有 payload 字节
 */
object PrinterProtocol {

    const val HEAD = 0xA5

    /* App -> 打印机 */
    const val CMD_BEGIN = 0x01
    const val CMD_DATA = 0x02
    const val CMD_END = 0x03
    const val CMD_FEED = 0x10
    const val CMD_SET_HEAT = 0x11
    const val CMD_SET_QUALITY = 0x12
    const val CMD_SET_DRIVE = 0x13
    const val CMD_GET_STATUS = 0x20
    const val CMD_SELFTEST = 0x30

    /* BLE OTA（build 86 起）。发的是纯 app 镜像 firmware.bin，
     * 不是含 bootloader/分区表的 merged_firmware.bin */
    const val CMD_OTA_INFO = 0x40
    const val CMD_OTA_BEGIN = 0x41
    const val CMD_OTA_DATA = 0x42
    const val CMD_OTA_END = 0x43
    const val CMD_OTA_ABORT = 0x44

    /* 打印机 -> App */
    const val EVT_STATUS = 0x80
    const val EVT_ACK = 0x81
    const val EVT_ERROR = 0x82
    const val EVT_OTA_INFO = 0x83
    const val EVT_OTA_ACK = 0x84

    /* 一帧 CMD_OTA_DATA = head/type/len(3) + seq(2) + data + chk(1) = data + 6。
     * MTU 247 时单次 BLE 写最多 244B，所以 data 取 238 → 整帧 244B，一次写完。
     * 旧值 243 让整帧变 249B，被切成 244 + 5，那个 5B 碎片白占一次写和 3ms 节流，
     * 4600 多帧累计约 40 秒纯空等。固件侧 243 只是「允许的上限」，用 238 完全兼容。 */
    const val OTA_BLOCK_DATA = 238
    const val OTA_WINDOW = 16          // 每批 16 块，发完停等一次 ACK（固件队列 20）

    /** 按实际协商到的 MTU 选块大小，保证整帧不切片：frame = block + 6 <= mtu - 3 */
    fun otaBlockForMtu(mtu: Int): Int =
        (mtu - 9).coerceAtMost(OTA_BLOCK_DATA).coerceAtLeast(8)

    /* 一行 384 点 = 48 字节 */
    const val LINE_BYTES = 48

    /* 一包几行。payload = 1(行序号) + 48*n，固件上限 PROTO_MAX_PAYLOAD=245：
     *   4 行 = 193 字节，整帧 197 字节，MTU 247 时一次写就能发完
     *   5 行 = 241 字节，整帧 245 字节，超出单次写上限要切片
     * 取 4 更稳，而且和固件的 245 之间有 48 字节余量 */
    const val LINES_PER_PACKET = 4

    /** 状态上报。paused = 打印头过热暂停，固件冷却后会自动从断点续打 */
    data class Status(
        val paperOk: Boolean,
        val temp: Int,
        val batteryMv: Int,
        val printing: Boolean,
        val paused: Boolean,
        val doneLines: Int,
        /** 电量百分比 0~100；固件 build 94 起才有这个字段，旧固件为 -1 */
        val batteryPct: Int = -1
    )

    /** 固件版本、当前运行的 slot（0=app0 1=app1）、备用分区可写上限 */
    data class OtaInfo(
        val build: Int,
        val slot: Int,
        val maxSize: Long,
        val active: Boolean,
        val written: Long
    )

    /** OTA 应答：nextSeq 是固件期望的下一块，缺口就从这里补发 */
    data class OtaAck(val cmd: Int, val code: Int, val nextSeq: Int, val received: Long)

    sealed class Event {
        data class StatusReport(val status: Status) : Event()
        data class Ack(val cmd: Int, val code: Int) : Event()
        data class Error(val code: Int) : Event()
        data class OtaInfoReport(val info: OtaInfo) : Event()
        data class OtaAckReport(val ack: OtaAck) : Event()
    }

    fun otaErrorText(code: Int): String = when (code) {
        0 -> "成功"
        1 -> "打印机忙（等当前作业结束再升级）"
        2 -> "镜像大小不合法"
        3 -> "备用分区空间不足"
        4 -> "升级会话状态错误"
        5 -> "写 flash 失败"
        6 -> "接收队列满"
        7 -> "数据不完整"
        8 -> "CRC32 校验失败（文件传坏了）"
        9 -> "已取消"
        else -> "未知错误 $code"
    }

    /* ---------------- 组帧 ---------------- */

    fun frame(type: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(payload.size + 4)
        out[0] = HEAD.toByte()
        out[1] = type.toByte()
        out[2] = payload.size.toByte()

        var chk = type xor payload.size
        payload.copyInto(out, 3)
        for (b in payload) chk = chk xor (b.toInt() and 0xFF)
        out[3 + payload.size] = chk.toByte()
        return out
    }

    fun begin(totalLines: Int) = frame(
        CMD_BEGIN,
        byteArrayOf((totalLines and 0xFF).toByte(), ((totalLines shr 8) and 0xFF).toByte())
    )

    fun end() = frame(CMD_END, ByteArray(0))

    fun feed(beats: Int) = frame(
        CMD_FEED,
        byteArrayOf((beats and 0xFF).toByte(), ((beats shr 8) and 0xFF).toByte())
    )

    fun setHeat(us: Int) = frame(
        CMD_SET_HEAT,
        byteArrayOf((us and 0xFF).toByte(), ((us shr 8) and 0xFF).toByte())
    )

    fun getStatus() = frame(CMD_GET_STATUS, ByteArray(0))

    /**
     * 打印质量：影响「点与点深浅是否一致」，拆机头效果最明显。
     * @param preheatUs 预加热脉冲宽度，0 = 关闭
     * @param repeat    同一行加热遍数，1 = 关闭
     * @param dotComp   点密度补偿百分比（满 64 点黑时额外加多少），0 = 关闭
     */
    fun setQuality(preheatUs: Int, repeat: Int, dotComp: Int) = frame(
        CMD_SET_QUALITY,
        byteArrayOf(
            (preheatUs and 0xFF).toByte(), ((preheatUs shr 8) and 0xFF).toByte(),
            repeat.toByte(), dotComp.toByte()
        )
    )

    /** 自检：kind = 0 整页自检页，1 打印头点检（384 根竖条，看弱点） */
    fun selftest(kind: Int) = frame(CMD_SELFTEST, byteArrayOf(kind.toByte()))

    /* ---------------- BLE OTA ---------------- */

    fun otaInfo() = frame(CMD_OTA_INFO, ByteArray(0))
    fun otaEnd() = frame(CMD_OTA_END, ByteArray(0))
    fun otaAbort() = frame(CMD_OTA_ABORT, ByteArray(0))

    /** size = firmware.bin 字节数，crc = java.util.zip.CRC32 的值（无符号低 32 位） */
    fun otaBegin(size: Int, crc: Long) = frame(
        CMD_OTA_BEGIN,
        byteArrayOf(
            (size and 0xFF).toByte(), ((size shr 8) and 0xFF).toByte(),
            ((size shr 16) and 0xFF).toByte(), ((size shr 24) and 0xFF).toByte(),
            (crc and 0xFF).toByte(), ((crc shr 8) and 0xFF).toByte(),
            ((crc shr 16) and 0xFF).toByte(), ((crc shr 24) and 0xFF).toByte()
        )
    )

    /** 第 seq 块，取 image[off .. off+len)，len <= OTA_BLOCK_DATA */
    fun otaData(seq: Int, image: ByteArray, off: Int, len: Int): ByteArray {
        require(len in 1..OTA_BLOCK_DATA)
        val payload = ByteArray(len + 2)
        payload[0] = (seq and 0xFF).toByte()
        payload[1] = ((seq shr 8) and 0xFF).toByte()
        image.copyInto(payload, 2, off, off + len)
        return frame(CMD_OTA_DATA, payload)
    }

    private fun le32(b: ByteArray, o: Int): Long =
        (b[o].toLong() and 0xFF) or
                ((b[o + 1].toLong() and 0xFF) shl 8) or
                ((b[o + 2].toLong() and 0xFF) shl 16) or
                ((b[o + 3].toLong() and 0xFF) shl 24)

    /**
     * 电机驱动方式，硬件不动，只换换相序列。
     * @param dual 0 = 1-2 相半步（一行 4 拍，省电）；1 = 双相整步（一行 2 拍，力矩恒定）
     */
    fun setDrive(dual: Int) = frame(CMD_SET_DRIVE, byteArrayOf(dual.toByte()))

    /**
     * 打包若干行位图。seq 用于让固件检测丢包，从 0 开始递增。
     * rows 长度必须是 48 的整数倍，且不超过 5 行。
     */
    fun data(seq: Int, rows: ByteArray): ByteArray {
        require(rows.size % LINE_BYTES == 0) { "rows 必须是整行" }
        val n = rows.size / LINE_BYTES
        require(n in 1..LINES_PER_PACKET) { "一包 1~5 行" }

        val payload = ByteArray(rows.size + 1)
        payload[0] = (seq and 0xFF).toByte()
        rows.copyInto(payload, 1)
        return frame(CMD_DATA, payload)
    }

    /* ---------------- 解析通知 ---------------- */

    /**
     * 从字节流里切帧。BLE 可能把一帧拆成多个通知，也可能合并，
     * 所以调用方要保留一个累积缓冲，这里只管从缓冲里取出完整帧。
     *
     * @return 解析出的事件；缓冲里还没有完整帧时返回 null
     */
    fun parse(buffer: MutableList<Byte>): Event? {
        while (buffer.size >= 3) {
            if (buffer[0] != HEAD.toByte()) {
                buffer.removeAt(0)          // 不同步，丢字节重新找帧头
                continue
            }
            val type = buffer[1].toInt() and 0xFF
            val len = buffer[2].toInt() and 0xFF
            if (buffer.size < len + 4) return null      // 帧还没收全

            var chk = type xor len
            for (i in 0 until len) chk = chk xor (buffer[3 + i].toInt() and 0xFF)

            if (chk != (buffer[3 + len].toInt() and 0xFF)) {
                buffer.removeAt(0)          // 校验失败，丢掉帧头重来
                continue
            }

            val payload = ByteArray(len) { buffer[3 + it] }
            repeat(len + 4) { buffer.removeAt(0) }

            return when (type) {
                EVT_STATUS -> {
                    if (len < 7) null else {
                        /* state: 0=空闲 1=打印/收尾 2=过热暂停（会自动续打）。
                         * 2 也算「忙」，否则打印完成判定会提前触发 */
                        val state = payload[4].toInt() and 0xFF
                        Event.StatusReport(
                            Status(
                                paperOk = payload[0].toInt() == 0,
                                temp = payload[1].toInt() and 0xFF,
                                batteryMv = (payload[2].toInt() and 0xFF) or
                                        ((payload[3].toInt() and 0xFF) shl 8),
                                printing = state != 0,
                                paused = state == 2,
                                doneLines = (payload[5].toInt() and 0xFF) or
                                        ((payload[6].toInt() and 0xFF) shl 8),
                                /* 第 8 字节是 build 94 新增的电量百分比。
                                 * 旧固件只有 7 字节，这里给 -1 让界面退回只显示电压 */
                                batteryPct = if (len >= 8) payload[7].toInt() and 0xFF else -1
                            )
                        )
                    }
                }
                EVT_ACK -> if (len < 2) null else
                    Event.Ack(payload[0].toInt() and 0xFF, payload[1].toInt() and 0xFF)
                EVT_ERROR -> if (len < 1) null else
                    Event.Error(payload[0].toInt() and 0xFF)
                EVT_OTA_INFO -> if (len < 11) null else
                    Event.OtaInfoReport(
                        OtaInfo(
                            build = payload[0].toInt() and 0xFF,
                            slot = payload[1].toInt() and 0xFF,
                            maxSize = le32(payload, 2),
                            active = payload[6].toInt() != 0,
                            written = le32(payload, 7)
                        )
                    )
                EVT_OTA_ACK -> if (len < 8) null else
                    Event.OtaAckReport(
                        OtaAck(
                            cmd = payload[0].toInt() and 0xFF,
                            code = payload[1].toInt() and 0xFF,
                            nextSeq = (payload[2].toInt() and 0xFF) or
                                    ((payload[3].toInt() and 0xFF) shl 8),
                            received = le32(payload, 4)
                        )
                    )
                else -> null
            }
        }
        return null
    }
}
