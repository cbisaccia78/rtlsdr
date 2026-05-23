#include <gtk/gtk.h>
#include <stdint.h>
#include <stdlib.h>

#include "radio_engine.h"

typedef struct {
    uint32_t value;
    const char *label;
} SampleRateOption;

typedef struct {
    RadioDemodMode value;
    const char *label;
} DemodModeOption;

typedef struct {
    GtkApplication *application;
    GtkLabel *status_label;
    GtkLabel *stats_label;
    GtkDrawingArea *spectrum_area;
    GtkSpinButton *center_freq_spin;
    GtkDropDown *sample_rate_dropdown;
    GtkDropDown *demod_mode_dropdown;
    GtkButton *audio_button;
    GtkButton *apply_button;
    GtkButton *start_button;
    GtkButton *stop_button;
    guint refresh_source_id;
    RadioEngineSnapshot snapshot;
    RadioEngine *engine;
} AppWidgets;

static const SampleRateOption sample_rate_options[] = {
    {256000U, "0.256 MS/s"},
    {512000U, "0.512 MS/s"},
    {768000U, "0.768 MS/s"},
    {1024000U, "1.024 MS/s"},
    {1536000U, "1.536 MS/s"},
    {2048000U, "2.048 MS/s"},
    {2400000U, "2.400 MS/s"},
    {2560000U, "2.560 MS/s"}
};

static const DemodModeOption demod_mode_options[] = {
    {RADIO_DEMOD_MODE_OFF, "Off"},
    {RADIO_DEMOD_MODE_FM, "FM"},
    {RADIO_DEMOD_MODE_AM, "AM"}
};

static const char *audio_button_label(gboolean audio_requested) {
    return audio_requested ? "Disable audio" : "Enable audio";
}

/* Read the sample-rate dropdown and map it back to a concrete Hz value. */
static uint32_t selected_sample_rate(AppWidgets *widgets) {
    guint selected = gtk_drop_down_get_selected(widgets->sample_rate_dropdown);

    if (selected >= G_N_ELEMENTS(sample_rate_options)) {
        return sample_rate_options[2].value;
    }

    return sample_rate_options[selected].value;
}

static RadioDemodMode selected_demod_mode(AppWidgets *widgets) {
    guint selected = gtk_drop_down_get_selected(widgets->demod_mode_dropdown);

    if (selected >= G_N_ELEMENTS(demod_mode_options)) {
        return RADIO_DEMOD_MODE_OFF;
    }

    return demod_mode_options[selected].value;
}

static const char *demod_mode_label(RadioDemodMode demod_mode) {
    for (size_t index = 0; index < G_N_ELEMENTS(demod_mode_options); index++) {
        if (demod_mode_options[index].value == demod_mode) {
            return demod_mode_options[index].label;
        }
    }

    return "Off";
}

/* Push a short status message into the GTK status label. */
static void set_status_text(AppWidgets *widgets, const char *message) {
    gtk_label_set_text(widgets->status_label, message);
}

/* Convert a spectrum bin index into a frequency offset from center in Hz. */
static double bin_frequency_hz(const RadioEngineSnapshot *snapshot, guint bin_index) {
    double normalized = ((double)bin_index / (double)RADIO_SPECTRUM_BINS) - 0.5;

    return normalized * (double)snapshot->sample_rate_hz;
}

/*
 * Render the live FFT spectrum into the drawing area.
 *
 * The plot uses dB on the vertical axis and sample-rate-relative frequency on
 * the horizontal axis, with the tuner center frequency positioned in the middle.
 */
static void draw_spectrum(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    AppWidgets *widgets = user_data;
    const RadioEngineSnapshot *snapshot = &widgets->snapshot;
    const double left_padding = 48.0;
    const double right_padding = 12.0;
    const double top_padding = 12.0;
    const double bottom_padding = 28.0;
    const double plot_width = width - left_padding - right_padding;
    const double plot_height = height - top_padding - bottom_padding;
    const double min_db = -120.0;
    const double max_db = 10.0;

    (void)area;

    cairo_set_source_rgb(cr, 0.07, 0.08, 0.10);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.18, 0.21, 0.24);
    cairo_rectangle(cr, left_padding, top_padding, plot_width, plot_height);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Menlo", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);

    for (int grid = 0; grid <= 4; grid++) {
        double y = top_padding + ((double)grid / 4.0) * plot_height;
        double db = max_db - ((double)grid / 4.0) * (max_db - min_db);

        cairo_set_source_rgba(cr, 0.30, 0.36, 0.40, 0.5);
        cairo_move_to(cr, left_padding, y);
        cairo_line_to(cr, left_padding + plot_width, y);
        cairo_stroke(cr);

        cairo_set_source_rgb(cr, 0.70, 0.74, 0.78);
        cairo_move_to(cr, 8.0, y + 4.0);
        cairo_show_text(cr, g_strdup_printf("%.0f dB", db));
    }

    for (int grid = 0; grid <= 4; grid++) {
        double x = left_padding + ((double)grid / 4.0) * plot_width;
        double hz_offset = (((double)grid / 4.0) - 0.5) * (double)snapshot->sample_rate_hz;
        double absolute_hz = (double)snapshot->center_freq_hz + hz_offset;
        char label[32];

        cairo_set_source_rgba(cr, 0.30, 0.36, 0.40, 0.5);
        cairo_move_to(cr, x, top_padding);
        cairo_line_to(cr, x, top_padding + plot_height);
        cairo_stroke(cr);

        g_snprintf(label, sizeof(label), "%.3f MHz", absolute_hz / 1000000.0);
        cairo_set_source_rgb(cr, 0.70, 0.74, 0.78);
        cairo_move_to(cr, x - 28.0, height - 8.0);
        cairo_show_text(cr, label);
    }

    cairo_set_source_rgb(cr, 0.95, 0.67, 0.16);
    cairo_set_line_width(cr, 1.5);

    if (!snapshot->spectrum_ready) {
        cairo_set_source_rgb(cr, 0.75, 0.79, 0.83);
        cairo_move_to(cr, left_padding + 16.0, top_padding + (plot_height / 2.0));
        cairo_show_text(cr, "Start streaming to populate the spectrum.");
        return;
    }

    for (guint index = 0; index < RADIO_SPECTRUM_BINS; index++) {
        double clamped_db = snapshot->spectrum_db[index];
        double x = left_padding + ((double)index / (double)(RADIO_SPECTRUM_BINS - 1U)) * plot_width;
        double normalized;
        double y;

        if (clamped_db < min_db) {
            clamped_db = min_db;
        }
        if (clamped_db > max_db) {
            clamped_db = max_db;
        }

        normalized = (clamped_db - min_db) / (max_db - min_db);
        y = top_padding + (1.0 - normalized) * plot_height;

        if (index == 0) {
            cairo_move_to(cr, x, y);
        } else {
            cairo_line_to(cr, x, y);
        }
    }

    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.48, 0.82, 0.98, 0.8);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, left_padding + (plot_width / 2.0), top_padding);
    cairo_line_to(cr, left_padding + (plot_width / 2.0), top_padding + plot_height);
    cairo_stroke(cr);
}

/* Rebuild the textual statistics panel from the latest engine snapshot. */
static void update_stats(AppWidgets *widgets, const RadioEngineSnapshot *snapshot) {
    char *stats;
    double peak_db = -120.0;
    double peak_offset_hz = 0.0;

    if (snapshot->spectrum_ready) {
        for (guint index = 0; index < RADIO_SPECTRUM_BINS; index++) {
            if (snapshot->spectrum_db[index] > peak_db) {
                peak_db = snapshot->spectrum_db[index];
                peak_offset_hz = bin_frequency_hz(snapshot, index);
            }
        }
    }

    stats = g_strdup_printf(
        "Devices: %u\nCenter freq: %.3f MHz\nSample rate: %.3f MS/s\nDemod mode: %s\nAudio requested: %s\nAudio active: %s\nAudio out rate: %.1f kHz\nAudio buffer: %zu / %zu\nAudio samples: %llu\nAudio level: %.3f\nTotal IQ samples: %llu\nLast normalized sample: %.4f + %.4fi\nPeak bin: %+.1f kHz at %.1f dB",
        snapshot->device_count,
        snapshot->center_freq_hz / 1000000.0,
        snapshot->sample_rate_hz / 1000000.0,
        demod_mode_label(snapshot->demod_mode),
        snapshot->audio_requested ? "yes" : "no",
        snapshot->audio_active ? "yes" : "no",
        snapshot->audio_output_sample_rate_hz / 1000.0,
        snapshot->audio_buffer_fill,
        snapshot->audio_buffer_capacity,
        (unsigned long long)snapshot->audio_samples_generated,
        snapshot->audio_level,
        (unsigned long long)snapshot->total_samples,
        snapshot->last_i,
        snapshot->last_q,
        peak_offset_hz / 1000.0,
        peak_db);

    gtk_label_set_text(widgets->stats_label, stats);
    g_free(stats);
}

/* Periodic GTK timer that refreshes labels, button state, and the spectrum plot. */
static gboolean refresh_ui(gpointer user_data) {
    AppWidgets *widgets = user_data;
    radio_engine_get_snapshot(widgets->engine, &widgets->snapshot);

    update_stats(widgets, &widgets->snapshot);
    gtk_label_set_text(widgets->status_label, widgets->snapshot.status);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->start_button), !widgets->snapshot.running);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->stop_button), widgets->snapshot.running);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->sample_rate_dropdown), !widgets->snapshot.running);
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->demod_mode_dropdown), TRUE);
    gtk_button_set_label(widgets->audio_button, audio_button_label(widgets->snapshot.audio_requested));
    gtk_widget_queue_draw(GTK_WIDGET(widgets->spectrum_area));

    return G_SOURCE_CONTINUE;
}

/* Validate and apply the current UI control values to the radio engine. */
static gboolean apply_settings(AppWidgets *widgets, gboolean show_partial_message) {
    char message[160];
    uint32_t center_freq_hz = (uint32_t)gtk_spin_button_get_value_as_int(widgets->center_freq_spin);
    uint32_t sample_rate_hz = selected_sample_rate(widgets);
    RadioDemodMode demod_mode = selected_demod_mode(widgets);

    if (!radio_engine_set_center_freq(widgets->engine, center_freq_hz, message, sizeof(message))) {
        set_status_text(widgets, message);
        return FALSE;
    }

    if (!radio_engine_set_sample_rate(widgets->engine, sample_rate_hz, message, sizeof(message))) {
        if (show_partial_message) {
            set_status_text(widgets, message);
        }
        return FALSE;
    }

    if (!radio_engine_set_demod_mode(widgets->engine, demod_mode, message, sizeof(message))) {
        set_status_text(widgets, message);
        return FALSE;
    }

    if (show_partial_message) {
        set_status_text(widgets, "Settings applied.");
    }
    return TRUE;
}

/* GTK signal handler for the Apply button. */
static void on_apply_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *widgets = user_data;

    (void)button;
    apply_settings(widgets, TRUE);
}

/* GTK signal handler that applies settings and starts streaming. */
static void on_start_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *widgets = user_data;
    char message[160];

    (void)button;
    if (!apply_settings(widgets, FALSE)) {
        return;
    }

    if (!radio_engine_start(widgets->engine, 0, message, sizeof(message))) {
        set_status_text(widgets, message);
        return;
    }

    set_status_text(widgets, "Starting stream...");
}

/* GTK signal handler that stops the active RTL-SDR stream. */
static void on_stop_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *widgets = user_data;

    (void)button;
    radio_engine_stop(widgets->engine);
    set_status_text(widgets, "Stopping stream...");
}

/* GTK signal handler that toggles future audio playback intent. */
static void on_audio_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *widgets = user_data;
    char message[160];
    gboolean audio_requested = !widgets->snapshot.audio_requested;

    (void)button;
    if (!radio_engine_set_audio_requested(widgets->engine, audio_requested, message, sizeof(message))) {
        set_status_text(widgets, message);
        return;
    }

    gtk_button_set_label(widgets->audio_button, audio_button_label(audio_requested));
    set_status_text(widgets, audio_requested ? "Audio playback enabled." : "Audio playback disabled.");
}

/* Build the full control panel layout for the main application window. */
static GtkWidget *build_controls(AppWidgets *widgets) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *freq_label = gtk_label_new("Center frequency (Hz)");
    GtkWidget *rate_label = gtk_label_new("Sample rate");
    GtkWidget *demod_label = gtk_label_new("Demod mode");
    GtkWidget *spectrum_frame = gtk_frame_new("Spectrum analyzer");
    GtkWidget *stats_frame = gtk_frame_new("Radio state");
    GtkWidget *status_frame = gtk_frame_new("Status");
    GtkStringList *sample_rate_model = gtk_string_list_new(NULL);
    GtkStringList *demod_mode_model = gtk_string_list_new(NULL);

    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_label_set_xalign(GTK_LABEL(freq_label), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(rate_label), 0.0f);
    gtk_label_set_xalign(GTK_LABEL(demod_label), 0.0f);

    widgets->center_freq_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(150000.0, 1766000000.0, 1000.0));
    gtk_spin_button_set_digits(widgets->center_freq_spin, 0);
    gtk_spin_button_set_value(widgets->center_freq_spin, 100000000.0);

    for (size_t index = 0; index < G_N_ELEMENTS(sample_rate_options); index++) {
        gtk_string_list_append(sample_rate_model, sample_rate_options[index].label);
    }
    widgets->sample_rate_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sample_rate_model), NULL));
    gtk_drop_down_set_selected(widgets->sample_rate_dropdown, 5);
    g_object_unref(sample_rate_model);

    for (size_t index = 0; index < G_N_ELEMENTS(demod_mode_options); index++) {
        gtk_string_list_append(demod_mode_model, demod_mode_options[index].label);
    }
    widgets->demod_mode_dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(demod_mode_model), NULL));
    gtk_drop_down_set_selected(widgets->demod_mode_dropdown, 0);
    g_object_unref(demod_mode_model);

    gtk_grid_attach(GTK_GRID(grid), freq_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(widgets->center_freq_spin), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), rate_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(widgets->sample_rate_dropdown), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), demod_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(widgets->demod_mode_dropdown), 1, 2, 1, 1);

    widgets->apply_button = GTK_BUTTON(gtk_button_new_with_label("Apply settings"));
    widgets->start_button = GTK_BUTTON(gtk_button_new_with_label("Start"));
    widgets->stop_button = GTK_BUTTON(gtk_button_new_with_label("Stop"));
    widgets->audio_button = GTK_BUTTON(gtk_button_new_with_label(audio_button_label(FALSE)));
    gtk_widget_set_sensitive(GTK_WIDGET(widgets->stop_button), FALSE);

    g_signal_connect(widgets->apply_button, "clicked", G_CALLBACK(on_apply_clicked), widgets);
    g_signal_connect(widgets->start_button, "clicked", G_CALLBACK(on_start_clicked), widgets);
    g_signal_connect(widgets->stop_button, "clicked", G_CALLBACK(on_stop_clicked), widgets);
    g_signal_connect(widgets->audio_button, "clicked", G_CALLBACK(on_audio_clicked), widgets);

    gtk_box_append(GTK_BOX(button_row), GTK_WIDGET(widgets->apply_button));
    gtk_box_append(GTK_BOX(button_row), GTK_WIDGET(widgets->start_button));
    gtk_box_append(GTK_BOX(button_row), GTK_WIDGET(widgets->stop_button));
    gtk_box_append(GTK_BOX(button_row), GTK_WIDGET(widgets->audio_button));

    widgets->spectrum_area = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_size_request(GTK_WIDGET(widgets->spectrum_area), 760, 280);
    gtk_drawing_area_set_draw_func(widgets->spectrum_area, draw_spectrum, widgets, NULL);
    gtk_frame_set_child(GTK_FRAME(spectrum_frame), GTK_WIDGET(widgets->spectrum_area));

    widgets->stats_label = GTK_LABEL(gtk_label_new("Devices: 0"));
    gtk_label_set_xalign(widgets->stats_label, 0.0f);
    gtk_label_set_selectable(widgets->stats_label, TRUE);
    gtk_frame_set_child(GTK_FRAME(stats_frame), GTK_WIDGET(widgets->stats_label));

    widgets->status_label = GTK_LABEL(gtk_label_new("Ready."));
    gtk_label_set_xalign(widgets->status_label, 0.0f);
    gtk_frame_set_child(GTK_FRAME(status_frame), GTK_WIDGET(widgets->status_label));

    gtk_box_append(GTK_BOX(box), grid);
    gtk_box_append(GTK_BOX(box), button_row);
    gtk_box_append(GTK_BOX(box), spectrum_frame);
    gtk_box_append(GTK_BOX(box), stats_frame);
    gtk_box_append(GTK_BOX(box), status_frame);

    return box;
}

/* Application shutdown hook that removes timers and releases engine/UI state. */
static void on_app_shutdown(GApplication *application, gpointer user_data) {
    AppWidgets *widgets = user_data;

    (void)application;
    if (widgets->refresh_source_id != 0U) {
        g_source_remove(widgets->refresh_source_id);
        widgets->refresh_source_id = 0U;
    }
    radio_engine_free(widgets->engine);
    g_free(widgets);
}

/* GTK application activation hook that creates the window and starts UI polling. */
static void on_activate(GtkApplication *application, gpointer user_data) {
    AppWidgets *widgets = user_data;
    GtkWidget *window;
    GtkWidget *content;

    widgets->application = application;
    widgets->engine = radio_engine_new();
    if (!widgets->engine) {
        g_error("Failed to allocate the radio engine.");
    }

    window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "RTL-SDR GTK Control");
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 720);

    content = build_controls(widgets);
    gtk_window_set_child(GTK_WINDOW(window), content);
    widgets->refresh_source_id = g_timeout_add(100, refresh_ui, widgets);
    gtk_window_present(GTK_WINDOW(window));
}

/* Program entry point. Creates the GTK application and enters its event loop. */
int main(int argc, char *argv[]) {
    AppWidgets *widgets = g_new0(AppWidgets, 1);
    GtkApplication *application = gtk_application_new("com.cole.rtlsdrgtk", G_APPLICATION_DEFAULT_FLAGS);
    int status;

    g_signal_connect(application, "activate", G_CALLBACK(on_activate), widgets);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_app_shutdown), widgets);
    status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
