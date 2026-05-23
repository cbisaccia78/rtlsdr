# RTL-SDR GTK Spectrum Viewer

This project is a small RTL-SDR desktop application written in C. It uses GTK 4 for the GUI, `librtlsdr` for hardware access, and Apple Accelerate for FFT processing. The code is split into a radio engine layer and a UI layer.

## File Layout

- `radio_engine.h`: public API for the RTL-SDR engine.
- `radio_engine.c`: worker thread, device control, IQ normalization, and FFT generation.
- `main.c`: GTK application, controls, and spectrum rendering.
- `Makefile`: build configuration using `pkg-config`.

## Function Reference

### radio_engine.h / radio_engine.c

#### `RadioEngine *radio_engine_new(void)`
Creates the radio engine and initializes all long-lived resources.

Responsibilities:
- allocates the engine struct
- initializes the mutex used to protect shared state
- creates the Accelerate DFT setup used for the spectrum analyzer
- builds the Hann window used before each FFT
- seeds default center frequency and sample rate
- queries the current RTL-SDR device count

Returns:
- a valid `RadioEngine *` on success
- `NULL` if allocation or FFT setup creation fails

#### `void radio_engine_free(RadioEngine *engine)`
Stops any active worker thread, destroys FFT resources, clears synchronization state, and frees the engine.

Notes:
- safe to call with `NULL`
- this is the owning destructor for the engine

#### `bool radio_engine_set_center_freq(RadioEngine *engine, uint32_t center_freq_hz, char *error_message, size_t error_message_size)`
Updates the configured center frequency in Hz.

Behavior:
- stores the new value in engine state
- if the device is already streaming, it immediately attempts to call `rtlsdr_set_center_freq`
- reports a human-readable result string through `error_message` when provided

Returns:
- `true` if the new value is accepted and any live update succeeds
- `false` if the engine is invalid or the live tuner update fails

#### `bool radio_engine_set_sample_rate(RadioEngine *engine, uint32_t sample_rate_hz, char *error_message, size_t error_message_size)`
Updates the configured sample rate in Hz.

Behavior:
- always stores the requested value in engine state
- if the engine is idle, the value becomes the next active hardware rate
- if the engine is running, the function refuses to hot-apply the hardware change and asks for a stop/start cycle

Returns:
- `true` when the value is accepted for immediate use
- `false` when the engine is running and the caller must restart streaming

#### `bool radio_engine_start(RadioEngine *engine, uint32_t device_index, char *error_message, size_t error_message_size)`
Starts the background worker thread for the selected RTL-SDR device.

Behavior:
- validates that the engine is not already running
- resets sample counters and spectrum state
- stores the requested device index
- spawns the GLib worker thread

Important detail:
- this function only confirms that the thread was created successfully
- actual hardware setup errors happen inside the worker thread and appear later in the engine status string

#### `void radio_engine_stop(RadioEngine *engine)`
Stops the async RTL-SDR read loop and waits for the worker thread to exit.

Behavior:
- marks shutdown as requested
- calls `rtlsdr_cancel_async` when a device is open
- joins the worker thread before returning

#### `void radio_engine_get_snapshot(RadioEngine *engine, RadioEngineSnapshot *snapshot)`
Copies the latest engine state into a caller-provided snapshot.

Use this when the UI needs a consistent view of:
- whether streaming is active
- the latest status string
- the total IQ sample count
- the most recent normalized IQ sample
- the current FFT spectrum buffer

#### `static void copy_message(...)`
Internal helper that safely copies a short message into an optional output buffer.

#### `static void set_status_locked(...)`
Internal helper that formats the engine status string while the caller already owns the engine mutex.

#### `static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)`
The core data-ingestion callback invoked by `librtlsdr`.

Responsibilities:
- receives raw interleaved unsigned 8-bit IQ bytes
- converts the first sample to normalized float IQ for status display
- copies one FFT frame worth of samples into split real/imaginary arrays
- applies a Hann window to reduce spectral leakage
- executes the FFT with Accelerate
- converts FFT power to dB values for the GUI spectrum display
- publishes spectrum and sample stats back into the engine under lock

#### `static gpointer radio_engine_thread(gpointer user_data)`
Background worker that owns the hardware lifecycle.

Responsibilities:
- checks device availability
- opens the selected RTL-SDR device
- applies sample rate and center frequency
- enables automatic gain control
- resets the device buffer
- enters `rtlsdr_read_async`
- publishes final stop/error status and closes the device

### main.c

#### `static uint32_t selected_sample_rate(AppWidgets *widgets)`
Reads the selected entry from the GTK dropdown and maps it back to an exact sample-rate value in Hz.

#### `static void set_status_text(AppWidgets *widgets, const char *message)`
Writes a short message into the status label shown in the main window.

#### `static double bin_frequency_hz(const RadioEngineSnapshot *snapshot, guint bin_index)`
Converts an FFT bin index into a signed frequency offset from the tuner center frequency.

This is used for:
- the peak-bin readout
- interpreting where carriers and sidebands appear in the plot

#### `static void draw_spectrum(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)`
Draws the spectrum analyzer.

Responsibilities:
- paints the analyzer background
- draws horizontal dB grid lines
- draws vertical frequency-offset grid lines
- plots the FFT trace from `snapshot.spectrum_db`
- draws a center marker for 0 Hz
- shows a placeholder message before the first FFT frame exists

#### `static void update_stats(AppWidgets *widgets, const RadioEngineSnapshot *snapshot)`
Builds the text shown in the stats pane.

The stats include:
- device count
- center frequency
- sample rate
- total IQ sample count
- last normalized IQ sample
- strongest FFT bin and its offset from center

#### `static gboolean refresh_ui(gpointer user_data)`
Periodic GTK timer callback.

Responsibilities:
- fetches a fresh `RadioEngineSnapshot`
- updates labels and button enablement state
- requests a redraw of the spectrum analyzer

Returns `G_SOURCE_CONTINUE` so GTK keeps calling it.

#### `static gboolean apply_settings(AppWidgets *widgets, gboolean show_partial_message)`
Reads control values from the UI and applies them to the engine.

Behavior:
- reads center frequency from the spin button
- reads sample rate from the dropdown
- forwards both values to the engine
- optionally updates the status label when partial results should be shown

#### `static void on_apply_clicked(GtkButton *button, gpointer user_data)`
GTK signal handler for the `Apply settings` button.

#### `static void on_start_clicked(GtkButton *button, gpointer user_data)`
GTK signal handler for the `Start` button.

Behavior:
- applies the current settings
- starts the radio engine if settings are valid
- updates the status label

#### `static void on_stop_clicked(GtkButton *button, gpointer user_data)`
GTK signal handler for the `Stop` button.

Behavior:
- requests shutdown of the radio engine
- updates the status label immediately

#### `static GtkWidget *build_controls(AppWidgets *widgets)`
Creates the full widget tree for the main window.

This assembles:
- the center-frequency input
- the sample-rate dropdown
- Apply / Start / Stop buttons
- the spectrum analyzer drawing area
- the stats panel
- the status panel

#### `static void on_app_shutdown(GApplication *application, gpointer user_data)`
Application shutdown hook.

Responsibilities:
- removes the periodic GTK refresh source
- frees the radio engine
- frees the app widget state structure

#### `static void on_activate(GtkApplication *application, gpointer user_data)`
GTK activation hook that creates the engine, builds the main window, and starts the periodic UI refresh timer.

#### `int main(int argc, char *argv[])`
Program entry point.

Responsibilities:
- allocates the top-level widget state
- creates the GTK application object
- connects activate/shutdown handlers
- enters the GTK event loop

## Control Flow

1. `main()` creates the GTK application.
2. `on_activate()` creates the engine, builds the window, and starts the refresh timer.
3. `on_start_clicked()` applies settings and calls `radio_engine_start()`.
4. `radio_engine_start()` creates the worker thread.
5. `radio_engine_thread()` opens the RTL-SDR and enters `rtlsdr_read_async()`.
6. `rtlsdr_callback()` converts IQ samples and updates the FFT spectrum.
7. `refresh_ui()` copies the latest snapshot and redraws the spectrum.
8. `on_stop_clicked()` or shutdown eventually calls `radio_engine_stop()`.

## Notes on Spectrum Interpretation

- The center vertical line is `0 Hz` relative to the tuned center frequency.
- A strong carrier exactly at the tuned frequency appears near the middle.
- Sidebands appear to the left and right of the carrier at their offset frequencies.
- The horizontal axis currently shows offset from center, not absolute RF frequency.
- The displayed FFT is a quick real-time view, not a calibrated lab measurement.
