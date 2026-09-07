package cz.dete.chuvickafinder.sdr

import java.io.DataOutputStream
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Client for the well-known `rtl_tcp` protocol (part of the librtlsdr project).
 *
 * This app does not talk to the RTL-SDR USB dongle directly. Instead it connects, as a
 * plain TCP client, to a locally-running `rtl_tcp`-compatible server. On Android that
 * server is normally provided by the separate, open-source "RTL2832U Driver" app, which
 * already implements the tricky low-level USB / tuner control and exposes it over this
 * simple, stable socket protocol - see README.md for setup instructions.
 *
 * Wire format (unchanged for over a decade, used by GNU Radio's rtl_tcp_source and many
 * other clients):
 *  - On connect, the server sends a 12 byte header: magic "RTL0" (4 bytes), tuner type
 *    (big-endian uint32) and tuner gain count (big-endian uint32).
 *  - After the header the server streams raw interleaved unsigned 8-bit I/Q samples
 *    forever.
 *  - The client controls the tuner by sending 5 byte commands: 1 command id byte followed
 *    by a big-endian uint32 parameter.
 */
class RtlTcpClient {

    enum class Command(val id: Byte) {
        SET_FREQUENCY(0x01),
        SET_SAMPLE_RATE(0x02),
        SET_GAIN_MODE(0x03),
        SET_GAIN(0x04),
        SET_FREQ_CORRECTION(0x05),
        SET_AGC_MODE(0x08),
    }

    data class HandshakeInfo(val tunerType: Int, val gainCount: Int)

    private var socket: Socket? = null
    private var output: DataOutputStream? = null
    private var input: InputStream? = null
    private val running = AtomicBoolean(false)

    val isConnected: Boolean
        get() = socket?.isConnected == true && running.get()

    @Throws(Exception::class)
    fun connect(host: String, port: Int, timeoutMs: Int = 4000): HandshakeInfo {
        val s = Socket()
        s.connect(InetSocketAddress(host, port), timeoutMs)
        s.tcpNoDelay = true
        socket = s
        output = DataOutputStream(s.getOutputStream())
        input = s.getInputStream()

        val header = ByteArray(12)
        readFully(input!!, header)
        val bb = ByteBuffer.wrap(header).order(ByteOrder.BIG_ENDIAN)
        val magic = ByteArray(4)
        bb.get(magic)
        val magicStr = String(magic, Charsets.US_ASCII)
        if (magicStr != "RTL0") {
            throw java.io.IOException("Neplatná odpověď serveru (magic=$magicStr), není to rtl_tcp?")
        }
        val tunerType = bb.int
        val gainCount = bb.int
        running.set(true)
        return HandshakeInfo(tunerType, gainCount)
    }

    fun sendCommand(cmd: Command, param: Long) {
        val out = output ?: return
        synchronized(out) {
            val buf = ByteBuffer.allocate(5).order(ByteOrder.BIG_ENDIAN)
            buf.put(cmd.id)
            buf.putInt(param.toInt())
            out.write(buf.array())
            out.flush()
        }
    }

    fun setFrequencyHz(hz: Long) = sendCommand(Command.SET_FREQUENCY, hz)
    fun setSampleRateHz(hz: Long) = sendCommand(Command.SET_SAMPLE_RATE, hz)
    fun setAutoGain(auto: Boolean) {
        sendCommand(Command.SET_GAIN_MODE, if (auto) 0L else 1L)
        sendCommand(Command.SET_AGC_MODE, if (auto) 1L else 0L)
    }
    /** [tenthsOfDb] is gain in tenths of a dB, e.g. 300 = 30.0 dB. */
    fun setManualGain(tenthsOfDb: Int) = sendCommand(Command.SET_GAIN, tenthsOfDb.toLong())

    /**
     * Blocking read loop - call from a background thread. Invokes [onSamples] with each
     * chunk of raw interleaved unsigned 8-bit I/Q bytes as they arrive. Returns (and the
     * loop stops) when the connection is closed or [stop] is called.
     */
    fun readLoop(chunkSize: Int = 32 * 1024, onSamples: (ByteArray, Int) -> Unit) {
        val stream = input ?: return
        val buffer = ByteArray(chunkSize)
        try {
            while (running.get()) {
                val read = stream.read(buffer)
                if (read <= 0) break
                onSamples(buffer, read)
            }
        } catch (_: Exception) {
            // Socket closed from stop()/close() - nothing else to do.
        }
    }

    fun stop() {
        running.set(false)
    }

    fun close() {
        running.set(false)
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        output = null
        input = null
    }

    private fun readFully(stream: InputStream, dst: ByteArray) {
        var offset = 0
        while (offset < dst.size) {
            val read = stream.read(dst, offset, dst.size - offset)
            if (read < 0) throw java.io.EOFException("Spojení se serverem bylo ukončeno během handshake")
            offset += read
        }
    }

    companion object {
        /** Human readable tuner names for the handshake tuner_type field (from librtlsdr). */
        fun tunerName(type: Int): String = when (type) {
            1 -> "E4000"
            2 -> "FC0012"
            3 -> "FC0013"
            4 -> "FC2580"
            5 -> "R820T"
            6 -> "R828D"
            else -> "neznámý ($type)"
        }
    }
}
