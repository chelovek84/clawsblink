/*
 * ClawsBlink - Blinking tray icon for Claws Mail (GTK3)
 *
 *  - GtkStatusIcon in the system tray
 *  - Icon blinks while there are unread messages
 *  - Left click: show / hide main window
 *  - Right click: tray menu
 *  - Preferences page: Configuration > Preferences > Plugins > ClawsBlink
 *
 * No popups, no notifications.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include <math.h>
#include <glib.h>
#include <gtk/gtk.h>

#include "common/version.h"
#include "common/claws.h"
#include "common/plugin.h"
#include "common/hooks.h"
#include "common/utils.h"
#include "main.h"
#include "mainwindow.h"
#include "folder.h"
#include "account.h"
#include "compose.h"
#include "prefs_gtk.h"

#define PLUGIN_NAME      "ClawsBlink"
#define CONFIG_FILE      "clawsblinkrc"
#define CONFIG_GROUP     "ClawsBlink"
#define UPDATE_DELAY     150   /* ms, coalesce folder update storms */

#define BLINK_MIN        200   /* ms */
#define BLINK_MAX        2000  /* ms */
#define BLINK_DEFAULT    500   /* ms */

/* ---------- state ---------- */

static GtkStatusIcon *tray_icon = NULL;
static GtkWidget     *tray_menu = NULL;
static GtkWidget     *menu_show_item = NULL;

static GdkPixbuf *pix_nomail = NULL;
static GdkPixbuf *pix_unread = NULL;
static gint icon_size = 16;            /* updated by "size-changed" */

static gboolean has_unread = FALSE;    /* result of the last folder scan */
static guint blink_timer = 0;
static gboolean blink_on = TRUE;
static guint update_idle = 0;
static gulong focus_handler = 0;

static gulong hook_item_update = HOOK_NONE;
static gulong hook_folder_update = HOOK_NONE;
static gulong hook_iconified = HOOK_NONE;
static gulong hook_close = HOOK_NONE;

static struct {
	/* window */
	gboolean hide_on_minimize;
	gboolean close_to_tray;
	gboolean start_hidden;
	/* folders whose unread mail is ignored */
	gboolean ignore_trash;
	gboolean ignore_junk;
	gboolean ignore_special;   /* Drafts, Queue, Outbox */
	/* blinking */
	gint     blink_interval;   /* ms */
	gboolean no_blink_focused;
	gboolean disable_blinking;
} prefs = {
	.hide_on_minimize = TRUE,
	.close_to_tray    = FALSE,
	.start_hidden     = FALSE,
	.ignore_trash     = TRUE,
	.ignore_junk      = TRUE,
	.ignore_special   = TRUE,
	.blink_interval   = BLINK_DEFAULT,
	.no_blink_focused = TRUE,
	.disable_blinking = FALSE,
};

static void tray_update(void);
static void tray_apply(void);

/* ---------- config ---------- */

static gchar *config_path(void)
{
	return g_build_filename(get_rc_dir(), CONFIG_FILE, NULL);
}

static gboolean kf_get_bool(GKeyFile *kf, const gchar *key, gboolean def)
{
	GError *err = NULL;
	gboolean v = g_key_file_get_boolean(kf, CONFIG_GROUP, key, &err);

	if (err) {
		g_error_free(err);
		return def;
	}
	return v;
}

static gint kf_get_int(GKeyFile *kf, const gchar *key, gint def)
{
	GError *err = NULL;
	gint v = g_key_file_get_integer(kf, CONFIG_GROUP, key, &err);

	if (err) {
		g_error_free(err);
		return def;
	}
	return v;
}

static void config_load(void)
{
	GKeyFile *kf = g_key_file_new();
	gchar *path = config_path();

	if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		prefs.hide_on_minimize = kf_get_bool(kf, "hide_on_minimize", prefs.hide_on_minimize);
		prefs.close_to_tray    = kf_get_bool(kf, "close_to_tray", prefs.close_to_tray);
		prefs.start_hidden     = kf_get_bool(kf, "start_hidden", prefs.start_hidden);
		prefs.ignore_trash     = kf_get_bool(kf, "ignore_trash", prefs.ignore_trash);
		prefs.ignore_junk      = kf_get_bool(kf, "ignore_junk", prefs.ignore_junk);
		prefs.ignore_special   = kf_get_bool(kf, "ignore_drafts_queue_outbox", prefs.ignore_special);
		prefs.blink_interval   = kf_get_int(kf, "blink_interval", prefs.blink_interval);
		prefs.no_blink_focused = kf_get_bool(kf, "no_blink_when_focused", prefs.no_blink_focused);
		prefs.disable_blinking = kf_get_bool(kf, "disable_blinking", prefs.disable_blinking);
	}
	prefs.blink_interval = CLAMP(prefs.blink_interval, BLINK_MIN, BLINK_MAX);
	g_free(path);
	g_key_file_free(kf);
}

static void config_save(void)
{
	GKeyFile *kf = g_key_file_new();
	gchar *path = config_path();
	GError *err = NULL;

	g_key_file_set_boolean(kf, CONFIG_GROUP, "hide_on_minimize", prefs.hide_on_minimize);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "close_to_tray", prefs.close_to_tray);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "start_hidden", prefs.start_hidden);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "ignore_trash", prefs.ignore_trash);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "ignore_junk", prefs.ignore_junk);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "ignore_drafts_queue_outbox", prefs.ignore_special);
	g_key_file_set_integer(kf, CONFIG_GROUP, "blink_interval", prefs.blink_interval);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "no_blink_when_focused", prefs.no_blink_focused);
	g_key_file_set_boolean(kf, CONFIG_GROUP, "disable_blinking", prefs.disable_blinking);

	if (!g_key_file_save_to_file(kf, path, &err)) {
		g_warning(PLUGIN_NAME ": cannot save %s: %s", path, err->message);
		g_error_free(err);
	}
	g_free(path);
	g_key_file_free(kf);
}

/* ---------- main window helpers ---------- */

static gboolean mainwin_is_shown(MainWindow *mainwin)
{
	GtkWidget *w = mainwin->window;
	GdkWindow *gw;

	if (!gtk_widget_get_visible(w))
		return FALSE;
	gw = gtk_widget_get_window(w);
	if (gw && (gdk_window_get_state(gw) & GDK_WINDOW_STATE_ICONIFIED))
		return FALSE;
	return TRUE;
}

static gboolean mainwin_is_focused(void)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	return mainwin && mainwin_is_shown(mainwin) &&
	       gtk_window_is_active(GTK_WINDOW(mainwin->window));
}

static void mainwin_show(MainWindow *mainwin)
{
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(mainwin->window), FALSE);
	gtk_window_deiconify(GTK_WINDOW(mainwin->window));
	main_window_show(mainwin);
	gtk_window_present(GTK_WINDOW(mainwin->window));
}

static void mainwin_hide(MainWindow *mainwin)
{
	main_window_hide(mainwin);
}

static void mainwin_toggle(void)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	if (!mainwin)
		return;
	if (mainwin_is_focused())
		mainwin_hide(mainwin);
	else
		mainwin_show(mainwin);
}

static void activate_mainwin_action(const gchar *name)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();
	GtkAction *action;

	if (!mainwin || !mainwin->action_group)
		return;
	action = gtk_action_group_get_action(mainwin->action_group, name);
	if (action && gtk_action_is_sensitive(action))
		gtk_action_activate(action);
}

/* ---------- icons ---------- */

static void icons_free(void)
{
	g_clear_object(&pix_nomail);
	g_clear_object(&pix_unread);
}

/* Icon colours (0xRRGGBB). The idle blue matches the network tray icon. */
#define NOMAIL_COLOR   0x33A4FF   /* tray blue */
#define UNREAD_COLOR   0xFF6B4A   /* coral */

/* Split 0xRRGGBB into 0..1 channels; 'k' > 0 mixes towards white,
 * 'k' < 0 darkens. */
static void to_rgb(guint32 rgb, gdouble k, gdouble *r, gdouble *g, gdouble *b)
{
	*r = ((rgb >> 16) & 0xff) / 255.0;
	*g = ((rgb >> 8) & 0xff) / 255.0;
	*b = (rgb & 0xff) / 255.0;
	if (k > 0) {
		*r += (1.0 - *r) * k;
		*g += (1.0 - *g) * k;
		*b += (1.0 - *b) * k;
	} else {
		*r *= 1.0 + k;
		*g *= 1.0 + k;
		*b *= 1.0 + k;
	}
}

static void add_stop(cairo_pattern_t *pat, gdouble offset, guint32 rgb, gdouble k)
{
	gdouble r, g, b;

	to_rgb(rgb, k, &r, &g, &b);
	cairo_pattern_add_color_stop_rgb(pat, offset, r, g, b);
}

static void rounded_rect(cairo_t *cr, gdouble x, gdouble y,
			 gdouble w, gdouble h, gdouble r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
	cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2, G_PI);
	cairo_arc(cr, x + r,     y + r,     r, G_PI, 1.5 * G_PI);
	cairo_close_path(cr);
}

/* Round 'v' to the nearest integer with the same parity as 'size', so a
 * shape of that width centred in the icon lands exactly on pixel edges. */
static gdouble same_parity(gdouble v, gint size)
{
	gint n = (gint)floor(v + 0.5);

	if ((n - size) % 2)
		n += (v > n) ? 1 : -1;
	return n;
}

/*
 * Draw a round badge with a white envelope inside, in the style of
 * modern app tray icons: circle with 1px margin and a soft vertical
 * gradient, white envelope at 60% of the circle's width, exactly centred,
 * leaving a 1px margin between its corners and the circle edge.
 * Drawn at the exact size the tray asks for, so it is always sharp.
 */
static GdkPixbuf *draw_envelope(gint size, guint32 color)
{
	cairo_surface_t *s;
	cairo_t *cr;
	cairo_pattern_t *grad;
	GdkPixbuf *p;
	gdouble r, g, b;
	gdouble c = size / 2.0;               /* centre */
	gdouble d = size - 2.0;               /* circle diameter, 1px margin */
	gdouble rad = d / 2.0;
	gdouble ew = same_parity(d * 0.60, size); /* envelope width */
	gdouble eh = same_parity(d * 0.50, size); /* envelope height */
	gdouble ex = c - ew / 2.0;            /* exact centre: pixel aligned */
	gdouble ey;
	/* trim the height for a wider, "cinematic" shape (2px at 22px);
	 * an even trim keeps it exactly centred */
	gdouble trim = 2.0 * MAX(1.0, floor(size / 22.0 + 0.5));
	gdouble lw = MAX(1.0, size / 16.0);   /* flap line width */
	gdouble m = lw * 0.9;                 /* flap inset */

	eh -= trim;
	ey = c - eh / 2.0;

	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cr = cairo_create(s);

	/* badge */
	grad = cairo_pattern_create_linear(0, c - rad, 0, c + rad);
	add_stop(grad, 0.0, color, 0.12);
	add_stop(grad, 1.0, color, -0.12);
	cairo_arc(cr, c, c, rad, 0, 2 * G_PI);
	cairo_set_source(cr, grad);
	cairo_fill(cr);
	cairo_pattern_destroy(grad);

	/* white envelope */
	rounded_rect(cr, ex, ey, ew, eh, MAX(1.0, ew / 9.0));
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);

	/* flap, in a slightly darker badge colour */
	cairo_set_line_width(cr, lw);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, ex + m, ey + m);
	cairo_line_to(cr, c, ey + eh * 0.58);
	cairo_line_to(cr, ex + ew - m, ey + m);
	to_rgb(color, -0.12, &r, &g, &b);
	cairo_set_source_rgb(cr, r, g, b);
	cairo_stroke(cr);

	cairo_destroy(cr);
	p = gdk_pixbuf_get_from_surface(s, 0, 0, size, size);
	cairo_surface_destroy(s);
	return p;
}

static void icons_load(void)
{
	icons_free();
	pix_nomail = draw_envelope(icon_size, NOMAIL_COLOR);
	pix_unread = draw_envelope(icon_size, UNREAD_COLOR);
}

static void icon_set(GdkPixbuf *p)
{
	if (tray_icon && p)
		gtk_status_icon_set_from_pixbuf(tray_icon, p);
}

/* ---------- blinking ---------- */

static gboolean blink_cb(gpointer data)
{
	blink_on = !blink_on;
	/* alternate between the unread (coral) and idle (blue) icons */
	icon_set(blink_on ? pix_unread : pix_nomail);
	return G_SOURCE_CONTINUE;
}

static void blink_start(void)
{
	if (blink_timer)
		return;
	blink_on = TRUE;
	blink_timer = g_timeout_add(prefs.blink_interval, blink_cb, NULL);
}

static void blink_stop(void)
{
	if (blink_timer) {
		g_source_remove(blink_timer);
		blink_timer = 0;
	}
	blink_on = TRUE;
}

/* Show the right icon for the current state, starting or stopping the
 * blinking as the preferences require. Cheap: no folder scan. */
static void tray_apply(void)
{
	gboolean blink;

	if (!tray_icon)
		return;

	if (!has_unread) {
		blink_stop();
		icon_set(pix_nomail);
		return;
	}

	blink = !prefs.disable_blinking &&
		!(prefs.no_blink_focused && mainwin_is_focused());

	if (blink) {
		if (!blink_timer) {
			icon_set(pix_unread);
			blink_start();
		} else {
			icon_set(blink_on ? pix_unread : pix_nomail);
		}
	} else {
		blink_stop();
		icon_set(pix_unread);   /* steady coral */
	}
}

static void on_focus_changed(GObject *obj, GParamSpec *pspec, gpointer data)
{
	tray_apply();
}

/* ---------- unread counting ---------- */

typedef struct {
	guint new_msgs;
	guint unread_msgs;
	guint total_msgs;
} Counts;

static gboolean is_junk_folder(FolderItem *item)
{
	static const gchar *names[] = {
		"junk", "spam", "junk e-mail", "junk email", "bulk mail", NULL
	};
	gint i;

	if (!item->name)
		return FALSE;
	for (i = 0; names[i]; i++)
		if (!g_ascii_strcasecmp(item->name, names[i]))
			return TRUE;
	return FALSE;
}

static gboolean folder_is_ignored(FolderItem *item)
{
	if (prefs.ignore_trash && item->stype == F_TRASH)
		return TRUE;
	if (prefs.ignore_special &&
	    (item->stype == F_DRAFT || item->stype == F_QUEUE ||
	     item->stype == F_OUTBOX))
		return TRUE;
	if (prefs.ignore_junk && is_junk_folder(item))
		return TRUE;
	return FALSE;
}

static void count_item(FolderItem *item, gpointer data)
{
	Counts *c = data;

	if (!item || !item->folder || item->no_select)
		return;
	/* virtual folders would count messages twice */
	if (item->folder->klass && item->folder->klass->idstr &&
	    !strcmp(item->folder->klass->idstr, "vfolder"))
		return;
	if (folder_is_ignored(item))
		return;

	c->new_msgs    += item->new_msgs;
	c->unread_msgs += item->unread_msgs;
	c->total_msgs  += item->total_msgs;
}

static void tray_update(void)
{
	Counts c = { 0, 0, 0 };
	gchar *tip;

	if (!tray_icon)
		return;

	folder_func_to_all_folders(count_item, &c);
	has_unread = (c.unread_msgs > 0 || c.new_msgs > 0);
	tray_apply();

	tip = g_strdup_printf("Claws Mail\nNew: %u, Unread: %u, Total: %u",
			      c.new_msgs, c.unread_msgs, c.total_msgs);
	gtk_status_icon_set_tooltip_text(tray_icon, tip);
	g_free(tip);
}

static gboolean update_idle_cb(gpointer data)
{
	update_idle = 0;
	tray_update();
	return G_SOURCE_REMOVE;
}

static void schedule_update(void)
{
	if (!update_idle)
		update_idle = g_timeout_add(UPDATE_DELAY, update_idle_cb, NULL);
}

/* ---------- hooks ---------- */

static gboolean folder_item_update_hook(gpointer source, gpointer data)
{
	FolderItemUpdateData *ud = source;

	if (!ud || (ud->update_flags & (F_ITEM_UPDATE_MSGCNT |
					F_ITEM_UPDATE_ADDMSG |
					F_ITEM_UPDATE_REMOVEMSG |
					F_ITEM_UPDATE_CONTENT |
					F_ITEM_UPDATE_NAME)))
		schedule_update();
	return FALSE;
}

static gboolean folder_update_hook(gpointer source, gpointer data)
{
	schedule_update();
	return FALSE;
}

static gboolean hide_idle_cb(gpointer data)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	if (mainwin && prefs.hide_on_minimize)
		mainwin_hide(mainwin);
	return G_SOURCE_REMOVE;
}

static gboolean iconified_hook(gpointer source, gpointer data)
{
	if (prefs.hide_on_minimize)
		g_idle_add(hide_idle_cb, NULL);
	return FALSE;
}

static gboolean close_hook(gpointer source, gpointer data)
{
	gboolean *close_allowed = source;
	MainWindow *mainwin = mainwindow_get_mainwindow();

	if (prefs.close_to_tray && close_allowed && mainwin) {
		*close_allowed = FALSE;
		mainwin_hide(mainwin);
	}
	return FALSE;
}

static gboolean on_size_changed(GtkStatusIcon *icon, gint size, gpointer data)
{
	if (size <= 0 || size == icon_size)
		return TRUE;
	icon_size = size;
	icons_load();
	tray_apply();
	return TRUE;
}

/* ---------- tray callbacks / menu ---------- */

typedef struct {
	GtkWidget *item;
	gboolean  *opt;
} MenuCheck;

static MenuCheck menu_checks[3];
static gint n_menu_checks = 0;

static void on_activate(GtkStatusIcon *icon, gpointer data)
{
	mainwin_toggle();
}

static void on_menu_show(GtkMenuItem *item, gpointer data)
{
	mainwin_toggle();
}

static void on_menu_get(GtkMenuItem *item, gpointer data)
{
	activate_mainwin_action("Message/Receive/CurrentAccount");
}

static void on_menu_get_all(GtkMenuItem *item, gpointer data)
{
	activate_mainwin_action("Message/Receive/AllAccounts");
}

static void on_menu_send(GtkMenuItem *item, gpointer data)
{
	activate_mainwin_action("Message/SendQueue");
}

static void on_menu_compose(GtkMenuItem *item, gpointer data)
{
	PrefsAccount *ac = account_get_cur_account();

	if (!ac)
		ac = account_get_default();
	compose_new(ac, NULL, NULL);
}

static void on_menu_prefs(GtkMenuItem *item, gpointer data)
{
	prefs_gtk_open();
}

static void on_menu_toggle(GtkCheckMenuItem *item, gpointer data)
{
	gboolean *opt = data;

	*opt = gtk_check_menu_item_get_active(item);
	config_save();
	if (opt == &prefs.ignore_trash)
		tray_update();
}

static void on_menu_exit(GtkMenuItem *item, gpointer data)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	if (mainwin && !mainwin_is_shown(mainwin))
		mainwin_show(mainwin);   /* exit may ask questions */
	activate_mainwin_action("File/Exit");
}

static GtkWidget *menu_add(GtkWidget *menu, const gchar *label, GCallback cb)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);

	g_signal_connect(item, "activate", cb, NULL);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	return item;
}

static void menu_add_check(GtkWidget *menu, const gchar *label, gboolean *opt)
{
	GtkWidget *item = gtk_check_menu_item_new_with_mnemonic(label);

	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), *opt);
	g_signal_connect(item, "toggled", G_CALLBACK(on_menu_toggle), opt);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	if (n_menu_checks < (gint)G_N_ELEMENTS(menu_checks)) {
		menu_checks[n_menu_checks].item = item;
		menu_checks[n_menu_checks].opt = opt;
		n_menu_checks++;
	}
}

static void menu_add_sep(GtkWidget *menu)
{
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void menu_create(void)
{
	GtkWidget *m = gtk_menu_new();

	n_menu_checks = 0;
	menu_show_item = menu_add(m, "_Show Claws Mail", G_CALLBACK(on_menu_show));
	menu_add_sep(m);
	menu_add(m, "_Get new mail", G_CALLBACK(on_menu_get));
	menu_add(m, "Get from _all accounts", G_CALLBACK(on_menu_get_all));
	menu_add(m, "_Send queued messages", G_CALLBACK(on_menu_send));
	menu_add_sep(m);
	menu_add(m, "Compose _new message", G_CALLBACK(on_menu_compose));
	menu_add_sep(m);
	menu_add_check(m, "_Minimize to tray", &prefs.hide_on_minimize);
	menu_add_check(m, "_Close to tray", &prefs.close_to_tray);
	menu_add_check(m, "Ignore _Trash", &prefs.ignore_trash);
	menu_add(m, "_Preferences...", G_CALLBACK(on_menu_prefs));
	menu_add_sep(m);
	menu_add(m, "E_xit", G_CALLBACK(on_menu_exit));

	gtk_widget_show_all(m);
	tray_menu = g_object_ref_sink(m);
}

/* Bring the menu's check items in line with the current preferences
 * (they may have been changed on the preferences page). */
static void menu_sync(void)
{
	gint i;

	for (i = 0; i < n_menu_checks; i++) {
		GtkCheckMenuItem *ci = GTK_CHECK_MENU_ITEM(menu_checks[i].item);

		g_signal_handlers_block_by_func(ci, on_menu_toggle, menu_checks[i].opt);
		gtk_check_menu_item_set_active(ci, *menu_checks[i].opt);
		g_signal_handlers_unblock_by_func(ci, on_menu_toggle, menu_checks[i].opt);
	}
}

static void on_popup_menu(GtkStatusIcon *icon, guint button,
			  guint activate_time, gpointer data)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	if (!tray_menu)
		return;
	if (mainwin && menu_show_item)
		gtk_menu_item_set_label(GTK_MENU_ITEM(menu_show_item),
			mainwin_is_shown(mainwin) ? "_Hide Claws Mail"
						  : "_Show Claws Mail");
	menu_sync();

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_menu_popup(GTK_MENU(tray_menu), NULL, NULL,
		       gtk_status_icon_position_menu, icon,
		       button, activate_time);
	G_GNUC_END_IGNORE_DEPRECATIONS
}

/* ---------- preferences page ---------- */

typedef struct {
	PrefsPage page;

	GtkWidget *hide_on_minimize;
	GtkWidget *close_to_tray;
	GtkWidget *start_hidden;
	GtkWidget *ignore_trash;
	GtkWidget *ignore_junk;
	GtkWidget *ignore_special;
	GtkWidget *disable_blinking;
	GtkWidget *no_blink_focused;
	GtkWidget *blink_interval;
	GtkWidget *blink_box;
} ClawsBlinkPage;

static ClawsBlinkPage prefs_page;
static gchar *prefs_page_path[3];

static GtkWidget *page_frame(GtkWidget *parent, const gchar *title)
{
	GtkWidget *frame = gtk_frame_new(title);
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

	gtk_frame_set_label_align(GTK_FRAME(frame), 0.01, 0.5);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
	gtk_container_add(GTK_CONTAINER(frame), vbox);
	gtk_box_pack_start(GTK_BOX(parent), frame, FALSE, TRUE, 0);
	return vbox;
}

static GtkWidget *page_check(GtkWidget *box, const gchar *label,
			     gboolean active, const gchar *tip)
{
	GtkWidget *cb = gtk_check_button_new_with_mnemonic(label);

	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cb), active);
	if (tip)
		gtk_widget_set_tooltip_text(cb, tip);
	gtk_box_pack_start(GTK_BOX(box), cb, FALSE, FALSE, 0);
	return cb;
}

static void page_blink_sensitivity(GtkToggleButton *btn, gpointer data)
{
	ClawsBlinkPage *p = data;

	gtk_widget_set_sensitive(p->blink_box,
				 !gtk_toggle_button_get_active(btn));
}

static void page_create(PrefsPage *_page, GtkWindow *window, gpointer data)
{
	ClawsBlinkPage *p = (ClawsBlinkPage *)_page;
	GtkWidget *vbox, *box, *hbox, *label;

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

	/* Window */
	box = page_frame(vbox, "Window");
	p->hide_on_minimize = page_check(box, "_Minimize to tray",
		prefs.hide_on_minimize,
		"Hide Claws Mail in the tray when its window is minimized.");
	p->close_to_tray = page_check(box, "_Close to tray",
		prefs.close_to_tray,
		"The window's close button hides Claws Mail in the tray "
		"instead of quitting. Use Exit in the tray menu to quit.");
	p->start_hidden = page_check(box, "_Start hidden in tray",
		prefs.start_hidden,
		"Start Claws Mail with only the tray icon showing.");

	/* Folders */
	box = page_frame(vbox, "Unread mail in these folders does not count");
	p->ignore_trash = page_check(box, "_Trash", prefs.ignore_trash, NULL);
	p->ignore_junk = page_check(box, "_Junk / Spam",
		prefs.ignore_junk,
		"Folders named Junk, Spam, Junk E-mail or Bulk Mail.");
	p->ignore_special = page_check(box, "_Drafts, Queue and Outbox",
		prefs.ignore_special, NULL);

	/* Blinking */
	box = page_frame(vbox, "Blinking");
	p->disable_blinking = page_check(box, "D_isable blinking",
		prefs.disable_blinking,
		"Show a steady coral icon while there is unread mail.");

	p->blink_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_pack_start(GTK_BOX(box), p->blink_box, FALSE, FALSE, 0);

	p->no_blink_focused = page_check(p->blink_box,
		"Don't blink while the Claws Mail _window is focused",
		prefs.no_blink_focused,
		"Show a steady coral icon while you are working in "
		"Claws Mail; blink again when you switch away.");

	hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(p->blink_box), hbox, FALSE, FALSE, 0);
	label = gtk_label_new_with_mnemonic("Blink _speed");
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	p->blink_interval = gtk_spin_button_new_with_range(BLINK_MIN, BLINK_MAX, 50);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->blink_interval),
				  prefs.blink_interval);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), p->blink_interval);
	gtk_widget_set_tooltip_text(p->blink_interval,
		"Time between icon changes. Lower is faster. Default: 500 ms.");
	gtk_box_pack_start(GTK_BOX(hbox), p->blink_interval, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("ms"), FALSE, FALSE, 0);

	g_signal_connect(p->disable_blinking, "toggled",
			 G_CALLBACK(page_blink_sensitivity), p);
	page_blink_sensitivity(GTK_TOGGLE_BUTTON(p->disable_blinking), p);

	gtk_widget_show_all(vbox);
	p->page.widget = vbox;
}

static void page_destroy(PrefsPage *_page)
{
}

static gboolean get_check(GtkWidget *w)
{
	return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}

static void page_save(PrefsPage *_page)
{
	ClawsBlinkPage *p = (ClawsBlinkPage *)_page;
	gint old_interval = prefs.blink_interval;

	prefs.hide_on_minimize = get_check(p->hide_on_minimize);
	prefs.close_to_tray    = get_check(p->close_to_tray);
	prefs.start_hidden     = get_check(p->start_hidden);
	prefs.ignore_trash     = get_check(p->ignore_trash);
	prefs.ignore_junk      = get_check(p->ignore_junk);
	prefs.ignore_special   = get_check(p->ignore_special);
	prefs.disable_blinking = get_check(p->disable_blinking);
	prefs.no_blink_focused = get_check(p->no_blink_focused);
	prefs.blink_interval   = CLAMP(gtk_spin_button_get_value_as_int(
					GTK_SPIN_BUTTON(p->blink_interval)),
				       BLINK_MIN, BLINK_MAX);
	config_save();

	if (prefs.blink_interval != old_interval)
		blink_stop();   /* restarted at the new speed below */
	tray_update();
}

static void page_register(void)
{
	prefs_page_path[0] = (gchar *)g_dgettext("claws-mail", "Plugins");
	prefs_page_path[1] = PLUGIN_NAME;
	prefs_page_path[2] = NULL;

	memset(&prefs_page, 0, sizeof(prefs_page));
	prefs_page.page.path = prefs_page_path;
	prefs_page.page.weight = 40.0;
	prefs_page.page.create_widget = page_create;
	prefs_page.page.destroy_widget = page_destroy;
	prefs_page.page.save_page = page_save;
	prefs_gtk_register_page((PrefsPage *)&prefs_page);
}

/* ---------- plugin interface ---------- */

#define REGISTER(var, list, fn)						\
	do {								\
		var = hooks_register_hook(list, fn, NULL);		\
		if (var == HOOK_NONE) {					\
			*error = g_strdup("Failed to register hook " list); \
			plugin_done();					\
			return -1;					\
		}							\
	} while (0)

gint plugin_init(gchar **error)
{
	MainWindow *mainwin;

	if (!check_plugin_version(MAKE_NUMERIC_VERSION(4, 0, 0, 0),
				  VERSION_NUMERIC, PLUGIN_NAME, error))
		return -1;

	config_load();
	icons_load();

	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	tray_icon = gtk_status_icon_new();
	gtk_status_icon_set_title(tray_icon, "Claws Mail");
	gtk_status_icon_set_name(tray_icon, "claws-mail");
	G_GNUC_END_IGNORE_DEPRECATIONS
	g_signal_connect(tray_icon, "activate", G_CALLBACK(on_activate), NULL);
	g_signal_connect(tray_icon, "popup-menu", G_CALLBACK(on_popup_menu), NULL);
	g_signal_connect(tray_icon, "size-changed", G_CALLBACK(on_size_changed), NULL);
	menu_create();

	REGISTER(hook_item_update, FOLDER_ITEM_UPDATE_HOOKLIST, folder_item_update_hook);
	REGISTER(hook_folder_update, FOLDER_UPDATE_HOOKLIST, folder_update_hook);
	REGISTER(hook_iconified, MAIN_WINDOW_GOT_ICONIFIED, iconified_hook);
	REGISTER(hook_close, MAIN_WINDOW_CLOSE, close_hook);

	page_register();

	mainwin = mainwindow_get_mainwindow();
	if (mainwin)
		focus_handler = g_signal_connect(mainwin->window, "notify::is-active",
						 G_CALLBACK(on_focus_changed), NULL);

	if (prefs.start_hidden && claws_is_starting()) {
		main_set_show_at_startup(FALSE);
		if (mainwin && gtk_widget_get_visible(mainwin->window))
			mainwin_hide(mainwin);
	}

	tray_update();
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	gtk_status_icon_set_visible(tray_icon, TRUE);
	G_GNUC_END_IGNORE_DEPRECATIONS

	return 0;
}

#define UNREGISTER(var, list)					\
	do {							\
		if (var != HOOK_NONE) {				\
			hooks_unregister_hook(list, var);	\
			var = HOOK_NONE;			\
		}						\
	} while (0)

gboolean plugin_done(void)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	UNREGISTER(hook_item_update, FOLDER_ITEM_UPDATE_HOOKLIST);
	UNREGISTER(hook_folder_update, FOLDER_UPDATE_HOOKLIST);
	UNREGISTER(hook_iconified, MAIN_WINDOW_GOT_ICONIFIED);
	UNREGISTER(hook_close, MAIN_WINDOW_CLOSE);

	if (prefs_page.page.path) {
		prefs_gtk_unregister_page((PrefsPage *)&prefs_page);
		prefs_page.page.path = NULL;
	}

	if (focus_handler && mainwin && mainwin->window)
		g_signal_handler_disconnect(mainwin->window, focus_handler);
	focus_handler = 0;

	blink_stop();
	if (update_idle) {
		g_source_remove(update_idle);
		update_idle = 0;
	}

	/* Never leave the user without a way back to the main window. */
	if (!claws_is_exiting() && mainwin && !mainwin_is_shown(mainwin))
		mainwin_show(mainwin);

	if (tray_menu) {
		gtk_widget_destroy(tray_menu);
		g_object_unref(tray_menu);
		tray_menu = NULL;
		menu_show_item = NULL;
		n_menu_checks = 0;
	}
	g_clear_object(&tray_icon);
	icons_free();
	return TRUE;
}

const gchar *plugin_name(void)
{
	return PLUGIN_NAME;
}

const gchar *plugin_desc(void)
{
	return "Blinking system tray icon.\n\n"
	       "The icon blinks while there are unread messages. Left click "
	       "shows/hides the main window, right click opens a menu.\n\n"
	       "Options (minimize/close to tray, start hidden, folders to "
	       "ignore, blinking) are in Configuration > Preferences > "
	       "Plugins > " PLUGIN_NAME ".\n\n"
	       "Settings are stored in ~/.claws-mail/" CONFIG_FILE ".";
}

const gchar *plugin_type(void)
{
	return "GTK3";
}

const gchar *plugin_licence(void)
{
	return "GPL3+";
}

const gchar *plugin_version(void)
{
	return "1.1";
}

struct PluginFeature *plugin_provides(void)
{
	static struct PluginFeature features[] = {
		{ PLUGIN_NOTIFIER, "Blinking tray icon" },
		{ PLUGIN_NOTHING, NULL }
	};
	return features;
}
