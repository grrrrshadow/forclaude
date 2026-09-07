package cz.dete.chuvickafinder.sdr

/**
 * Common radio bands used by (mostly analog/FM) children's baby monitors sold in the EU.
 * The exact carrier frequency varies by model/channel - check the FCC ID / CE label on the
 * monitor itself (usually inside the battery compartment) if you need to be sure. These are
 * meant as a starting point for the frequency field and for the "search nearby" sweep.
 */
data class BandPreset(
    val label: String,
    val centerMhz: Double,
    val note: String,
    val coveredByTypicalDongle: Boolean,
)

object BandPresets {
    /** Typical tuning range of a cheap R820T2-based RTL-SDR dongle (e.g. RTL-SDR Blog V3/V4). */
    const val TYPICAL_DONGLE_MIN_MHZ = 24.0
    const val TYPICAL_DONGLE_MAX_MHZ = 1700.0

    val presets = listOf(
        BandPreset("40 MHz (starší analogové chůvičky)", 40.680, "Pásmo 40,665–40,700 MHz", true),
        BandPreset("49 MHz (starší analogové chůvičky)", 49.860, "Pásmo 49,830–49,890 MHz", true),
        BandPreset("863–865 MHz (SRD860, běžné EU chůvičky)", 864.000, "Nejčastější pásmo dnešních analogových chůviček v EU", true),
        BandPreset("902–928 MHz (US ISM)", 915.000, "Používá se hlavně mimo EU", true),
        BandPreset("1880–1900 MHz (DECT)", 1890.000, "Mnoho \"DECT\" chůviček - mimo dosah běžného RTL-SDR dongle!", false),
        BandPreset("2,4 GHz (digitální/WiFi kamery)", 2440.000, "Mimo dosah běžného RTL-SDR dongle - řeš spíš WiFi/BLE skenováním, ne SDR", false),
        BandPreset("Vlastní frekvence", 0.0, "Zadej ručně podle štítku na chůvičce", true),
    )

    fun isWithinTypicalDongleRange(mhz: Double): Boolean =
        mhz in TYPICAL_DONGLE_MIN_MHZ..TYPICAL_DONGLE_MAX_MHZ
}
