///////////////////////////////////////////////////////////////////////////////
// Free42 -- an HP-42S calculator simulator
// Copyright (C) 2004-2006  Thomas Okken
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
///////////////////////////////////////////////////////////////////////////////

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "shell.h"
#include "shell_main.h"
#include "shell_skin.h"
#include "shell_spool.h"
#include "core_main.h"
#include "core_display.h"
#include "icon.xpm"


/* These are global because the skin code uses them a lot */

GtkWidget *calc_widget;
bool allow_paint = false;

state_type state;
char free42dirname[FILENAMELEN];


/* PRINT_LINES is limited to an even lower value than in the Motif version.
 * It appears that GTK does not allow the SUM of a widget's height and its
 * container's height to exceed 32k, which means that as the print widget's
 * height approaches that magical number, the top-level window is forced to
 * become shorter and shorter. So, I set the limit at 30000 instead of
 * 32768; if we're reading a print-out file from Free42/Motif which has more
 * than 30000 pixels, we chop off the excess from the top.
 */
#define PRINT_LINES 30000
#define PRINT_BYTESPERLINE 36
#define PRINT_SIZE 1080000


static unsigned char *print_bitmap;
static int printout_top;
static int printout_bottom;
static bool quit_flag = false;
static int enqueued;


/* Private globals */

static FILE *print_txt = NULL;
static FILE *print_gif = NULL;
static char print_gif_name[FILENAMELEN];
static int gif_seq = -1;
static int gif_lines;

static int pype[2];

static GtkWidget *mainwindow;
static GtkWidget *printwindow;
static GtkWidget *print_widget;
static GdkGC *print_gc = NULL;
static GtkAdjustment *print_adj;
static char export_file_name[FILENAMELEN];
static FILE *export_file = NULL;
static FILE *import_file = NULL;
static GdkPixbuf *icon;

static int ckey = 0;
static int skey;
static bool mouse_key;
static guint16 active_keycode = 0;
static bool just_pressed_shift = false;
static guint timeout_id = 0;
static guint timeout3_id = 0;

static int keymap_length = 0;
static keymap_entry *keymap = NULL;

static guint reminder_id = 0;
static FILE *statefile = NULL;
static char statefilename[FILENAMELEN];
static char printfilename[FILENAMELEN];

static int ann_updown = 0;
static int ann_shift = 0;
static int ann_print = 0;
static int ann_run = 0;
static int ann_battery = 0;
static int ann_g = 0;
static int ann_rad = 0;


/* Private functions */

static void read_key_map(const char *keymapfilename);
static void init_shell_state(int4 version);
static int read_shell_state(int4 *version);
static int write_shell_state();
static void int_term_handler(int sig);
static gboolean gt_term_handler(GIOChannel *source, GIOCondition condition,
							    gpointer data);
static void quit();
static char *strclone(const char *s);
static bool is_file(const char *name);
static void show_message(char *title, char *message);
static void no_mwm_resize_borders(GtkWidget *window);
static void scroll_printout_to_bottom();
static void quitCB();
static void showPrintOutCB();
static void exportProgramCB();
static GtkWidget *make_file_select_dialog(
	const char *title, const char *pattern, bool save, GtkWidget *owner);
static void importProgramCB();
static void clearPrintOutCB();
static void preferencesCB();
static void appendSuffix(char *path, char *suffix);
static void copyCB();
static void pasteCB();
static void aboutCB();
static void delete_cb(GtkWidget *w, gpointer cd);
static void delete_print_cb(GtkWidget *w, gpointer cd);
static gboolean expose_cb(GtkWidget *w, GdkEventExpose *event, gpointer cd);
static gboolean print_expose_cb(GtkWidget *w, GdkEventExpose *event, gpointer cd);
static gboolean button_cb(GtkWidget *w, GdkEventButton *event, gpointer cd);
static gboolean key_cb(GtkWidget *w, GdkEventKey *event, gpointer cd);
static void enable_reminder();
static void disable_reminder();
static gboolean repeater(gpointer cd);
static gboolean timeout1(gpointer cd);
static gboolean timeout2(gpointer cd);
static gboolean timeout3(gpointer cd);
static void repaint_printout(int x, int y, int width, int height);
static gboolean reminder(gpointer cd);
static void txt_writer(const char *text, int length);
static void txt_newliner();
static void gif_seeker(int4 pos);
static void gif_writer(const char *text, int length);


#ifdef BCD_MATH
#define TITLE "Free42 Decimal"
#else
#define TITLE "Free42 Binary"
#endif

static GtkItemFactoryEntry entries[] = {
    { "/File", NULL, NULL, 0, "<Branch>" },
    { "/File/Show Print-Out", NULL, showPrintOutCB, 0, "<Item>" },
    { "/File/sep1", NULL, NULL, 0, "<Separator>" },
    { "/File/Import Program...", NULL, importProgramCB, 0, "<Item>" },
    { "/File/Export Program...", NULL, exportProgramCB, 0, "<Item>" },
    { "/File/sep2", NULL, NULL, 0, "<Separator>" },
    { "/File/Clear Print-Out", NULL, clearPrintOutCB, 0, "<Item>" },
    { "/File/Preferences...", NULL, preferencesCB, 0, "<Item>" },
    { "/File/sep3", NULL, NULL, 0, "<Separator>" },
    { "/File/Quit", "<CTRL>Q", quitCB, 0, "<Item>" },
    { "/Edit", NULL, NULL, 0, "<Branch>" },
    { "/Edit/Copy", "<CTRL>C", copyCB, 0, "<Item>" },
    { "/Edit/Paste", "<CTRL>V", pasteCB, 0, "<Item>" },
    { "/Skin", NULL, NULL, 0, "<Branch>" },
    { "/Help", NULL, NULL, 0, "<LastBranch>" },
    { "/Help/About Free42...", NULL, aboutCB, 0, "<Item>" }
};

static gint num_entries = sizeof(entries) / sizeof(entries[0]);

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    /*****************************************************/
    /***** Try to create the $HOME/.free42 directory *****/
    /*****************************************************/

    bool free42dir_exists = false;
    char *home = getenv("HOME");
    snprintf(free42dirname, FILENAMELEN, "%s/.free42", home);
    struct stat st;
    if (stat(free42dirname, &st) == -1 || !S_ISDIR(st.st_mode)) {
	mkdir(free42dirname, 0755);
	if (stat(free42dirname, &st) == 0 && S_ISDIR(st.st_mode)) {
	    char oldpath[FILENAMELEN], newpath[FILENAMELEN];
	    free42dir_exists = true;
	    /* Note that we only rename the .free42rc and .free42print files
	     * if we have just created the .free42 directory; if the user
	     * creates the .free42 directory manually, they also have to take
	     * responsibility for the old-style state and print files; I don't
	     * want to do any second-guessing here.
	     */
	    snprintf(oldpath, FILENAMELEN, "%s/.free42rc", home);
	    snprintf(newpath, FILENAMELEN, "%s/.free42/state", home);
	    rename(oldpath, newpath);
	    snprintf(oldpath, FILENAMELEN, "%s/.free42print", home);
	    snprintf(newpath, FILENAMELEN, "%s/.free42/print", home);
	    rename(oldpath, newpath);
	    snprintf(oldpath, FILENAMELEN, "%s/.free42keymap", home);
	    snprintf(newpath, FILENAMELEN, "%s/.free42/keymap", home);
	    rename(oldpath, newpath);
	}
    } else
	free42dir_exists = true;

    char keymapfilename[FILENAMELEN];
    if (free42dir_exists) {
	snprintf(statefilename, FILENAMELEN, "%s/.free42/state", home);
	snprintf(printfilename, FILENAMELEN, "%s/.free42/print", home);
	snprintf(keymapfilename, FILENAMELEN, "%s/.free42/keymap", home);
    } else {
	snprintf(statefilename, FILENAMELEN, "%s/.free42rc", home);
	snprintf(printfilename, FILENAMELEN, "%s/.free42print", home);
	snprintf(keymapfilename, FILENAMELEN, "%s/.free42keymap", home);
    }

    
    /****************************/
    /***** Read the key map *****/
    /****************************/

    read_key_map(keymapfilename);


    /***********************************************************/
    /***** Open the state file and read the shell settings *****/
    /***********************************************************/

    int4 version;
    int init_mode;

    statefile = fopen(statefilename, "r");
    if (statefile != NULL) {
	if (read_shell_state(&version))
	    init_mode = 1;
	else {
	    init_shell_state(-1);
	    init_mode = 2;
	}
    } else {
	init_shell_state(-1);
	init_mode = 0;
    }


    /*********************************/
    /***** Build the main window *****/
    /*********************************/

    icon = gdk_pixbuf_new_from_xpm_data((const char **) icon_xpm);

    mainwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon(GTK_WINDOW(mainwindow), icon);
    gtk_window_set_title(GTK_WINDOW(mainwindow), TITLE);
    gtk_window_set_resizable(GTK_WINDOW(mainwindow), FALSE);
    no_mwm_resize_borders(mainwindow);
    g_signal_connect(G_OBJECT(mainwindow), "delete_event",
		     G_CALLBACK(delete_cb), NULL);
    if (state.mainWindowKnown)
	gtk_window_move(GTK_WINDOW(mainwindow), state.mainWindowX,
					    state.mainWindowY);

    GtkAccelGroup *acc_grp = gtk_accel_group_new();
    GtkItemFactory *fac = gtk_item_factory_new(GTK_TYPE_MENU_BAR,
					       "<Main>", acc_grp);
    gtk_item_factory_create_items(fac, num_entries, entries, NULL);
    gtk_window_add_accel_group(GTK_WINDOW(mainwindow), acc_grp);
    GtkWidget *menubar = gtk_item_factory_get_widget(fac, "<Main>");

    // The "Skin" menu is dynamic; we don't populate any items in it here.
    // Instead, we attach a callback which scans the .free42 directory for
    // available skins; this callback is invoked when the menu is about to
    // be mapped.
    GList *children = gtk_container_get_children(GTK_CONTAINER(menubar));
    GtkMenuItem *skin_btn = (GtkMenuItem *) children->next->next->data;
    g_list_free(children);

    g_signal_connect(G_OBJECT(skin_btn), "activate",
		     G_CALLBACK(skin_menu_update), NULL);

    GtkWidget *box = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(mainwindow), box);
    gtk_box_pack_start(GTK_BOX(box), menubar, FALSE, FALSE, 0);


    /****************************************/
    /* Drawing area for the calculator skin */
    /****************************************/

    int win_width, win_height;
    skin_load(&win_width, &win_height);
    GtkWidget *w = gtk_drawing_area_new();
    gtk_widget_set_size_request(w, win_width, win_height);
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(w), "expose_event", G_CALLBACK(expose_cb), NULL);
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(button_cb), NULL);
    g_signal_connect(G_OBJECT(w), "button-release-event", G_CALLBACK(button_cb), NULL);
    GTK_WIDGET_SET_FLAGS(w, GTK_CAN_FOCUS);
    g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(key_cb), NULL);
    g_signal_connect(G_OBJECT(w), "key-release-event", G_CALLBACK(key_cb), NULL);
    calc_widget = w;


    /**************************************/
    /***** Build the print-out window *****/
    /**************************************/

    // In the Motif version, I create an XImage and read the bitmap data into
    // it; in the GTK version, that approach is not practical, since pixbuf
    // only comes in 24-bit and 32-bit flavors -- which would mean wasting
    // 25 megabytes for a 286x32768 pixbuf. So, instead, I use a 1 bpp buffer,
    // and simply create pixbufs on the fly whenever I have to repaint.
    print_bitmap = (unsigned char *) malloc(PRINT_SIZE);

    FILE *printfile = fopen(printfilename, "r");
    if (printfile != NULL) {
	int n = fread(&printout_bottom, 1, sizeof(int), printfile);
	if (n == sizeof(int)) {
	    if (printout_bottom > PRINT_LINES) {
		int excess = (printout_bottom - PRINT_LINES) * PRINT_BYTESPERLINE;
		fseek(printfile, excess, SEEK_CUR);
		printout_bottom = PRINT_LINES;
	    }
	    int bytes = printout_bottom * PRINT_BYTESPERLINE;
	    n = fread(print_bitmap, 1, bytes, printfile);
	    if (n != bytes)
		printout_bottom = 0;
	} else
	    printout_bottom = 0;
	fclose(printfile);
    } else
	printout_bottom = 0;
    printout_top = 0;
    for (int n = printout_bottom * PRINT_BYTESPERLINE; n < PRINT_SIZE; n++)
	print_bitmap[n] = 0;

    printwindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon(GTK_WINDOW(printwindow), icon);
    gtk_window_set_title(GTK_WINDOW(printwindow), "Free42 Print-Out");
    g_signal_connect(G_OBJECT(printwindow), "delete_event",
		     G_CALLBACK(delete_print_cb), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
    gtk_container_add(GTK_CONTAINER(printwindow), scroll);
    GtkWidget *view = gtk_viewport_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    print_widget = gtk_drawing_area_new();
    gtk_widget_set_size_request(print_widget, 286, printout_bottom);
    gtk_container_add(GTK_CONTAINER(view), print_widget);
    print_adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    g_signal_connect(G_OBJECT(print_widget), "expose_event", G_CALLBACK(print_expose_cb), NULL);

    gtk_widget_show(print_widget);
    gtk_widget_show(view);
    gtk_widget_show(scroll);

    GdkGeometry geom;
    geom.min_width = 286;
    geom.max_width = 286;
    geom.min_height = 1;
    geom.max_height = 32767;
    gtk_window_set_geometry_hints(GTK_WINDOW(printwindow), print_widget, &geom, GdkWindowHints(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));

    if (state.printWindowKnown)
	gtk_window_move(GTK_WINDOW(printwindow), state.printWindowX,
						 state.printWindowY);
    gint width, height;
    gtk_window_get_size(GTK_WINDOW(printwindow), &width, &height);
    gtk_window_resize(GTK_WINDOW(printwindow), width,
	    state.printWindowKnown ? state.printWindowHeight : 600);

    gtk_widget_realize(printwindow);
    scroll_printout_to_bottom();


    /*************************************************/
    /***** Show main window & start the emulator *****/
    /*************************************************/

    if (state.printWindowKnown && state.printWindowMapped)
	gtk_widget_show(printwindow);
    gtk_widget_show_all(mainwindow);
    gtk_widget_show(mainwindow);

    core_init(init_mode, version);
    if (statefile != NULL) {
	fclose(statefile);
	statefile = NULL;
    }

    if (pipe(pype) != 0)
	fprintf(stderr, "Could not create pipe for signal handler; not catching INT and TERM signals.\n");
    else {
	GIOChannel *channel = g_io_channel_unix_new(pype[0]);
	GError *err;
	g_io_channel_set_encoding(channel, NULL, &err);
	g_io_channel_set_flags(channel,     
	    (GIOFlags) (g_io_channel_get_flags(channel) | G_IO_FLAG_NONBLOCK), &err);
	g_io_add_watch(channel, G_IO_IN, gt_term_handler, NULL);
	struct sigaction act;
	act.sa_handler = int_term_handler;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGINT);
	sigaddset(&act.sa_mask, SIGTERM);
	act.sa_flags = 0;
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
    }
    gtk_main();
    return 0;
}

keymap_entry *parse_keymap_entry(char *line, int lineno) {
    char *p;
    static keymap_entry entry;

    p =  strchr(line, '#');
    if (p != NULL)
	*p = 0;
    p = strchr(line, '\n');
    if (p != NULL)
	*p = 0;
    p = strchr(line, '\r');
    if (p != NULL)
	*p = 0;

    p = strchr(line, ':');
    if (p != NULL) {
	char *val = p + 1;
	char *tok;
	bool ctrl = false;
	bool alt = false;
	bool shift = false;
	guint keyval = GDK_VoidSymbol;
	bool done = false;
	unsigned char macro[KEYMAP_MAX_MACRO_LENGTH];
	int macrolen = 0;

	/* Parse keysym */
	*p = 0;
	tok = strtok(line, " \t");
	while (tok != NULL) {
	    if (done) {
		fprintf(stderr, "Keymap, line %d: Excess tokens in key spec.\n", lineno);
		return NULL;
	    }
	    if (strcasecmp(tok, "ctrl") == 0)
		ctrl = true;
	    else if (strcasecmp(tok, "alt") == 0)
		alt = true;
	    else if (strcasecmp(tok, "shift") == 0)
		shift = true;
	    else {
		keyval = gdk_keyval_from_name(tok);
		if (keyval == GDK_VoidSymbol) {
		    fprintf(stderr, "Keymap, line %d: Unrecognized KeyName.\n", lineno);
		    return NULL;
		}
		done = true;
	    }
	    tok = strtok(NULL, " \t");
	}
	if (!done) {
	    fprintf(stderr, "Keymap, line %d: Unrecognized KeyName.\n", lineno);
	    return NULL;
	}

	/* Parse macro */
	tok = strtok(val, " \t");
	memset(macro, 0, KEYMAP_MAX_MACRO_LENGTH);
	while (tok != NULL) {
	    char *endptr;
	    long k = strtol(tok, &endptr, 10);
	    if (*endptr != 0 || k < 1 || k > 255) {
		fprintf(stderr, "Keymap, line %d: Bad value (%s) in macro.\n", lineno, tok);
		return NULL;
	    } else if (macrolen == KEYMAP_MAX_MACRO_LENGTH) {
		fprintf(stderr, "Keymap, line %d: Macro too long (max=%d).\n", lineno, KEYMAP_MAX_MACRO_LENGTH);
		return NULL;
	    } else
		macro[macrolen++] = k;
	    tok = strtok(NULL, " \t");
	}

	entry.ctrl = ctrl;
	entry.alt = alt;
	entry.shift = shift;
	entry.keyval = keyval;
	memcpy(entry.macro, macro, KEYMAP_MAX_MACRO_LENGTH);
	return &entry;
    } else
	return NULL;
}

static void read_key_map(const char *keymapfilename) {
    FILE *keymapfile = fopen(keymapfilename, "r");
    int kmcap = 0;
    char line[1024];
    int lineno = 0;

    if (keymapfile == NULL) {
	/* Try to create default keymap file */
	extern long keymap_filesize;
	extern const char keymap_filedata[];
	long n;

	keymapfile = fopen(keymapfilename, "wb");
	if (keymapfile == NULL)
	    return;
	n = fwrite(keymap_filedata, 1, keymap_filesize, keymapfile);
	if (n != keymap_filesize) {
	    int err = errno;
	    fprintf(stderr, "Error writing \"%s\": %s (%d)\n",
			    keymapfilename, strerror(err), err);
	}
	fclose(keymapfile);

	keymapfile = fopen(keymapfilename, "r");
	if (keymapfile == NULL)
	    return;
    }

    while (fgets(line, 1024, keymapfile) != NULL) {
	keymap_entry *entry = parse_keymap_entry(line, ++lineno);
	if (entry == NULL)
	    continue;
	/* Create new keymap entry */
	if (keymap_length == kmcap) {
	    kmcap += 50;
	    keymap = (keymap_entry *) realloc(keymap, kmcap * sizeof(keymap_entry));
	}
	memcpy(keymap + (keymap_length++), entry, sizeof(keymap_entry));
    }

    fclose(keymapfile);
}

static void init_shell_state(int4 version) {
    switch (version) {
	case -1:
	    state.extras = 0;
	    /* fall through */
	case 0:
	    state.printerToTxtFile = 0;
	    state.printerToGifFile = 0;
	    state.printerTxtFileName[0] = 0;
	    state.printerGifFileName[0] = 0;
	    state.printerGifMaxLength = 256;
	    /* fall through */
	case 1:
	    state.mainWindowKnown = 0;
	    state.printWindowKnown = 0;
	    /* fall through */
	case 2:
	    state.skinName[0] = 0;
	    /* fall through */
	case 3:
	    /* current version (SHELL_VERSION = 3),
	     * so nothing to do here since everything
	     * was initialized from the state file.
	     */
	    ;
    }
}

static int read_shell_state(int4 *ver) {
    int4 magic;
    int4 version;
    int4 state_size;
    int4 state_version;

    if (shell_read_saved_state(&magic, sizeof(int4)) != sizeof(int4))
	return 0;
    if (magic != FREE42_MAGIC)
	return 0;

    if (shell_read_saved_state(&version, sizeof(int4)) != sizeof(int4))
	return 0;
    if (version == 0) {
	/* State file version 0 does not contain shell state,
	 * only core state, so we just hard-init the shell.
	 */
	init_shell_state(-1);
	*ver = version;
	return 1;
    } else if (version > FREE42_VERSION)
	/* Unknown state file version */
	return 0;
    
    if (shell_read_saved_state(&state_size, sizeof(int4)) != sizeof(int4))
	return 0;
    if (shell_read_saved_state(&state_version, sizeof(int4)) != sizeof(int4))
	return 0;
    if (state_version < 0 || state_version > SHELL_VERSION)
	/* Unknown shell state version */
	return 0;
    if (shell_read_saved_state(&state, state_size) != state_size)
	return 0;

    init_shell_state(state_version);
    *ver = version;
    return 1;
}

static int write_shell_state() {
    int4 magic = FREE42_MAGIC;
    int4 version = FREE42_VERSION;
    int4 state_size = sizeof(state_type);
    int4 state_version = SHELL_VERSION;

    if (!shell_write_saved_state(&magic, sizeof(int4)))
	return 0;
    if (!shell_write_saved_state(&version, sizeof(int4)))
	return 0;
    if (!shell_write_saved_state(&state_size, sizeof(int4)))
	return 0;
    if (!shell_write_saved_state(&state_version, sizeof(int4)))
	return 0;
    if (!shell_write_saved_state(&state, sizeof(state_type)))
	return 0;

    return 1;
}

static void int_term_handler(int sig) {
    write(pype[1], "\n", 1);
}

static gboolean gt_term_handler(GIOChannel *source, GIOCondition condition,
							    gpointer data) {
    quit();
    return FALSE;
}

static void quit() {
    FILE *printfile;
    int n, length;

    printfile = fopen(printfilename, "w");
    if (printfile != NULL) {
	length = printout_bottom - printout_top;
	if (length < 0)
	    length += PRINT_LINES;
	n = fwrite(&length, 1, sizeof(int), printfile);
	if (n != sizeof(int))
	    goto failed;
	if (printout_bottom >= printout_top) {
	    n = fwrite(print_bitmap + PRINT_BYTESPERLINE * printout_top,
		       1, PRINT_BYTESPERLINE * length, printfile);
	    if (n != PRINT_BYTESPERLINE * length)
		goto failed;
	} else {
	    n = fwrite(print_bitmap + PRINT_BYTESPERLINE * printout_top,
		       1, PRINT_SIZE - PRINT_BYTESPERLINE * printout_top,
		       printfile);
	    if (n != PRINT_SIZE - PRINT_BYTESPERLINE * printout_top)
		goto failed;
	    n = fwrite(print_bitmap, 1,
		       PRINT_BYTESPERLINE * printout_bottom, printfile);
	    if (n != PRINT_BYTESPERLINE * printout_bottom)
		goto failed;
	}

	fclose(printfile);
	goto done;

	failed:
	fclose(printfile);
	remove(printfilename);

	done:
	;
    }

    if (print_txt != NULL)
	fclose(print_txt);

    if (print_gif != NULL) {
	shell_finish_gif(gif_seeker, gif_writer);
	fclose(print_gif);
    }

    gint x, y;
    gtk_window_get_position(GTK_WINDOW(mainwindow), &x, &y);
    state.mainWindowKnown = 1;
    state.mainWindowX = x;
    state.mainWindowY = y;

    if (state.printWindowKnown) {
	gtk_window_get_position(GTK_WINDOW(printwindow), &x, &y);
	state.printWindowX = x;
	state.printWindowY = y;
	gtk_window_get_size(GTK_WINDOW(printwindow), &x, &y);
	state.printWindowHeight = y;
    }

    statefile = fopen(statefilename, "w");
    if (statefile != NULL)
	write_shell_state();
    core_quit();
    if (statefile != NULL)
	fclose(statefile);

    shell_spool_exit();

    exit(0);
}

static char *strclone(const char *s) {
    char *s2 = (char *) malloc(strlen(s) + 1);
    strcpy(s2, s);
    return s2;
}

static bool is_file(const char *name) {
    struct stat st;
    if (stat(name, &st) == -1)
	return false;
    return S_ISREG(st.st_mode);
}

static void show_message(char *title, char *message) {
    GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(mainwindow),
					    GTK_DIALOG_MODAL,
					    GTK_MESSAGE_ERROR,
					    GTK_BUTTONS_OK,
					    message);
    gtk_window_set_title(GTK_WINDOW(msg), title);
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
}

static void no_mwm_resize_helper(GtkWidget *w, gpointer cd) {
    gdk_window_set_decorations(w->window, GdkWMDecoration(GDK_DECOR_ALL
				    | GDK_DECOR_RESIZEH | GDK_DECOR_MAXIMIZE));
    gdk_window_set_functions(w->window, GdkWMFunction(GDK_FUNC_ALL
				    | GDK_FUNC_RESIZE | GDK_FUNC_MAXIMIZE));
}

static void no_mwm_resize_borders(GtkWidget *window) {
    // gtk_window_set_resizable(w, FALSE) only sets the WM size hints, so that
    // the minimum and maximum sizes coincide with the actual size. While this
    // certainly has the desired effect of making the window non-resizable, it
    // does not deal with the way mwm visually distinguishes resizable and
    // non-resizable windows; the result is that, when running under mwm, you
    // have a window with resize borders and a maximize button, none of which
    // actually let you resize the window.
    // So, we use an additional GDK call to set the appropriate mwm properties.
    g_signal_connect(G_OBJECT(window), "realize",
		     G_CALLBACK(no_mwm_resize_helper), NULL);
}

static void scroll_printout_to_bottom() {
    gtk_adjustment_set_value(print_adj,
			     print_adj->upper - print_adj->page_size);
}

static void quitCB() {
    quit();
}

static void showPrintOutCB() {
    gtk_widget_show(printwindow);
    state.printWindowKnown = 1;
    state.printWindowMapped = 1;
}

static void exportProgramCB() {
    static GtkWidget *sel_dialog = NULL;
    static GtkTreeView *tree;
    static GtkTreeSelection *select;

    if (sel_dialog == NULL) {
	sel_dialog = gtk_dialog_new_with_buttons(
			    "Export Program",
			    GTK_WINDOW(mainwindow),
			    GTK_DIALOG_MODAL,
			    GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
			    GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			    NULL);
	gtk_window_set_resizable(GTK_WINDOW(sel_dialog), FALSE);
	no_mwm_resize_borders(sel_dialog);
	GtkWidget *container = gtk_bin_get_child(GTK_BIN(sel_dialog));
	GtkWidget *box = gtk_vbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER(container), box);

	GtkWidget *label = gtk_label_new("Select Programs to Export:");
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 10);

	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	tree = (GtkTreeView *) gtk_tree_view_new();
	select = gtk_tree_view_get_selection(tree);
	gtk_tree_selection_set_mode(select, GTK_SELECTION_MULTIPLE);
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	//gtk_cell_renderer_text_set_fixed_height_from_font((GtkCellRendererText *) renderer, 12);
	GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Foo", renderer, "text", 0, NULL);
	gtk_tree_view_append_column(tree, column);
	gtk_tree_view_set_headers_visible(tree, FALSE);
	gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(tree));
	gtk_widget_set_size_request(scroll, -1, 200);
	gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(scroll), FALSE, FALSE, 10);

	gtk_widget_show_all(GTK_WIDGET(sel_dialog));
    }

    char buf[10000];
    int count = core_list_programs(buf, 10000);
    char *p = buf;

    GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
    GtkTreeIter iter;
    while (count-- > 0) {
	gtk_list_store_append(model, &iter);
	gtk_list_store_set(model, &iter, 0, p, -1);
	p += strlen(p) + 1;
    }
    gtk_tree_view_set_model(tree, GTK_TREE_MODEL(model));

    // TODO: does this leak list-stores? Or is everything taken case of by the
    // GObject reference-counting stuff?

    bool cancelled = gtk_dialog_run(GTK_DIALOG(sel_dialog)) != GTK_RESPONSE_ACCEPT;
    gtk_widget_hide(sel_dialog);
    if (cancelled)
	return;

    count = gtk_tree_selection_count_selected_rows(select);
    if (count == 0)
	return;

    static GtkWidget *save_dialog = NULL;
    if (save_dialog == NULL)
	save_dialog = make_file_select_dialog("Export Program",
		"Program Files (*.raw)\0*.[Rr][Aa][Ww]\0All Files (*.*)\0*\0",
		true, mainwindow);

    char *filename = NULL;
    if (gtk_dialog_run(GTK_DIALOG(save_dialog)) == GTK_RESPONSE_ACCEPT)
	filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(save_dialog));
    gtk_widget_hide(GTK_WIDGET(save_dialog));
    if (filename == NULL)
	return;

    strcpy(export_file_name, filename);
    g_free(filename);
    if (strncmp(gtk_file_filter_get_name(
		    gtk_file_chooser_get_filter(
			GTK_FILE_CHOOSER(save_dialog))), "All", 3) != 0)
	appendSuffix(export_file_name, ".raw");

    if (is_file(export_file_name)) {
	char buf[1000];
	snprintf(buf, 1000, "Replace existing \"%s\"?", export_file_name);
	GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(mainwindow),
						GTK_DIALOG_MODAL,
						GTK_MESSAGE_QUESTION,
						GTK_BUTTONS_YES_NO,
						buf);
	gtk_window_set_title(GTK_WINDOW(msg), "Replace?");
	cancelled = gtk_dialog_run(GTK_DIALOG(msg)) != GTK_RESPONSE_YES;
	gtk_widget_destroy(msg);
	if (cancelled)
	    return;
    }

    export_file = fopen(export_file_name, "w");

    if (export_file == NULL) {
	char buf[1000];
	int err = errno;
	snprintf(buf, 1000, "Could not open \"%s\" for writing:\n%s (%d)",
		export_file_name, strerror(err), err);
	show_message("Message", buf);
    } else {
	int *p2 = (int *) malloc(count * sizeof(int));
	GList *rows = gtk_tree_selection_get_selected_rows(select, NULL);
	GList *item = rows;
	int i = 0;
	while (item != NULL) {
	    GtkTreePath *path = (GtkTreePath *) item->data;
	    char *pathstring = gtk_tree_path_to_string(path);
	    sscanf(pathstring, "%d", p2 + i);
	    item = item->next;
	    i++;
	}
	g_list_free(rows);
	core_export_programs(count, p2, NULL);
	free(p2);
	if (export_file != NULL) {
	    fclose(export_file);
	    export_file = NULL;
	}
    }
}

static GtkWidget *make_file_select_dialog(const char *title,
					  const char *pattern,
					  bool save,
					  GtkWidget *owner) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
			title,
			GTK_WINDOW(owner),
			save ? GTK_FILE_CHOOSER_ACTION_SAVE
			     : GTK_FILE_CHOOSER_ACTION_OPEN,
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			save ? GTK_STOCK_SAVE : GTK_STOCK_OPEN,
			GTK_RESPONSE_ACCEPT,
			NULL);
    const char *p = pattern;
    while (1) {
	const char *descr, *ext;
	int n = strlen(p);
	if (n == 0)
	    break;
	descr = p;
	p += n + 1;
	n = strlen(p);
	if (n == 0)
	    break;
	ext = p;
	p += n + 1;
	
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_add_pattern(filter, ext);
	gtk_file_filter_set_name(filter, descr);
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    }
    return dialog;
}

static void importProgramCB() {
    static GtkWidget *dialog = NULL;

    if (dialog == NULL)
	dialog = make_file_select_dialog("Import Program",
		"Program Files (*.raw)\0*.[Rr][Aa][Ww]\0All Files (*.*)\0*\0",
		false, mainwindow);

    bool cancelled = gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT;
    gtk_widget_hide(dialog);
    if (cancelled)
	return;
    
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename == NULL) {
	import_file = NULL;
	return;
    }

    char filenamebuf[FILENAMELEN];
    strncpy(filenamebuf, filename, FILENAMELEN);
    filenamebuf[FILENAMELEN - 1] = 0;
    g_free(filename);

    if (strncmp(gtk_file_filter_get_name(
		    gtk_file_chooser_get_filter(
			GTK_FILE_CHOOSER(dialog))), "All", 3) != 0)
	appendSuffix(filenamebuf, ".raw");

    import_file = fopen(filenamebuf, "r");
    if (import_file == NULL) {
	char buf[1000];
	int err = errno;
	snprintf(buf, 1000, "Could not open \"%s\" for reading:\n%s (%d)",
		    filenamebuf, strerror(err), err);
	show_message("Message", buf);
    } else {
	core_import_programs(NULL);
	redisplay();
	if (import_file != NULL) {
	    fclose(import_file);
	    import_file = NULL;
	}
    }
}

static void clearPrintOutCB() {
    printout_top = 0;
    printout_bottom = 0;
    gtk_widget_set_size_request(print_widget, 286, 1);

    if (print_gif != NULL) {
	shell_finish_gif(gif_seeker, gif_writer);
	fclose(print_gif);
	print_gif = NULL;
    }
}

struct browse_file_info {
    const char *title;
    const char *patterns;
    GtkWidget *textfield;
    browse_file_info(const char *t, const char *p, GtkWidget *tf)
			: title(t), patterns(p), textfield(tf) {}
};

static void browse_file(GtkButton *button, gpointer cd) {
    browse_file_info *info = (browse_file_info *) cd;
    GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(button));
    GtkWidget *save_dialog = make_file_select_dialog(
				    info->title, info->patterns, true, dialog);
    const char *filename = gtk_entry_get_text(GTK_ENTRY(info->textfield));
    if (filename[0] == '/')
	gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(save_dialog), filename);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(save_dialog), filename);

    if (gtk_dialog_run(GTK_DIALOG(save_dialog)) == GTK_RESPONSE_ACCEPT) {
	filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(save_dialog));

	char filenamebuf[FILENAMELEN];
	strncpy(filenamebuf, filename, FILENAMELEN);
	filenamebuf[FILENAMELEN - 1] = 0;
	const gchar *filtername =
	    gtk_file_filter_get_name(
			gtk_file_chooser_get_filter(
			    GTK_FILE_CHOOSER(save_dialog)));
	if (strncmp(filtername, "Text", 4) == 0)
	    appendSuffix(filenamebuf, ".txt");
	else if (strncmp(filtername, "GIF", 3) == 0)
	    appendSuffix(filenamebuf, ".gif");

	gtk_entry_set_text(GTK_ENTRY(info->textfield), filenamebuf);
    }
    gtk_widget_destroy(GTK_WIDGET(save_dialog));
}

static void preferencesCB() {
    static GtkWidget *dialog = NULL;
    static GtkWidget *singularmatrix;
    static GtkWidget *matrixoutofrange;
    static GtkWidget *printtotext;
    static GtkWidget *textpath;
    static GtkWidget *rawtext;
    static GtkWidget *printtogif;
    static GtkWidget *gifpath;
    static GtkWidget *gifheight;

    if (dialog == NULL) {
	dialog = gtk_dialog_new_with_buttons(
			    "Preferences",
			    GTK_WINDOW(mainwindow),
			    GTK_DIALOG_MODAL,
			    GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
			    GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
			    NULL);
	gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
	no_mwm_resize_borders(dialog);
	GtkWidget *container = gtk_bin_get_child(GTK_BIN(dialog));
	GtkWidget *table = gtk_table_new(6, 4, FALSE);
	gtk_container_add(GTK_CONTAINER(container), table);

	singularmatrix = gtk_check_button_new_with_label("Inverting or solving a singular matrix yields \"Singular Matrix\" error");
	gtk_table_attach(GTK_TABLE(table), singularmatrix, 0, 4, 0, 1, (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	matrixoutofrange = gtk_check_button_new_with_label("Overflows during matrix operations yield \"Out of Range\" error");
	gtk_table_attach(GTK_TABLE(table), matrixoutofrange, 0, 4, 1, 2, (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	printtotext = gtk_check_button_new_with_label("Print to text file:");
	gtk_table_attach(GTK_TABLE(table), printtotext, 0, 1, 2, 3, (GtkAttachOptions) (GTK_SHRINK | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	textpath = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), textpath, 1, 3, 2, 3, (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	GtkWidget *browse1 = gtk_button_new_with_label("Browse...");
	gtk_table_attach(GTK_TABLE(table), browse1, 3, 4, 2, 3, (GtkAttachOptions) (GTK_SHRINK | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	rawtext = gtk_check_button_new_with_label("Raw text");
	gtk_table_attach(GTK_TABLE(table), rawtext, 1, 3, 3, 4, (GtkAttachOptions) (GTK_SHRINK | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	printtogif = gtk_check_button_new_with_label("Print to GIF file:");
	gtk_table_attach(GTK_TABLE(table), printtogif, 0, 1, 4, 5, (GtkAttachOptions) (GTK_SHRINK | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	gifpath = gtk_entry_new();
	gtk_table_attach(GTK_TABLE(table), gifpath, 1, 3, 4, 5, (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	GtkWidget *browse2 = gtk_button_new_with_label("Browse...");
	gtk_table_attach(GTK_TABLE(table), browse2, 3, 4, 4, 5, (GtkAttachOptions) (GTK_SHRINK | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	GtkWidget *label = gtk_label_new("Maximum GIF height (pixels):");
	gtk_table_attach(GTK_TABLE(table), label, 1, 2, 5, 6, (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), (GtkAttachOptions) 0, 3, 3);
	gifheight = gtk_entry_new_with_max_length(5);
	gtk_table_attach(GTK_TABLE(table), gifheight, 2, 3, 5, 6, (GtkAttachOptions) (GTK_SHRINK), (GtkAttachOptions) 0, 3, 3);

	g_signal_connect(G_OBJECT(browse1), "clicked", G_CALLBACK(browse_file),
		(gpointer) new browse_file_info("Select Text File Name",
						"Text (*.txt)\0*.[Tt][Xx][Tt]\0All Files (*.*)\0*\0",
						textpath));
	g_signal_connect(G_OBJECT(browse2), "clicked", G_CALLBACK(browse_file),
		(gpointer) new browse_file_info("Select GIF File Name",
						"GIF (*.gif)\0*.[Gg][Ii][Ff]\0All Files (*.*)\0*\0",
						gifpath));
	
	gtk_widget_show_all(GTK_WIDGET(dialog));
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(singularmatrix), core_settings.matrix_singularmatrix);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(matrixoutofrange), core_settings.matrix_outofrange);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(printtotext), state.printerToTxtFile);
    gtk_entry_set_text(GTK_ENTRY(textpath), state.printerTxtFileName);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rawtext), core_settings.raw_text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(printtogif), state.printerToGifFile);
    gtk_entry_set_text(GTK_ENTRY(gifpath), state.printerGifFileName);
    char maxlen[6];
    snprintf(maxlen, 6, "%d", state.printerGifMaxLength);
	gtk_entry_set_text(GTK_ENTRY(gifheight), maxlen);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
	core_settings.matrix_singularmatrix = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(singularmatrix));
	core_settings.matrix_outofrange = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(matrixoutofrange));
	core_settings.raw_text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rawtext));

	state.printerToTxtFile = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printtotext));
	char *old = strclone(state.printerTxtFileName);
	const char *s = gtk_entry_get_text(GTK_ENTRY(textpath));
	strncpy(state.printerTxtFileName, s, FILENAMELEN);
	state.printerTxtFileName[FILENAMELEN - 1] = 0;
	appendSuffix(state.printerTxtFileName, ".txt");
	if (print_txt != NULL && (!state.printerToTxtFile || strcmp(state.printerTxtFileName, old) != 0)) {
	    fclose(print_txt);
	    print_txt = NULL;
	}
	free(old);

	state.printerToGifFile = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printtogif));
	old = strclone(state.printerGifFileName);
	s = gtk_entry_get_text(GTK_ENTRY(gifpath));
	strncpy(state.printerGifFileName, s, FILENAMELEN);
	state.printerGifFileName[FILENAMELEN - 1] = 0;
	appendSuffix(state.printerGifFileName, ".gif");
	if (print_gif != NULL && (!state.printerToGifFile || strcmp(state.printerGifFileName, old) != 0)) {
	    shell_finish_gif(gif_seeker, gif_writer);
	    fclose(print_gif);
	    print_gif = NULL;
	}
	free(old);

	s = gtk_entry_get_text(GTK_ENTRY(gifheight));
	if (sscanf(s, "%d", &state.printerGifMaxLength) == 1) {
	    if (state.printerGifMaxLength < 32)
		state.printerGifMaxLength = 32;
	    else if (state.printerGifMaxLength > 32767) state.printerGifMaxLength = 32767;
	} else
	    state.printerGifMaxLength = 256;
    }

    gtk_widget_hide(GTK_WIDGET(dialog));
}

static void appendSuffix(char *path, char *suffix) {
    int len = strlen(path);
    int slen = strlen(suffix);
    if (len == 0 || len >= FILENAMELEN - slen)
	return;
    if (len >= slen && strcasecmp(path + len - slen, suffix) != 0)
	strcat(path, suffix);
}

static void copyCB() {
    char buf[100];
    core_copy(buf, 100);
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, buf, -1);
    clip = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
    gtk_clipboard_set_text(clip, buf, -1);
}

static void paste2(GtkClipboard *clip, const gchar *text, gpointer cd) {
    if (text != NULL) {
	core_paste(text);
	redisplay();
    }
}

static void pasteCB() {
#ifdef GDK_WINDOWING_X11
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
#else
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
#endif
    gtk_clipboard_request_text(clip, paste2, NULL);
}

static void aboutCB() {
    static GtkWidget *about = NULL;

    if (about == NULL) {
	about = gtk_dialog_new_with_buttons(
			    "About Free42",
			    GTK_WINDOW(mainwindow),
			    GTK_DIALOG_MODAL,
			    GTK_STOCK_OK,
			    GTK_RESPONSE_ACCEPT,
			    NULL);
	gtk_window_set_resizable(GTK_WINDOW(about), FALSE);
	no_mwm_resize_borders(about);
	GtkWidget *container = gtk_bin_get_child(GTK_BIN(about));
	GtkWidget *box = gtk_hbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER(container), box);
	GtkWidget *image = gtk_image_new_from_pixbuf(icon);
	gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 10);
	GtkWidget *label = gtk_label_new("Free42 1.4.12\n(C) 2004-2006 Thomas Okken\nthomas_okken@yahoo.com\nhttp://home.planet.nl/~demun000/thomas_projects/free42/");
	gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 10);
	gtk_widget_show_all(GTK_WIDGET(about));
    }

    gtk_dialog_run(GTK_DIALOG(about));
    gtk_widget_hide(GTK_WIDGET(about));
}

static void delete_cb(GtkWidget *w, gpointer cd) {
    quit();
}

static void delete_print_cb(GtkWidget *w, gpointer cd) {
    state.printWindowMapped = 0;
    gtk_widget_hide(GTK_WIDGET(printwindow));
}

static gboolean expose_cb(GtkWidget *w, GdkEventExpose *event, gpointer cd) {
    allow_paint = true;
    skin_repaint();
    skin_repaint_display();
    skin_repaint_annunciator(1, ann_updown);
    skin_repaint_annunciator(2, ann_shift);
    skin_repaint_annunciator(3, ann_print);
    skin_repaint_annunciator(4, ann_run);
    skin_repaint_annunciator(5, ann_battery);
    skin_repaint_annunciator(6, ann_g);
    skin_repaint_annunciator(7, ann_rad);
    if (ckey != 0)
	skin_repaint_key(skey, 1);
    return TRUE;
}

static gboolean print_expose_cb(GtkWidget *w, GdkEventExpose *event, gpointer cd) {
    repaint_printout(event->area.x, event->area.y,
		     event->area.width, event->area.height);
    return TRUE;
}

static void shell_keydown() {
    int repeat, keep_running;
    if (skey == -1)
	skey = skin_find_skey(ckey);
    skin_repaint_key(skey, 1);
    if (timeout3_id != 0 && ckey != 28 /* KEY_SHIFT */) {
	g_source_remove(timeout3_id);
	timeout3_id = 0;
	core_timeout3(0);
    }

    if (ckey >= 38 && ckey <= 255) {
	/* Macro */
	unsigned char *macro = skin_find_macro(ckey);
	if (macro == NULL || *macro == 0) {
	    squeak();
	    return;
	}
	while (*macro != 0) {
	    keep_running = core_keydown(*macro++, &enqueued, &repeat);
	    if (*macro != 0 && !enqueued)
		core_keyup();
	}
	repeat = 0;
    } else
	keep_running = core_keydown(ckey, &enqueued, &repeat);

    if (quit_flag)
	quit();
    if (keep_running)
	enable_reminder();
    else {
	disable_reminder();
	if (timeout_id != 0)
	    g_source_remove(timeout_id);
	if (repeat != 0)
	    timeout_id = g_timeout_add(repeat == 1 ? 1000 : 500, repeater, NULL);
	else if (!enqueued)
	    timeout_id = g_timeout_add(250, timeout1, NULL);
    }
}

static void shell_keyup() {
    skin_repaint_key(skey, 0);
    ckey = 0;
    skey = -1;
    if (timeout_id != 0) {
	g_source_remove(timeout_id);
	timeout_id = 0;
    }
    if (!enqueued) {
	int keep_running = core_keyup();
	if (quit_flag)
	    quit();
	if (keep_running)
	    enable_reminder();
	else
	    disable_reminder();
    }
}

static gboolean button_cb(GtkWidget *w, GdkEventButton *event, gpointer cd) {
    if (event->type == GDK_BUTTON_PRESS) {
	if (ckey == 0) {
	    int x = (int) event->x;
	    int y = (int) event->y;
	    skin_find_key(x, y, &skey, &ckey);
	    if (ckey != 0) {
		shell_keydown();
		mouse_key = true;
	    }
	}
    } else if (event->type == GDK_BUTTON_RELEASE) {
	if (ckey != 0 && mouse_key)
	    shell_keyup();
    }
    return TRUE;
}

static gboolean key_cb(GtkWidget *w, GdkEventKey *event, gpointer cd) {
    if (event->type == GDK_KEY_PRESS) {
	if (event->hardware_keycode == active_keycode)
	    // Auto-repeat
	    return TRUE;
	if (ckey == 0 || !mouse_key) {
	    int i;
	    unsigned char *macro;

	    bool printable = event->length == 1 && event->string[0] >= 32 && event->string[0] <= 126;
	    just_pressed_shift = false;

	    if (event->keyval == GDK_Shift_L || event->keyval == GDK_Shift_R) {
		just_pressed_shift = true;
		return TRUE;
	    }
	    bool ctrl = (event->state & GDK_CONTROL_MASK) != 0;
	    bool alt = (event->state & GDK_MOD1_MASK) != 0;
	    bool shift = (event->state & (GDK_SHIFT_MASK | GDK_LOCK_MASK)) != 0;

	    if (ckey != 0) {
		shell_keyup();
		active_keycode = 0;
	    }

	    if (!ctrl && !alt) {
		char c = event->string[0];
		if (printable && core_alpha_menu()) {
		    if (c >= 'a' && c <= 'z')
			c = c + 'A' - 'a';
		    else if (c >= 'A' && c <= 'Z')
			c = c + 'a' - 'A';
		    ckey = 1024 + c;
		    skey = -1;
		    shell_keydown();
		    mouse_key = false;
		    active_keycode = event->hardware_keycode;
		    return TRUE;
		} else if (core_hex_menu() && ((c >= 'a' && c <= 'f')
					    || (c >= 'A' && c <= 'F'))) {
		    if (c >= 'a' && c <= 'f')
			ckey = c - 'a' + 1;
		    else
			ckey = c - 'A' + 1;
		    skey = -1;
		    shell_keydown();
		    mouse_key = false;
		    active_keycode = event->hardware_keycode;
		    return TRUE;
		}
	    }

	    macro = skin_keymap_lookup(event->keyval, printable, ctrl, alt, shift);
	    if (macro == NULL) {
		for (i = 0; i < keymap_length; i++) {
		    keymap_entry *entry = keymap + i;
		    if (ctrl == entry->ctrl
			    && alt == entry->alt
			    && (printable || shift == entry->shift)
			    && event->keyval == entry->keyval) {
			macro = entry->macro;
			break;
		    }
		}
	    }
	    if (macro != NULL) {
		int j;
		for (j = 0; j < KEYMAP_MAX_MACRO_LENGTH; j++)
		    if (macro[j] == 0)
			break;
		    else {
			if (ckey != 0)
			    shell_keyup();
			ckey = macro[j];
			skey = -1;
			shell_keydown();
		    }
		mouse_key = false;
		active_keycode = event->hardware_keycode;
	    }
	}
    } else if (event->type == GDK_KEY_RELEASE) {
	if (ckey == 0) {
	    if (just_pressed_shift && (event->keyval == GDK_Shift_L
				    || event->keyval == GDK_Shift_R)) {
		ckey = 28;
		skey = -1;
		shell_keydown();
		shell_keyup();
	    }
	} else {
	    if (!mouse_key && event->hardware_keycode == active_keycode) {
		shell_keyup();
		active_keycode = 0;
	    }
	}
    }
    return TRUE;
}

static void enable_reminder() {
    if (reminder_id == 0)
	reminder_id = g_idle_add(reminder, NULL);
    if (timeout_id != 0) {
	g_source_remove(timeout_id);
	timeout_id = 0;
    }
}

static void disable_reminder() {
    if (reminder_id != 0) {
	g_source_remove(reminder_id);
	reminder_id = 0;
    }
}

static gboolean repeater(gpointer cd) {
    int repeat = core_repeat();
    if (repeat != 0)
	timeout_id = g_timeout_add(repeat == 1 ? 200 : 100, repeater, NULL);
    else
	timeout_id = g_timeout_add(250, timeout1, NULL);
    return FALSE;
}

static gboolean timeout1(gpointer cd) {
    if (ckey != 0) {
	core_keytimeout1();
	timeout_id = g_timeout_add(1750, timeout2, NULL);
    } else
	timeout_id = 0;
    return FALSE;
}

static gboolean timeout2(gpointer cd) {
    if (ckey != 0)
	core_keytimeout2();
    timeout_id = 0;
    return FALSE;
}

static gboolean timeout3(gpointer cd) {
    core_timeout3(1);
    timeout3_id = 0;
    return FALSE;
}

static void repaint_printout(int x, int y, int width, int height) {
    GdkPixbuf *buf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE,
				    8, width, height);
    int d_bpl = gdk_pixbuf_get_rowstride(buf);
    guchar *d1 = gdk_pixbuf_get_pixels(buf);
    int length = printout_bottom - printout_top;
    if (length < 0)
	length += PRINT_LINES;

    for (int v = y; v < y + height; v++) {
	int v2 = printout_top + v;
	if (v2 >= PRINT_LINES)
	    v2 -= PRINT_LINES;
	int v3 = v2 * 36;
	guchar *dst = d1;
	for (int h = x; h < x + width; h++) {
	    unsigned char c;
	    if (h >= 286 || v >= length)
		c = 127;
	    else if ((print_bitmap[v3 + (h >> 3)] & (1 << (h & 7))) == 0)
		c = 255;
	    else
		c = 0;
	    *dst++ = c;
	    *dst++ = c;
	    *dst++ = c;
	}
	d1 += d_bpl;
    }

    gdk_draw_pixbuf(print_widget->window, NULL, buf,
		    0, 0, x, y, width, height,
		    GDK_RGB_DITHER_MAX, 0, 0);
    g_object_unref(G_OBJECT(buf));
}

static gboolean reminder(gpointer cd) {
    int dummy1, dummy2;
    int keep_running = core_keydown(0, &dummy1, &dummy2);
    if (quit_flag)
	quit();
    if (keep_running)
	return TRUE;
    else {
	reminder_id = 0;
	return FALSE;
    }
}

/* Callbacks used by shell_print() and shell_spool_txt() / shell_spool_gif() */

static void txt_writer(const char *text, int length) {
    int n;
    if (print_txt == NULL)
	return;
    n = fwrite(text, 1, length, print_txt);
    if (n != length) {
	char buf[1000];
	state.printerToTxtFile = 0;
	fclose(print_txt);
	print_txt = NULL;
	snprintf(buf, 1000, "Error while writing to \"%s\".\nPrinting to text file disabled", state.printerTxtFileName);
	show_message("Message", buf);
    }
}   
    
static void txt_newliner() {
    if (print_txt == NULL)
	return;
    fputc('\n', print_txt);
    fflush(print_txt);
}   
    
static void gif_seeker(int4 pos) {
    if (print_gif == NULL)
	return;
    if (fseek(print_gif, pos, SEEK_SET) == -1) {
	char buf[1000];
	state.printerToGifFile = 0;
	fclose(print_gif);
	print_gif = NULL;
	snprintf(buf, 1000, "Error while seeking \"%s\".\nPrinting to GIF file disabled", print_gif_name);
	show_message("Message", buf);
    }
}

static void gif_writer(const char *text, int length) {
    int n;
    if (print_gif == NULL)
	return;
    n = fwrite(text, 1, length, print_gif);
    if (n != length) {
	char buf[1000];
	state.printerToGifFile = 0;
	fclose(print_gif);
	print_gif = NULL;
	snprintf(buf, 1000, "Error while writing to \"%s\".\nPrinting to GIF file disabled", print_gif_name);
	show_message("Message", buf);
    }
}

void shell_blitter(const char *bits, int bytesperline, int x, int y,
				     int width, int height) {
    skin_display_blitter(bits, bytesperline, x, y, width, height);
    if (skey >= -7 && skey <= -2)
	skin_repaint_key(skey, 1);
}

void shell_beeper(int frequency, int duration) {
    gdk_beep();
}

void shell_annunciators(int updn, int shf, int prt, int run, int g, int rad) {
    if (updn != -1 && ann_updown != updn) {
	ann_updown = updn;
	skin_repaint_annunciator(1, ann_updown);
    }
    if (shf != -1 && ann_shift != shf) {
	ann_shift = shf;
	skin_repaint_annunciator(2, ann_shift);
    }
    if (prt != -1 && ann_print != prt) {
	ann_print = prt;
	skin_repaint_annunciator(3, ann_print);
    }
    if (run != -1 && ann_run != run) {
	ann_run = run;
	skin_repaint_annunciator(4, ann_run);
    }
    if (g != -1 && ann_g != g) {
	ann_g = g;
	skin_repaint_annunciator(6, ann_g);
    }
    if (rad != -1 && ann_rad != rad) {
	ann_rad = rad;
	skin_repaint_annunciator(7, ann_rad);
    }
}

int shell_wants_cpu() {
    return g_main_context_pending(NULL) ? 1 : 0;
}

void shell_delay(int duration) {
    gdk_display_flush(gdk_display_get_default());
    g_usleep(duration * 1000);
}

void shell_request_timeout3(int delay) {
    if (timeout3_id != 0)
	g_source_remove(timeout3_id);
    timeout3_id = g_timeout_add(delay, timeout3, NULL);
}

int4 shell_read_saved_state(void *buf, int4 bufsize) {
    if (statefile == NULL)
	return -1;
    else {
	int4 n = fread(buf, 1, bufsize, statefile);
	if (n != bufsize && ferror(statefile)) {
	    fclose(statefile);
	    statefile = NULL;
	    return -1;
	} else
	    return n;
    }
}

bool shell_write_saved_state(const void *buf, int4 nbytes) {
    if (statefile == NULL)
	return false;
    else {
	int4 n = fwrite(buf, 1, nbytes, statefile);
	if (n != nbytes) {
	    fclose(statefile);
	    remove(statefilename);
	    statefile = NULL;
	    return false;
	} else
	    return true;
    }
}

int shell_get_mem() { 
    FILE *meminfo = fopen("/proc/meminfo", "r");
    char line[1024];
    int bytes = 0;
    if (meminfo == NULL)
	return 0;
    while (fgets(line, 1024, meminfo) != NULL) {
	if (strncmp(line, "MemFree:", 8) == 0) {
	    int kbytes;
	    if (sscanf(line + 8, "%d", &kbytes) == 1)
		bytes = 1024 * kbytes;
	    break;
	}
    }
    fclose(meminfo);
    return bytes;
}

int shell_low_battery() {
         
    /* /proc/apm partial legend:
     * 
     * 1.16 1.2 0x03 0x01 0x03 0x09 9% -1 ?
     *               ^^^^ ^^^^
     *                 |    +-- Battery status (0 = full, 1 = low,
     *                 |                        2 = critical, 3 = charging)
     *                 +------- AC status (0 = offline, 1 = online)
     */

    FILE *apm = fopen("/proc/apm", "r");
    char line[1024];
    int lowbat = 0;
    int ac_stat, bat_stat;
    if (apm == NULL)
	goto done2;
    if (fgets(line, 1024, apm) == NULL)
	goto done1;
    if (sscanf(line, "%*s %*s %*s %x %x", &ac_stat, &bat_stat) == 2)
	lowbat = ac_stat != 1 && (bat_stat == 1 || bat_stat == 2);
    done1:
    fclose(apm);
    done2:
    if (lowbat != ann_battery) {
	ann_battery = lowbat;
	if (allow_paint)
	    skin_repaint_annunciator(5, ann_battery);
    }
    return lowbat;
}

void shell_powerdown() {
    /* We defer the actual shutdown so the emulator core can
     * return from core_keyup() or core_keydown() and isn't
     * asked to save its state while still in the middle of
     * executing the OFF instruction...
     */
    quit_flag = true;
}

double shell_random_seed() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((tv.tv_sec * 1000000L + tv.tv_usec) & 0xffffffffL) / 4294967296.0;
}

uint4 shell_milliseconds() {
    struct timeval tv;               
    gettimeofday(&tv, NULL);
    return (uint4) (tv.tv_sec * 1000L + tv.tv_usec / 1000);
}

struct print_growth_info {
    int y, height;
    print_growth_info(int yy, int hheight) : y(yy), height(hheight) {}
};

static gboolean print_widget_grew(GtkWidget *w, GdkEventConfigure *event,
								gpointer cd) {
    print_growth_info *info = (print_growth_info *) cd;
    scroll_printout_to_bottom();
    repaint_printout(0, info->y, 286, info->height);
    g_signal_handlers_disconnect_by_func(G_OBJECT(w), (gpointer) print_widget_grew, cd);
    delete info;
    return FALSE;
}

void shell_print(const char *text, int length,
		 const char *bits, int bytesperline,
		 int x, int y, int width, int height) {
    int xx, yy;
    int oldlength, newlength;

    for (yy = 0; yy < height; yy++) {
	int4 Y = (printout_bottom + 2 * yy) % PRINT_LINES;
	for (xx = 0; xx < 143; xx++) {
	    int bit, px, py;
	    if (xx < width) {
		char c = bits[(y + yy) * bytesperline + ((x + xx) >> 3)];
		bit = (c & (1 << ((x + xx) & 7))) != 0;
	    } else
		bit = 0;
	    for (px = xx * 2; px < (xx + 1) * 2; px++)
		for (py = Y; py < Y + 2; py++)
		    if (bit)
			print_bitmap[py * PRINT_BYTESPERLINE + (px >> 3)]
			    |= 1 << (px & 7);
		    else
			print_bitmap[py * PRINT_BYTESPERLINE + (px >> 3)]
			    &= ~(1 << (px & 7));
	}
    }

    oldlength = printout_bottom - printout_top;
    if (oldlength < 0)
	oldlength += PRINT_LINES;
    printout_bottom = (printout_bottom + 2 * height) % PRINT_LINES;
    newlength = oldlength + 2 * height;

    if (newlength >= PRINT_LINES) {
	int offset;
	printout_top = (printout_bottom + 2) % PRINT_LINES;
	newlength = PRINT_LINES - 2;
	if (newlength != oldlength)
	    gtk_widget_set_size_request(print_widget, 286, newlength);
	scroll_printout_to_bottom();
	offset = 2 * height - newlength + oldlength;
	if (print_gc == NULL)
	    print_gc = gdk_gc_new(print_widget->window);
	gdk_draw_drawable(print_widget->window, print_gc, print_widget->window,
			  0, offset, 0, 0, 286, oldlength - offset);
	repaint_printout(0, newlength - 2 * height, 286, 2 * height);
    } else {
	gtk_widget_set_size_request(print_widget, 286, newlength);
	// The resize request does not take effect immediately;
	// if I call scroll_printout_to_bottom() now, the scrolling will take
	// place *before* the resizing, leaving the scroll bar in the wrong
	// position.
	// I work around this by using a callback to finish the job.
	g_signal_connect(G_OBJECT(print_widget), "configure-event",
			 G_CALLBACK(print_widget_grew),
			 (gpointer) new print_growth_info(oldlength, 2 * height));
    }

    if (state.printerToTxtFile) {
	int err;
	char buf[1000];

	if (print_txt == NULL) {
	    print_txt = fopen(state.printerTxtFileName, "a");
	    if (print_txt == NULL) {
		err = errno;
		state.printerToTxtFile = 0;
		snprintf(buf, 1000, "Can't open \"%s\" for output:\n%s (%d)\nPrinting to text file disabled.", state.printerTxtFileName, strerror(err), err);
		show_message("Message", buf);
		goto done_print_txt;
	    }
	}

	shell_spool_txt(text, length, txt_writer, txt_newliner);
	done_print_txt:;
    }

    if (state.printerToGifFile) {
	int err;
	char buf[1000];

	if (print_gif != NULL
		&& gif_lines + height > state.printerGifMaxLength) {
	    shell_finish_gif(gif_seeker, gif_writer);
	    fclose(print_gif);
	    print_gif = NULL;
	}

	if (print_gif == NULL) {
	    while (1) {
		int len, p;

		gif_seq = (gif_seq + 1) % 10000;

		strcpy(print_gif_name, state.printerGifFileName);
		len = strlen(print_gif_name);

		/* Strip ".gif" extension, if present */
		if (len >= 4 &&
			strcasecmp(print_gif_name + len - 4, ".gif") == 0) {
		    len -= 4;
		    print_gif_name[len] = 0;
		}

		/* Strip ".[0-9]+", if present */
		p = len;
		while (p > 0 && print_gif_name[p] >= '0'
			     && print_gif_name[p] <= '9')
		    p--;
		if (p < len && p >= 0 && print_gif_name[p] == '.')
		    print_gif_name[p] = 0;

		/* Make sure we have enough space for the ".nnnn.gif" */
		p = 1000 - 10;
		print_gif_name[p] = 0;
		p = strlen(print_gif_name);
		snprintf(print_gif_name + p, 6, ".%04d", gif_seq);
		strcat(print_gif_name, ".gif");

		if (!is_file(print_gif_name))
		    break;
	    }
	    print_gif = fopen(print_gif_name, "w+");
	    if (print_gif == NULL) {
		err = errno;
		state.printerToGifFile = 0;
		snprintf(buf, 1000, "Can't open \"%s\" for output:\n%s (%d)\nPrinting to GIF file disabled.", print_gif_name, strerror(err), err);
		show_message("Message", buf);
		goto done_print_gif;
	    }
	    if (!shell_start_gif(gif_writer, state.printerGifMaxLength)) {
		state.printerToGifFile = 0;
		show_message("Message", "Not enough memory for the GIF encoder.\nPrinting to GIF file disabled.");
		goto done_print_gif;
	    }
	    gif_lines = 0;
	}

	shell_spool_gif(bits, bytesperline, x, y, width, height, gif_writer);
	gif_lines += height;
	done_print_gif:;
    }
}

int shell_write(const char *buf, int4 buflen) {
    int4 written;
    if (export_file == NULL)
	return 0;
    written = fwrite(buf, 1, buflen, export_file);
    if (written != buflen) {
	char buf[1000];
	fclose(export_file);
	export_file = NULL;
	snprintf(buf, 1000, "Writing \"%s\" failed.", export_file_name);
	show_message("Message", buf);
	return 0;
    } else
	return 1;
}

int shell_read(char *buf, int4 buflen) {
    int4 nread;
    if (import_file == NULL)
	return -1;
    nread = fread(buf, 1, buflen, import_file);
    if (nread != buflen && ferror(import_file)) {
	fclose(import_file);
	import_file = NULL;
	show_message("Message",
		"An error occurred; import was terminated prematurely.");
	return -1;
    } else
	return nread;
}

shell_bcd_table_struct *shell_get_bcd_table() {
    return NULL;
}

shell_bcd_table_struct *shell_put_bcd_table(shell_bcd_table_struct *bcdtab,
					    uint4 size) {
    return bcdtab;
}

void shell_release_bcd_table(shell_bcd_table_struct *bcdtab) {
    free(bcdtab);
}