package cz.dete.chuvickafinder

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.ArrayAdapter
import android.widget.SeekBar
import androidx.appcompat.app.AppCompatActivity
import cz.dete.chuvickafinder.databinding.ActivityMainBinding
import cz.dete.chuvickafinder.dsp.ExponentialAverage
import cz.dete.chuvickafinder.dsp.SignalProcessor
import cz.dete.chuvickafinder.feedback.GeigerBeeper
import cz.dete.chuvickafinder.sdr.BandPresets
import cz.dete.chuvickafinder.sdr.RtlTcpClient
import java.util.Locale
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val ui = Handler(Looper.getMainLooper())

    private val rtlClient = RtlTcpClient()
    private val signalProcessor = SignalProcessor()
    private val smoother = ExponentialAverage(alpha = 0.3)
    private val geigerBeeper = GeigerBeeper()

    private val sampleRates = listOf(
        "2.048 MHz (výchozí)" to 2_048_000,
        "1.024 MHz" to 1_024_000,
        "0.25 MHz (úzké pásmo, přesnější)" to 250_000,
    )

    // Cached copies of UI state that the background reader/sweep threads need, so they
    // never have to touch a View off the main thread.
    @Volatile private var currentSampleRateHz: Int = sampleRates[0].second

    @Volatile private var isMeasuring = false
    @Volatile private var isCalibrating = false
    @Volatile private var isSweeping = false
    @Volatile private var lastCenterDb: Double? = null
    @Volatile private var noiseFloorDb: Double? = null
    @Volatile private var maxAboveFloorDb: Double = 20.0 // adaptive full-scale for the meter

    private val calibrationSamples = mutableListOf<Double>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        setupPresetSpinner()
        setupSampleRateSpinner()
        setupGainControls()
        setupFrequencyField()
        setupButtons()
    }

    /** Fire-and-forget a blocking rtl_tcp command off the main thread. */
    private fun sendToDevice(action: () -> Unit) {
        thread {
            try { action() } catch (_: Exception) { /* connection dropped meanwhile */ }
        }
    }

    // ---------------------------------------------------------------- setup

    private fun setupPresetSpinner() {
        val labels = BandPresets.presets.map { it.label }
        binding.spinnerPreset.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)
        binding.spinnerPreset.setSelection(2) // 863-865 MHz, most common EU band today
        binding.etFrequency.setText(formatMhz(BandPresets.presets[2].centerMhz))
        binding.spinnerPreset.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, position: Int, id: Long) {
                val preset = BandPresets.presets[position]
                if (preset.centerMhz > 0.0) {
                    binding.etFrequency.setText(formatMhz(preset.centerMhz))
                }
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }
    }

    private fun setupSampleRateSpinner() {
        binding.spinnerSampleRate.adapter =
            ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, sampleRates.map { it.first })
        binding.spinnerSampleRate.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, position: Int, id: Long) {
                val hz = sampleRates[position].second
                currentSampleRateHz = hz
                if (rtlClient.isConnected) sendToDevice { rtlClient.setSampleRateHz(hz.toLong()) }
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }
    }

    private fun setupGainControls() {
        updateGainLabel(binding.seekBarGain.progress)
        binding.radioGroupGain.setOnCheckedChangeListener { _, checkedId ->
            val manual = checkedId == binding.radioManual.id
            binding.seekBarGain.isEnabled = manual
            val gainTenths = binding.seekBarGain.progress
            if (rtlClient.isConnected) {
                sendToDevice {
                    rtlClient.setAutoGain(!manual)
                    if (manual) rtlClient.setManualGain(gainTenths)
                }
            }
        }
        binding.seekBarGain.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                updateGainLabel(progress)
                if (fromUser && rtlClient.isConnected && binding.radioManual.isChecked) {
                    sendToDevice { rtlClient.setManualGain(progress) }
                }
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    private fun updateGainLabel(tenthsDb: Int) {
        binding.tvGainValue.text = String.format(Locale.getDefault(), "%.1f dB", tenthsDb / 10.0)
    }

    private fun setupFrequencyField() {
        binding.etFrequency.addTextChangedListener(object : android.text.TextWatcher {
            override fun afterTextChanged(s: android.text.Editable?) {
                val mhz = s?.toString()?.toDoubleOrNull()
                binding.tvRangeWarning.visibility =
                    if (mhz != null && !BandPresets.isWithinTypicalDongleRange(mhz)) View.VISIBLE else View.GONE
            }
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
        })
    }

    private fun setupButtons() {
        binding.btnConnect.setOnClickListener {
            if (rtlClient.isConnected) disconnect() else connect()
        }
        binding.btnStartStop.setOnClickListener {
            if (isMeasuring) stopMeasuring() else startMeasuring()
        }
        binding.btnCalibrate.setOnClickListener { startCalibration() }
        binding.btnSweep.setOnClickListener { startSweep() }
    }

    // ------------------------------------------------------------- connect

    private fun connect() {
        val host = binding.etHost.text?.toString()?.trim().orEmpty().ifEmpty { "127.0.0.1" }
        val port = binding.etPort.text?.toString()?.trim()?.toIntOrNull() ?: 1234
        val sampleRate = sampleRates[binding.spinnerSampleRate.selectedItemPosition.coerceAtLeast(0)].second
        val autoGain = binding.radioAuto.isChecked
        val manualGainTenths = binding.seekBarGain.progress
        val frequencyHz = parseFrequencyHz()

        currentSampleRateHz = sampleRate
        binding.btnConnect.isEnabled = false
        binding.tvConnectionStatus.text = getString(R.string.status_connecting)

        thread {
            try {
                val info = rtlClient.connect(host, port)
                rtlClient.setSampleRateHz(sampleRate.toLong())
                rtlClient.setFrequencyHz(frequencyHz)
                rtlClient.setAutoGain(autoGain)
                if (!autoGain) rtlClient.setManualGain(manualGainTenths)

                thread(name = "rtl-tcp-reader") { readLoop() }

                ui.post {
                    binding.tvConnectionStatus.text =
                        getString(R.string.status_connected, RtlTcpClient.tunerName(info.tunerType))
                    binding.btnConnect.text = getString(R.string.btn_disconnect)
                    binding.btnConnect.isEnabled = true
                    binding.btnStartStop.isEnabled = true
                    binding.btnCalibrate.isEnabled = true
                    binding.btnSweep.isEnabled = true
                }
            } catch (e: Exception) {
                ui.post {
                    binding.tvConnectionStatus.text = getString(R.string.status_error, e.message ?: e.toString())
                    binding.btnConnect.isEnabled = true
                }
            }
        }
    }

    private fun disconnect() {
        stopMeasuring()
        rtlClient.close()
        binding.tvConnectionStatus.text = getString(R.string.status_disconnected)
        binding.btnConnect.text = getString(R.string.btn_connect)
        binding.btnStartStop.isEnabled = false
        binding.btnCalibrate.isEnabled = false
        binding.btnSweep.isEnabled = false
        noiseFloorDb = null
        binding.tvCalibrateStatus.text = ""
    }

    /** Reads and parses the frequency field on the calling (main) thread. Does not touch the socket. */
    private fun parseFrequencyHz(): Long {
        val mhz = binding.etFrequency.text?.toString()?.toDoubleOrNull() ?: BandPresets.presets[2].centerMhz
        return (mhz * 1_000_000.0).toLong()
    }

    // -------------------------------------------------------------- reader

    private fun readLoop() {
        rtlClient.readLoop { buffer, count ->
            val db = signalProcessor.centerPowerDb(buffer, count, currentSampleRateHz) ?: return@readLoop
            lastCenterDb = db

            if (isCalibrating) {
                synchronized(calibrationSamples) { calibrationSamples.add(db) }
            }
            if (isMeasuring) {
                val floor = noiseFloorDb
                val relative = if (floor != null) db - floor else 0.0
                val smoothed = smoother.update(relative)
                if (smoothed > maxAboveFloorDb) maxAboveFloorDb = smoothed * 1.1
                val normalized = (smoothed / maxAboveFloorDb).coerceIn(0.0, 1.0)
                geigerBeeper.setLevel(normalized)
                ui.post {
                    binding.meterView.setLevel(normalized.toFloat())
                    binding.tvRelativeDb.text = getString(R.string.label_relative_db, smoothed)
                }
            }
        }
    }

    // ----------------------------------------------------------- measuring

    private fun startMeasuring() {
        if (noiseFloorDb == null) {
            binding.tvCalibrateStatus.text = "Tip: nejdřív klikni na \"Kalibrovat ticho\" pro přesnější odečet."
        }
        val hz = parseFrequencyHz()
        sendToDevice { rtlClient.setFrequencyHz(hz) }
        smoother.reset()
        isMeasuring = true
        binding.btnStartStop.text = getString(R.string.btn_stop)
        if (binding.switchAudioFeedback.isChecked) geigerBeeper.start()
    }

    private fun stopMeasuring() {
        isMeasuring = false
        geigerBeeper.stop()
        binding.btnStartStop.text = getString(R.string.btn_start)
        binding.meterView.setLevel(0f)
    }

    // --------------------------------------------------------- calibration

    private fun startCalibration() {
        if (isCalibrating) return
        synchronized(calibrationSamples) { calibrationSamples.clear() }
        isCalibrating = true
        val durationSec = 3
        var remaining = durationSec
        binding.tvCalibrateStatus.text = getString(R.string.calibrating, remaining)

        val tick = object : Runnable {
            override fun run() {
                remaining--
                if (remaining <= 0) {
                    isCalibrating = false
                    val samples = synchronized(calibrationSamples) { calibrationSamples.toList() }
                    if (samples.isNotEmpty()) {
                        val floor = samples.sorted()[samples.size / 2] // median, robust to spikes
                        noiseFloorDb = floor
                        maxAboveFloorDb = 20.0
                        binding.tvCalibrateStatus.text =
                            String.format(Locale.getDefault(), "Šumové pozadí nastaveno (%.1f dB).", floor)
                    } else {
                        binding.tvCalibrateStatus.text = "Kalibrace selhala - nepřišla žádná data."
                    }
                } else {
                    binding.tvCalibrateStatus.text = getString(R.string.calibrating, remaining)
                    ui.postDelayed(this, 1000)
                }
            }
        }
        ui.postDelayed(tick, 1000)
    }

    // -------------------------------------------------------------- sweep

    private fun startSweep() {
        if (isSweeping || !rtlClient.isConnected) return
        val centerHz = parseFrequencyHz()
        val spanHz = 300_000L
        val stepHz = 20_000L
        val steps = ((2 * spanHz) / stepHz).toInt() + 1

        isSweeping = true
        binding.btnSweep.isEnabled = false
        val wasMeasuring = isMeasuring
        isMeasuring = false

        thread(name = "sweep") {
            val results = mutableListOf<Pair<Long, Double>>()
            for (i in 0 until steps) {
                val freq = centerHz - spanHz + i * stepHz
                rtlClient.setFrequencyHz(freq)
                val db = readAveragedDb(settleMs = 120, pollMs = 20, polls = 6)
                if (db != null) results.add(freq to db)
                val progress = i + 1
                ui.post { binding.tvSweepStatus.text = getString(R.string.sweep_progress, progress, steps) }
            }
            rtlClient.setFrequencyHz(centerHz)

            ui.post {
                isSweeping = false
                isMeasuring = wasMeasuring
                binding.btnSweep.isEnabled = true
                if (results.isEmpty()) {
                    binding.tvSweepStatus.text = "Prohledávání se nezdařilo."
                    return@post
                }
                val floor = results.minOf { it.second }
                val sorted = results.sortedByDescending { it.second }
                val best = sorted.first()
                binding.tvSweepStatus.text = getString(R.string.sweep_result, best.first / 1_000_000.0)
                binding.tvSweepResults.text = sorted.take(8).joinToString("\n") { (freq, db) ->
                    String.format(Locale.getDefault(), "%.3f MHz   %+5.1f dB", freq / 1_000_000.0, db - floor)
                }
            }
        }
    }

    private fun readAveragedDb(settleMs: Long, pollMs: Long, polls: Int): Double? {
        try { Thread.sleep(settleMs) } catch (_: InterruptedException) {}
        val values = mutableListOf<Double>()
        repeat(polls) {
            lastCenterDb?.let { values.add(it) }
            try { Thread.sleep(pollMs) } catch (_: InterruptedException) {}
        }
        if (values.isEmpty()) return null
        return values.average()
    }

    private fun formatMhz(mhz: Double): String = String.format(Locale.getDefault(), "%.3f", mhz)

    override fun onDestroy() {
        super.onDestroy()
        stopMeasuring()
        rtlClient.close()
    }
}
