/******************************************************************
 * Copyright (C) 2003 by SG Software.
 ******************************************************************/

/* FILE NAME   : player.c
 * PURPOSE     : SG MPFC. Main player functions implementation.
 * PROGRAMMER  : Sergey Galanov
 * LAST UPDATE : 8.08.2003
 * NOTE        : None.
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License 
 * as published by the Free Software Foundation; either version 2 
 * of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public 
 * License along with this program; if not, write to the Free 
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, 
 * MA 02111-1307, USA.
 */

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "types.h"
#include "button.h"
#include "cfg.h"
#include "choice_ctrl.h"
#include "colors.h"
#include "dlgbox.h"
#include "editbox.h"
#include "eqwnd.h"
#include "error.h"
#include "file_input.h"
#include "help_screen.h"
#include "history.h"
#include "key_bind.h"
#include "listbox.h"
#include "menu.h"
#include "player.h"
#include "plist.h"
#include "pmng.h"
#include "sat.h"
#include "util.h"

/* Files for player to play */
int player_num_files = 0;
char **player_files = NULL;

/* Play list */
plist_t *player_plist = NULL;

/* Command repeat value */
int player_repval = 0;

/* Search string and position */
char player_search_string[256] = "";

/* Message text */
char player_msg[80] = "";

/* Player thread ID */
pthread_t player_tid = 0;

/* Player termination flag */
bool player_end_thread = FALSE;

/* Timer thread ID */
pthread_t player_timer_tid = 0;

/* Timer termination flag */
bool player_end_timer = FALSE;
bool player_end_track = FALSE;

/* Current song playing time */
int player_cur_time = 0;

/* Player status */
int player_status = PLAYER_STATUS_STOPPED;

/* Current volume */
int player_volume = 0;

/* Has equalizer value changed */
bool player_eq_changed = FALSE;

/* Objects to load */
char player_objects[10][256];
int player_num_obj = 0;

/* Edit boxes history lists */
hist_list_t *player_hist_lists[PLAYER_NUM_HIST_LISTS];

/* Current input plugin */
in_plugin_t *player_inp = NULL;

/* Initialize player */
bool player_init( int argc, char *argv[] )
{
	int i;

	/* Initialize configuration */
	cfg_init();

	/* Parse command line */
	if (!player_parse_cmd_line(argc, argv))
		return FALSE;

	/* Create screen window */
	if (wnd_new_root() == NULL)
	{
		return FALSE;
	}
	wnd_register_handler(wnd_root, WND_MSG_DISPLAY, 
			player_display);
	wnd_register_handler(wnd_root, WND_MSG_KEYDOWN, 
			player_handle_key);
	wnd_register_handler(wnd_root, WND_MSG_MOUSE_LEFT_CLICK,
			player_handle_mouse_click);

	/* Initialize key bindings */
	kbind_init();

	/* Initialize colors */
	col_init();
	
	/* Initialize plugin manager */
	pmng_init();

	/* Initialize song adder thread */
	sat_init();

	/* Create a play list and add files to it */
	player_plist = plist_new(3, wnd_root->m_height - 6);
	if (player_plist == NULL)
	{
		return FALSE;
	}
	for ( i = 0; i < player_num_obj; i ++ )
		plist_add_obj(player_plist, player_objects[i]);
	for ( i = 0; i < player_num_files; i ++ )
		plist_add(player_plist, player_files[i]);

	/* Load saved play list if files list is empty */
	if (!player_num_files && !player_num_obj)
		plist_add(player_plist, "~/mpfc.m3u");

	/* Initialize history lists */
	for ( i = 0; i < PLAYER_NUM_HIST_LISTS; i ++ )
		player_hist_lists[i] = hist_list_new();

	/* Initialize playing thread */
	pthread_create(&player_tid, NULL, player_thread, NULL);

	/* Get volume */
	player_volume = outp_get_volume(pmng_cur_out);

	/* Initialize equalizer */
	player_eq_changed = FALSE;

	/* Exit */
	return TRUE;
} /* End of 'player_init' function */

/* Unitialize player */
void player_deinit( void )
{
	int i;
	
	/* End playing thread */
	sat_free();
	player_end_track = TRUE;
	player_end_thread = TRUE;
	pthread_join(player_tid, NULL);
	player_end_thread = FALSE;
	player_tid = 0;
	
	/* Uninitialize plugin manager */
	pmng_free();

	/* Uninitialize key bindings */
	kbind_free();

	/* Uninitialize history lists */
	for ( i = 0; i < PLAYER_NUM_HIST_LISTS; i ++ )
		hist_list_free(player_hist_lists[i]);
	
	/* Destroy screen window */
	wnd_destroy(wnd_root);
	
	/* Destroy all objects */
	if (player_plist != NULL)
	{
		/* Save play list */
		if (cfg_get_var_int(cfg_list, "save_playlist_on_exit"))
			plist_save(player_plist, "~/mpfc.m3u");
		
		plist_free(player_plist);
		player_plist = NULL;
	}

	/* Uninitialize configuration manager */
	cfg_free();
} /* End of 'player_deinit' function */

/* Run player */
bool player_run( void )
{
	/* Run window message loop */
	wnd_run(wnd_root);
	return TRUE;
} /* End of 'player_run' function */

/* Parse program command line */
bool player_parse_cmd_line( int argc, char *argv[] )
{
	int i;

	/* Get options */
	for ( i = 1; i < argc && argv[i][0] == '-'; i ++ )
	{
		char name[80], val[256];
		char *str = argv[i];
		int name_start, name_end;
		
		/* Get variable name start */
		for ( name_start = 0; str[name_start] == '-'; name_start ++ );
		if (name_start >= strlen(str))
			continue;

		/* Get variable name end */
		for ( name_end = name_start; 
				str[name_end] && str[name_end] != '=' && str[name_end] != ' '; 
				name_end ++ );
		name_end --;

		/* Extract variable name */
		memcpy(name, &str[name_start], name_end - name_start + 1);
		name[name_end - name_start + 1] = 0;

		/* Load object */
		if (!strcmp(name, "obj"))
		{
			if (i < argc - 1 && player_num_obj < 10)
				strcpy(player_objects[player_num_obj ++], argv[++i]);
			continue;
		}

		/* We have no value - assume it "1" */
		if (name_end == strlen(str) - 1)
		{
			strcpy(val, "1");
		}
		/* Extract value */
		else
		{
			strcpy(val, &str[name_end + 2]);
		}
		
		/* Set respective variable */
		cfg_set_var(cfg_list, name, val);
	}

	/* Get file names from command line */
	player_num_files = argc - i;
	player_files = &argv[i];
	return TRUE;
} /* End of 'player_parse_cmd_line' function */

/* Handle key function */
void player_handle_key( wnd_t *wnd, dword data )
{
	kbind_key2buf((int)data);
} /* End of 'player_handle_key' function */

/* Handle mouse left button click */
void player_handle_mouse_click( wnd_t *wnd, dword data )
{
	strcpy(player_msg, "Mouse button clicked");
} /* End of 'player_handle_mouse_click' function */

/* Display player function */
void player_display( wnd_t *wnd, dword data )
{
	int i;
	song_t *s = NULL;
	
	/* Clear the screen */
	wnd_clear(wnd, FALSE);
	
	/* Display head */
	wnd_move(wnd, 0, 0);
	if (player_plist->m_cur_song == -1)
	{
		col_set_color(wnd, COL_EL_ABOUT);
		wnd_printf(wnd, "SG Software Media Player For Console\n"
				"version 0.3\n");
		col_set_color(wnd, COL_EL_DEFAULT);
	}
	else
	{
		char title[80];
		
		s = player_plist->m_list[player_plist->m_cur_song];
		if (strlen(s->m_title) >= wnd->m_width - 1)
		{
			memcpy(title, s->m_title, wnd->m_width - 4);
			strcpy(&title[wnd->m_width - 4], "...");
		}
		else
			strcpy(title, s->m_title);
		col_set_color(wnd, COL_EL_CUR_TITLE);
		wnd_printf(wnd, "%s\n", title);
		col_set_color(wnd, COL_EL_CUR_TIME);
		wnd_printf(wnd, "%i:%02i/%i:%02i\n", 
				player_cur_time / 60, player_cur_time % 60,
				s->m_len / 60, s->m_len % 60);
		col_set_color(wnd, COL_EL_DEFAULT);
	}

	/* Display play modes */
	col_set_color(wnd, COL_EL_PLAY_MODES);
	if (cfg_get_var_int(cfg_list, "shuffle_play"))
	{
		wnd_move(wnd, wnd->m_width - 13, 1);
		wnd_printf(wnd, "Shuffle");
	}
	if (cfg_get_var_int(cfg_list, "loop_play"))
	{
		wnd_move(wnd, wnd->m_width - 5, 1);
		wnd_printf(wnd, "Loop");
	}
	col_set_color(wnd, COL_EL_DEFAULT);

	/* Display different slidebars */
	player_display_slider(wnd, 0, 2, wnd->m_width - 24, 
			player_cur_time, (s == NULL) ? 0 : s->m_len);
	player_display_slider(wnd, wnd->m_width - 22, 2, 20, 
			player_volume, 100.);
	
	/* Display play list */
	plist_display(player_plist, wnd);

	/* Print message */
	col_set_color(wnd, COL_EL_STATUS);
	wnd_move(wnd, 0, wnd->m_height - 2);
	wnd_printf(wnd, "%s\n", player_msg);
	col_set_color(wnd, COL_EL_DEFAULT);

	/* Hide cursor */
	wnd_move(wnd, wnd->m_width - 1, wnd->m_height - 1);
} /* End of 'player_display' function */

/* Key handler function for command repeat value edit box */
void player_repval_handle_key( wnd_t *wnd, dword data )
{
	editbox_t *box = (editbox_t *)wnd;
	int key = (int)data;
	
	/* Remember last pressed key */
	box->m_last_key = key;

	/* Add character to text if it is a digit */
	if (key >= '0' && key <= '9')
		ebox_add(box, key);
	/* Move cursor */
	else if (key == KEY_RIGHT)
		ebox_move(box, TRUE, 1);
	else if (key == KEY_LEFT)
		ebox_move(box, TRUE, -1);
	else if (key == KEY_HOME)
		ebox_move(box, FALSE, 0);
	else if (key == KEY_END)
		ebox_move(box, FALSE, box->m_len);
	/* Delete character */
	else if (key == KEY_BACKSPACE)
	{
		if (box->m_cursor)
			ebox_del(box, box->m_cursor - 1);
		else
			wnd_send_msg(wnd, WND_MSG_CLOSE, 0);
	}
	else if (key == KEY_DC)
		ebox_del(box, box->m_cursor);
	/* Exit */
	else 
		wnd_send_msg(wnd, WND_MSG_CLOSE, 0);
} /* End of 'player_repval_handle_key' function */

/* Seek song */
void player_seek( int sec, bool rel )
{
	song_t *s;
	int new_time;
	
	if (player_plist->m_cur_song == -1)
		return;

	s = player_plist->m_list[player_plist->m_cur_song];

	new_time = (rel ? (player_cur_time + sec) : sec);
	if (new_time < 0)
		new_time = 0;
	else if (new_time > s->m_len)
		new_time = s->m_len;

	inp_seek(s->m_inp, new_time);
	player_cur_time = new_time;
	wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
} /* End of 'player_seek' function */

/* Play song */
void player_play( void )
{
	song_t *s;
	
	/* Check that we have anything to play */
	if (player_plist->m_cur_song == -1 || 
			(s = player_plist->m_list[player_plist->m_cur_song]) == NULL)
		return;

	/* End current playing */
	player_end_play();

	/* Start new playing thread */
	player_status = PLAYER_STATUS_PLAYING;
} /* End of 'player_play' function */

/* End playing song */
void player_end_play( void )
{
	player_end_track = TRUE;
	player_status = PLAYER_STATUS_STOPPED;
} /* End of 'player_end_play' function */

/* Player thread function */
void *player_thread( void *arg )
{
	bool no_outp = FALSE;
	
	/* Main loop */
	while (!player_end_thread)
	{
		song_t *s;
		int ch, freq;
		dword fmt;
		song_info_t si;

		/* Skip to next iteration if there is nothing to play */
		if (player_plist->m_cur_song < 0 || 
				player_status == PLAYER_STATUS_STOPPED)
		{
			util_delay(0, 100000L);
			continue;
		}

		/* Play track */
		s = player_plist->m_list[player_plist->m_cur_song];
		player_status = PLAYER_STATUS_PLAYING;
		player_end_track = FALSE;
	
		/* Get song length and information at first */
		if (cfg_get_var_int(cfg_list, "update_song_len_on_play"))
			s->m_len = inp_get_len(s->m_inp, s->m_file_name);
		song_update_info(s);

		/* Start playing */
		if (!inp_start(s->m_inp, s->m_file_name))
		{
			player_next_track();
			error_set_code(ERROR_UNKNOWN_FILE_TYPE);
			strcpy(player_msg, error_text);
			wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
			continue;
		}
		no_outp = inp_get_flags(s->m_inp) & INP_NO_OUTP;

		/* Start output plugin */
		if (!no_outp && (pmng_cur_out == NULL || 
				(!cfg_get_var_int(cfg_list, "silent_mode") && 
					!outp_start(pmng_cur_out))))
		{
			strcpy(player_msg, _("Unable to initialize output plugin"));
//			wnd_send_msg(wnd_root, WND_MSG_USER, PLAYER_MSG_END_TRACK);
			inp_end(s->m_inp);
			player_status = PLAYER_STATUS_STOPPED;
			wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
			continue;
		}

		/* Set audio parameters */
		if (!no_outp)
		{
			inp_get_audio_params(s->m_inp, &ch, &freq, &fmt);
			outp_set_channels(pmng_cur_out, 2);
			outp_set_freq(pmng_cur_out, freq);
			outp_set_fmt(pmng_cur_out, fmt);
		}

		/* Save current input plugin */
		player_inp = s->m_inp;

		/* Set equalizer */
		inp_set_eq(s->m_inp);

		/* Start timer thread */
		pthread_create(&player_timer_tid, NULL, player_timer_func, 0);
		wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
	
		/* Play */
		while (!player_end_track)
		{
			byte buf[8192];
			int size = 8192;
			struct timespec tv;

			if (player_status == PLAYER_STATUS_PLAYING)
			{
				/* Update equalizer if it's parameters have changed */
				if (player_eq_changed)
				{
					player_eq_changed = FALSE;
					inp_set_eq(s->m_inp);
				}
				
				/* Get stream from input plugin */
				if (size = inp_get_stream(s->m_inp, buf, size))
				{
					int new_ch, new_freq;
					dword new_fmt;
					
					/* Update audio parameters if they have changed */
					if (!no_outp)
					{
						inp_get_audio_params(s->m_inp, &new_ch, &new_freq, 
								&new_fmt);
						if (ch != new_ch || freq != new_freq || fmt != new_fmt)
						{
							ch = new_ch;
							freq = new_freq;
							fmt = new_fmt;
						
							outp_flush(pmng_cur_out);
							outp_set_channels(pmng_cur_out, 2);
							outp_set_freq(pmng_cur_out, freq);
							outp_set_fmt(pmng_cur_out, fmt);
						}

						/* Apply effects */
						size = pmng_apply_effects(buf, size, fmt, freq, 2);
					
						/* Send to output plugin */
						outp_play(pmng_cur_out, buf, size);
					}
				}
				else
				{
					break;
				}
			}
		
			/* Sleep a little */
			util_delay(0, 100000L);
		}

		/* Wait until we really stop playing */
		if (!no_outp)
			outp_flush(pmng_cur_out);

		/* Stop timer thread */
		player_stop_timer();

		/* End playing */
		inp_end(s->m_inp);
		player_inp = NULL;

		/* End output plugin */
		if (!no_outp)
			outp_end(pmng_cur_out);

		/* Send message about track end */
		if (!player_end_track)
			player_next_track();

		/* Update screen */
		wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
	}
	return NULL;
} /* End of 'player_thread' function */

/* Stop timer thread */
void player_stop_timer( void )
{
	if (player_timer_tid != 0)
	{
		player_end_timer = TRUE;
		pthread_join(player_timer_tid, NULL);
		player_end_timer = FALSE;
		player_timer_tid = 0;
		player_cur_time = 0;
	}
} /* End of 'player_stop_timer' function */

/* Timer thread function */
void *player_timer_func( void *arg )
{
	time_t t = time(NULL);

	/* Start zero timer count */
	player_cur_time = 0;

	/* Timer loop */
	while (!player_end_timer)
	{
		time_t new_t = time(NULL);
		struct timespec tv;
		int tm;

		/* Update timer */
		if (player_status == PLAYER_STATUS_PAUSED)
		{
			t = new_t;
		}
		else
		{	
			tm = inp_get_cur_time(player_inp);
			if (tm < 0)
			{
				if (new_t > t)
				{
					player_cur_time += (new_t - t);
					t = new_t;
					wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
				}
			}
			else if (tm - player_cur_time)
			{
				player_cur_time = tm;
				wnd_send_msg(wnd_root, WND_MSG_DISPLAY, 0);
			}
		}

		/* Sleep a little */
		tv.tv_sec = 0;
		tv.tv_nsec = 100000L;
		nanosleep(&tv, NULL);
	}

	player_cur_time = 0;
	player_end_timer = FALSE;
	player_timer_tid = 0;
	return NULL;
} /* End of 'player_timer_func' function */

/* Process add file dialog */
void player_add_dialog( void )
{
	file_input_box_t *fin;

	/* Create edit box for path input */
	fin = fin_new(wnd_root, 0, wnd_root->m_height - 1, 
			wnd_root->m_width, _("Enter file (or directory) name: "));
	if (fin != NULL)
	{
		((editbox_t *)fin)->m_hist_list = 
			player_hist_lists[PLAYER_HIST_LIST_ADD];

		/* Run message loop */
		wnd_run(fin);

		/* Add file if enter was pressed */
		if (fin->m_box.m_last_key == '\n')
			plist_add(player_plist, fin->m_box.m_text);

		/* Destroy edit box */
		wnd_destroy(fin);
	}
} /* End of 'player_add_dialog' function */

/* Process save play list dialog */
void player_save_dialog( void )
{
	file_input_box_t *fin;

	/* Create edit box for path input */
	fin = fin_new(wnd_root, 0, wnd_root->m_height - 1, wnd_root->m_width, 
			_("Enter file name: "));
	if (fin != NULL)
	{
		((editbox_t *)fin)->m_hist_list = 
			player_hist_lists[PLAYER_HIST_LIST_SAVE];

		/* Run message loop */
		wnd_run(fin);

		/* Add file if enter was pressed */
		if (fin->m_box.m_last_key == '\n')
		{
			bool res = plist_save(player_plist, fin->m_box.m_text);
			strcpy(player_msg, (res) ? _("Play list saved") : 
					_("Unable to save play list"));
		}

		/* Destroy edit box */
		wnd_destroy(fin);
	}
} /* End of 'player_save_dialog' function */

/* Process remove song(s) dialog */
void player_rem_dialog( void )
{
	int was = player_plist->m_len;
	
	plist_rem(player_plist);
	sprintf(player_msg, _("Removed %i songs"), was - player_plist->m_len);
} /* End of 'player_rem_dialog' function */

/* Process sort play list dialog */
void player_sort_dialog( void )
{
	choice_ctrl_t *ch;
	char choice;
	int t;
	bool g;

	/* Get sort globalness parameter */
	ch = choice_new(wnd_root, 0, wnd_root->m_height - 1, wnd_root->m_width,
			1, _("Sort globally? (Yes/No)"), "yn");
	if (ch == NULL)
		return;
	wnd_run(ch);
	choice = ch->m_choice;
	wnd_destroy(ch);
	if (!CHOICE_VALID(choice))
		return;
	g = (choice == 'y');
	
	/* Get sort criteria */
	ch = choice_new(wnd_root, 0, wnd_root->m_height - 1, wnd_root->m_width,
		1, _("Sort by: (T)itle, (F)ile name, (P)ath and file name"), "tfp");
	if (ch == NULL)
		return;
	wnd_run(ch);
	choice = ch->m_choice;
	wnd_destroy(ch);
	if (!CHOICE_VALID(choice))
		return;

	/* Sort */
	switch (choice)
	{
	case 't':
		t = PLIST_SORT_BY_TITLE;
		break;
	case 'f':
		t = PLIST_SORT_BY_NAME;
		break;
	case 'p':
		t = PLIST_SORT_BY_PATH;
		break;
	}
	plist_sort(player_plist, g, t);
} /* End of 'player_sort_dialog' function */

/* Process search play list dialog */
void player_search_dialog( void )
{
	editbox_t *ebox;

	/* Display edit box for entering search text */
	ebox = ebox_new(wnd_root, 0, wnd_root->m_height - 1, wnd_root->m_width,
			1, 256, _("Enter search string: "), "");
	ebox->m_hist_list = 
		player_hist_lists[PLAYER_HIST_LIST_SEARCH];
	if (ebox != NULL)
	{
		wnd_run(ebox);

		/* Save search parameters and search */
		if (ebox->m_last_key == '\n')
		{
			strcpy(player_search_string, ebox->m_text);
			if (!plist_search(player_plist, player_search_string, 1))
				strcpy(player_msg, _("String not found"));
			else
				strcpy(player_msg, _("String found"));
		}
		wnd_destroy(ebox);
	}
} /* End of 'player_search_dialog' function */

/* Process song info dialog */
void player_info_dialog( void )
{
	dlgbox_t *dlg;
	song_t *s;
	editbox_t *name, *album, *artist, *year, *comments, *track;
	listbox_t *genre;
	genre_list_t *glist;
	int i;

	/* Get song object */
	if (player_plist->m_sel_end < 0 || !player_plist->m_len)
		return;
	else
		s = player_plist->m_list[player_plist->m_sel_end];

	/* Update song information */
	song_update_info(s);

	/* Check if there exists song information */
	if (s->m_info == NULL)
		return;

	/* Create info dialog */
	dlg = dlg_new(wnd_root, 2, 2, wnd_root->m_width - 4, 20, 
			cfg_get_var_int(cfg_list, "info_editor_show_full_name") ? 
			s->m_file_name : util_get_file_short_name(s->m_file_name));
	name = ebox_new((wnd_t *)dlg, 2, 1, wnd_root->m_width - 10, 1, 
			256, _("Song name: "), s->m_info->m_name);
	artist = ebox_new((wnd_t *)dlg, 2, 2, wnd_root->m_width - 10, 1, 
			256, _("Artist name: "), s->m_info->m_artist);
	album = ebox_new((wnd_t *)dlg, 2, 3, wnd_root->m_width - 10, 1, 
			256, _("Album name: "), s->m_info->m_album);
	year = ebox_new((wnd_t *)dlg, 2, 4, wnd_root->m_width - 10, 1, 
			4, _("Year: "), s->m_info->m_year);
	track = ebox_new((wnd_t *)dlg, 2, 5, wnd_root->m_width - 10, 1, 
			4, _("Track No: "), s->m_info->m_track);
	comments = ebox_new((wnd_t *)dlg, 2, 6, wnd_root->m_width - 10, 1, 
			256, _("Comments: "), s->m_info->m_comments);
	genre = lbox_new((wnd_t *)dlg, 2, 7, wnd_root->m_width - 10, 12,
			_("Genre: "));
	glist = inp_get_glist(s->m_inp);
	for ( i = 0; glist != NULL && i < glist->m_size; i ++ )
		lbox_add(genre, glist->m_list[i].m_name);
	lbox_move_cursor(genre, FALSE, 
			(s->m_info->m_genre == GENRE_ID_UNKNOWN) ? -1 : 
			s->m_info->m_genre, FALSE);
	wnd_run(dlg);

	/* Save */
	if (dlg->m_ok)
	{
		/* Remember information */
		strcpy(s->m_info->m_name, name->m_text);
		strcpy(s->m_info->m_artist, artist->m_text);
		strcpy(s->m_info->m_album, album->m_text);
		strcpy(s->m_info->m_year, year->m_text);
		strcpy(s->m_info->m_comments, comments->m_text);
		strcpy(s->m_info->m_track, track->m_text);
		s->m_info->m_genre = genre->m_cursor;
	
		/* Get song length and information at first */
		inp_save_info(s->m_inp, s->m_file_name, s->m_info);

		/* Update */
		song_update_info(s);
	}
	
	wnd_destroy(dlg);
} /* End of 'player_info_dialog' function */

/* Show help screen */
void player_help( void )
{
	help_screen_t *h;

	h = help_new(wnd_root, 0, 0, wnd_root->m_width, wnd_root->m_height);
	wnd_run(h);
	wnd_destroy(h);
} /* End of 'player_help' function */

/* Start next track */
void player_next_track( void )
{
	player_skip_songs(1);
} /* End of 'player_next_track' function */

/* Handle non-digit key (place it to buffer) */
void player_handle_non_digit( int key )
{
} /* End of 'player_handle_non_digit' function */

/* Execute key action */
void player_exec_key_action( void )
{
} /* End of 'player_exec_key_action' function */

/* Set volume */
void player_set_vol( int vol, bool rel )
{
	player_volume = (rel) ? player_volume + vol : vol;
	if (player_volume < 0)
		player_volume = 0;
	else if (player_volume > 100)
		player_volume = 100;
	outp_set_volume(pmng_cur_out, player_volume);
} /* End of 'player_set_vol' function */

/* Display slider */
void player_display_slider( wnd_t *wnd, int x, int y, int width, 
		int pos, int range )
{
	int i, slider_pos;
	
	col_set_color(wnd, COL_EL_SLIDER);
	wnd_move(wnd, x, y);
	slider_pos = (range) ? (pos * width / range) : 0;
	for ( i = 0; i <= width; i ++ )
	{
		if (i == slider_pos)
			wnd_printf(wnd, "O");
		else 
			wnd_printf(wnd, "=");
	}
	col_set_color(wnd, COL_EL_DEFAULT);
} /* End of 'player_display_slider' function */

/* Process equalizer dialog */
void player_eq_dialog( void )
{
	eq_wnd_t *wnd;

	wnd = eqwnd_new(wnd_root, 0, 0, wnd_root->m_width, wnd_root->m_height);
	wnd_run(wnd);
	wnd_destroy(wnd);
} /* End of 'player_eq_dialog' function */

/* Skip some songs */
void player_skip_songs( int num )
{
	if (player_plist == NULL || !player_plist->m_len)
		return;
	
	/* Change current song */
	if (cfg_get_var_int(cfg_list, "shuffle_play"))
	{
		int initial = player_plist->m_cur_song;

		while (player_plist->m_cur_song == initial)
			player_plist->m_cur_song = rand() % player_plist->m_len;
	}
	else 
	{
		player_plist->m_cur_song += num;
		if (cfg_get_var_int(cfg_list, "loop_play"))
		{
			while (player_plist->m_cur_song < 0)
				player_plist->m_cur_song += player_plist->m_len;
			player_plist->m_cur_song %= player_plist->m_len;
		}
		else if (player_plist->m_cur_song < 0 || 
					player_plist->m_cur_song >= player_plist->m_len)
			player_plist->m_cur_song = -1;
	}

	/* Start or end play */
	if (player_plist->m_cur_song == -1)
		player_end_play();
	else
		player_play();
} /* End of 'player_skip_songs' function */

/* Launch variables manager */
void player_var_manager( void )
{
	dlgbox_t *dlg;
	listbox_t *var_lb;
	editbox_t *val_eb;
	button_t *btn;
	int i;

	/* Initialize dialog */
	dlg = dlg_new(wnd_root, 2, 2, wnd_root->m_width - 4, 20, 
			_("Variables manager"));
	wnd_register_handler(dlg, WND_MSG_NOTIFY, player_var_mngr_notify);
	var_lb = lbox_new(WND_OBJ(dlg), 2, 2, WND_WIDTH(dlg) - 4, 
			WND_HEIGHT(dlg) - 5, "");
	var_lb->m_minimalizing = FALSE;
	WND_OBJ(var_lb)->m_id = PLAYER_VAR_MNGR_VARS;
	val_eb = ebox_new(WND_OBJ(dlg), 2, WND_HEIGHT(dlg) - 3,
			WND_WIDTH(dlg) - 4, 1, 256, "Value: ", 
			(cfg_list->m_num_vars) ? cfg_list->m_vars[0].m_val : "");
	WND_OBJ(val_eb)->m_id = PLAYER_VAR_MNGR_VAL;
	btn = btn_new(WND_OBJ(dlg), 2, WND_HEIGHT(dlg) - 2, -1, "New variable");
	WND_OBJ(btn)->m_id = PLAYER_VAR_MNGR_NEW;
	btn = btn_new(WND_OBJ(dlg), WND_X(btn) + WND_WIDTH(btn) + 1, 
			WND_HEIGHT(dlg) - 2, -1, "Save");
	WND_OBJ(btn)->m_id = PLAYER_VAR_MNGR_SAVE;
	btn = btn_new(WND_OBJ(dlg), WND_X(btn) + WND_WIDTH(btn) + 1,
		   	WND_HEIGHT(dlg) - 2, -1, "Restore value");
	WND_OBJ(btn)->m_id = PLAYER_VAR_MNGR_RESTORE;

	/* Fill variables list box */
	for ( i = 0; i < cfg_list->m_num_vars; i ++ )
		lbox_add(var_lb, cfg_list->m_vars[i].m_name);
	lbox_move_cursor(var_lb, FALSE, 0, TRUE);

	/* Display dialog */
	wnd_run(dlg);

	/* Save variables */
	if (var_lb->m_cursor >= 0)
		cfg_set_var(cfg_list, var_lb->m_list[var_lb->m_cursor].m_name, 
				val_eb->m_text);

	/* Free memory */
	wnd_destroy(dlg);
} /* End of 'player_var_manager' function */

/* Launch add object dialog */
void player_add_obj_dialog( void )
{
	editbox_t *ebox;

	/* Create edit box for path input */
	ebox = ebox_new(wnd_root, 0, wnd_root->m_height - 1, 
			wnd_root->m_width, 1, 256, _("Enter object name: "), "");
	if (ebox != NULL)
	{
		ebox->m_hist_list = player_hist_lists[PLAYER_HIST_LIST_ADD_OBJ];
		
		/* Run message loop */
		wnd_run(ebox);

		/* Add object if enter was pressed */
		if (ebox->m_last_key == '\n')
			plist_add_obj(player_plist, ebox->m_text);

		/* Destroy edit box */
		wnd_destroy(ebox);
	}
} /* End of 'player_add_obj_dialog' function */

/* Handle action */
void player_handle_action( int action )
{
	char str[10];
	editbox_t *ebox;
	bool dont_change_repval = FALSE;

	/* Clear message string */
	strcpy(player_msg, "");

	switch (action)
	{
	/* Exit MPFC */
	case KBIND_QUIT:
		wnd_send_msg(wnd_root, WND_MSG_CLOSE, 0);
		break;

	/* Show help screen */
	case KBIND_HELP:
		player_help();
		break;

	/* Move cursor */
	case KBIND_MOVE_DOWN:
		plist_move(player_plist, (player_repval == 0) ? 1 : player_repval, 
				TRUE);
		break;
	case KBIND_MOVE_UP:
		plist_move(player_plist, (player_repval == 0) ? -1 : -player_repval, 
				TRUE);
		break;
	case KBIND_SCREEN_DOWN:
		plist_move(player_plist, (player_repval == 0) ? 
				player_plist->m_height : 
				player_plist->m_height * player_repval, TRUE);
		break;
	case KBIND_SCREEN_UP:
		plist_move(player_plist, (player_repval == 0) ? 
				-player_plist->m_height : 
				-player_plist->m_height * player_repval, TRUE);
		break;
	case KBIND_MOVE:
		plist_move(player_plist, (player_repval == 0) ? 
				player_plist->m_len - 1 : player_repval - 1, FALSE);
		break;

	/* Seek song */
	case KBIND_TIME_FW:
		player_seek((player_repval == 0) ? 10 : 10 * player_repval, TRUE);
		break;
	case KBIND_TIME_BW:
		player_seek((player_repval == 0) ? -10 : -10 * player_repval, TRUE);
		break;
	case KBIND_TIME_MOVE:
		player_seek((player_repval == 0) ? 0 : player_repval, FALSE);
		break;

	/* Increase/decrease volume */
	case KBIND_VOL_FW:
		player_set_vol((player_repval == 0) ? 5 : 5 * player_repval, TRUE);
		break;
	case KBIND_VOL_BW:
		player_set_vol((player_repval == 0) ? -5 : -5 * player_repval, TRUE);
		break;

	/* Centrize view */
	case KBIND_CENTRIZE:
		plist_centrize(player_plist);
		break;

	/* Enter visual mode */
	case KBIND_VISUAL:
		player_plist->m_visual = !player_plist->m_visual;
		break;

	/* Resume playing */
	case KBIND_PLAY:
		if (player_status == PLAYER_STATUS_PAUSED)
		{
			player_status = PLAYER_STATUS_PLAYING;
			inp_resume(player_inp);
		}
		else
			player_play();
		break;

	/* Pause */
	case KBIND_PAUSE:
		if (player_status == PLAYER_STATUS_PLAYING)
		{
			player_status = PLAYER_STATUS_PAUSED;
			inp_pause(player_inp);
		}
		else if (player_status == PLAYER_STATUS_PAUSED)
		{
			player_status = PLAYER_STATUS_PLAYING;
			inp_resume(player_inp);
		}
		break;

	/* Stop */
	case KBIND_STOP:
		player_end_play();
		break;

	/* Play song */
	case KBIND_START_PLAY:
		if (!player_plist->m_len)
			break;
		player_plist->m_cur_song = player_plist->m_sel_end;
		player_play();
		break;

	/* Go to next song */
	case KBIND_NEXT:
		player_skip_songs((player_repval) ? player_repval : 1);
		break;

	/* Go to previous song */
	case KBIND_PREV:
		player_skip_songs(-((player_repval) ? player_repval : 1));
		break;
		break;

	/* Add a file */
	case KBIND_ADD:
		player_add_dialog();
		break;

	/* Add an object */
	case KBIND_ADD_OBJ:
		player_add_obj_dialog();
		break;

	/* Save play list */
	case KBIND_SAVE:
		player_save_dialog();
		break;

	/* Remove song(s) */
	case KBIND_REM:
		player_rem_dialog();
		break;

	/* Sort play list */
	case KBIND_SORT:
		player_sort_dialog();
		break;

	/* Song info dialog */
	case KBIND_INFO:
		player_info_dialog();
		break;

	/* Search */
	case KBIND_SEARCH:
		player_search_dialog();
		break;

	/* Find next/previous search match */
	case KBIND_NEXT_MATCH:
	case KBIND_PREV_MATCH:
		if (!plist_search(player_plist, player_search_string, 
					(action == KBIND_NEXT_MATCH) ? 1 : -1))
			strcpy(player_msg, _("String not found"));
		else
			strcpy(player_msg, _("String found"));
		break;

	/* Show equalizer dialog */
	case KBIND_EQUALIZER:
		player_eq_dialog();
		break;

	/* Set/unset shuffle mode */
	case KBIND_SHUFFLE:
		cfg_set_var_int(cfg_list, "shuffle_play",
				!cfg_get_var_int(cfg_list, "shuffle_play"));
		break;
		
	/* Set/unset loop mode */
	case KBIND_LOOP:
		cfg_set_var_int(cfg_list, "loop_play",
				!cfg_get_var_int(cfg_list, "loop_play"));
		break;

	/* Variables manager */
	case KBIND_VAR_MANAGER:
		player_var_manager();
		break;
	case KBIND_VAR_MINI_MANAGER:
		player_var_mini_mngr();
		break;

	/* Move play list */
	case KBIND_PLIST_DOWN:
		plist_move_sel(player_plist, (player_repval == 0) ? 1 : player_repval, 
				TRUE);
		break;
	case KBIND_PLIST_UP:
		plist_move_sel(player_plist, (player_repval == 0) ? -1 : 
				-player_repval, TRUE);
		break;
	case KBIND_PLIST_MOVE:
		plist_move_sel(player_plist, (player_repval == 0) ? 
				1 : player_repval - 1, FALSE);
		break;
		
	/* Digit means command repeation value edit */
	case KBIND_DIG_1:
	case KBIND_DIG_2:
	case KBIND_DIG_3:
	case KBIND_DIG_4:
	case KBIND_DIG_5:
	case KBIND_DIG_6:
	case KBIND_DIG_7:
	case KBIND_DIG_8:
	case KBIND_DIG_9:
	case KBIND_DIG_0:
		/* Create edit box */
		str[0] = (action - KBIND_DIG_0) + '0';
		str[1] = 0;
		ebox = ebox_new(wnd_root, 0, wnd_root->m_height - 1, 
				wnd_root->m_width, 1, 5, 
					_("Enter repeat value for the next command: "), str);
		if (ebox != NULL)
		{
			/* Run edit box message loop */
			wnd_register_handler(ebox, WND_MSG_KEYDOWN,
					player_repval_handle_key);
			wnd_run(ebox);

			/* Remember repeat value or handle last pressed command */
			if (ebox->m_last_key != 27)
			{
				player_repval = atoi(ebox->m_text);
				dont_change_repval = TRUE;
				player_handle_key(wnd_root, ebox->m_last_key);
			}
			
			/* Destroy edit box */
			wnd_destroy(ebox);
		}
		break;
	}

	/* Flush repeat value */
	if (!dont_change_repval)
		player_repval = 0;
} /* End of 'player_handle_action' function */

/* Launch variables mini-manager */
void player_var_mini_mngr( void )
{
	editbox_t *ebox;
	char name[256], val[256];

	/* Create edit box for variable name input */
	ebox = ebox_new(wnd_root, 0, wnd_root->m_height - 1, 
			wnd_root->m_width, 1, 256, _("Enter variable name: "), "");
	if (ebox == NULL)
		return;
	ebox->m_hist_list = player_hist_lists[PLAYER_HIST_LIST_VAR_NAME];

	/* Run message loop */
	wnd_run(ebox);

	/* Check input */
	if (ebox->m_last_key != '\n' || !ebox->m_len)
	{
		wnd_destroy(ebox);
		return;
	}
	strcpy(name, ebox->m_text);
	wnd_destroy(ebox);

	/* Get value */
	ebox = ebox_new(wnd_root, 0, wnd_root->m_height - 1, 
			wnd_root->m_width, 1, 256, _("Enter variable value: "), "");
	if (ebox == NULL)
		return;
	ebox->m_hist_list = player_hist_lists[PLAYER_HIST_LIST_VAR_VAL];
	wnd_run(ebox);
	if (ebox->m_last_key != '\n')
	{
		wnd_destroy(ebox);
		return;
	}
	strcpy(val, ebox->m_len ? ebox->m_text : "1");
	wnd_destroy(ebox);

	/* Set variable value */
	cfg_set_var(cfg_list, name, val);
} /* End of 'player_var_mini_mngr' function */

/* Variables manager dialog notify handler */
void player_var_mngr_notify( wnd_t *wnd, dword data )
{
	dlgbox_t *dlg;
	short id, act;
	static int pos = -1;
	editbox_t *eb;
	listbox_t *lb;

	if (wnd == NULL)
		return;

	/* Get message data */
	dlg = (dlgbox_t *)wnd;
	id = WND_NOTIFY_ID(data);
	act = WND_NOTIFY_ACT(data);
	eb = (editbox_t *)dlg_get_item_by_id(dlg, PLAYER_VAR_MNGR_VAL);
	lb = (listbox_t *)dlg_get_item_by_id(dlg, PLAYER_VAR_MNGR_VARS);
	if (eb == NULL || lb == NULL)
		return;

	/* List box cursor movement */
	if (id == PLAYER_VAR_MNGR_VARS && act == LBOX_MOVE)
	{
		/* Save current edit box value to the respective variable */
		if (pos >= 0)
			cfg_set_var(cfg_list, lb->m_list[pos].m_name, eb->m_text);
			
		/* Read new variable */
		pos = lb->m_cursor;
		ebox_set_text(eb, cfg_list->m_vars[pos].m_val);
	}
	/* Save list */
	else if (id == PLAYER_VAR_MNGR_SAVE && act == BTN_CLICKED)
	{
		cfg_save_list(cfg_list, "~/.mpfcrc");
	}
	/* Restore value */
	else if (id == PLAYER_VAR_MNGR_RESTORE && act == BTN_CLICKED)
	{
		if (lb->m_cursor >= 0)
		{
			ebox_set_text(eb, cfg_get_var(cfg_list,
						lb->m_list[lb->m_cursor].m_name));
		}
	}
	/* New variable */
	else if (id == PLAYER_VAR_MNGR_NEW && act == BTN_CLICKED)
	{
		dlgbox_t *d;
		editbox_t *name, *val;

		d = dlg_new(WND_OBJ(dlg), 4, 4, 40, 5, "New variable");
		name = ebox_new(WND_OBJ(d), 4, 3, WND_WIDTH(d) - 5, 1, 256, 
				"Name: ", "");
		val = ebox_new(WND_OBJ(d), 4, 4, WND_WIDTH(d) - 5, 1, 256, 
				"Value: ", "");
		wnd_run(d);
		if (d->m_ok)
		{
			int was_len = cfg_list->m_num_vars;

			/* Set variable */
			cfg_set_var(cfg_list, name->m_text, val->m_text);

			/* Update main dialog items */
			if (was_len != cfg_list->m_num_vars)
			{
				lbox_add(lb, name->m_text);
			}
			else
			{
				if (lb->m_cursor >= 0 && 
						!strcmp(lb->m_list[lb->m_cursor].m_name, name->m_text))
					ebox_set_text(eb, val->m_text);
			}
		}
		wnd_destroy(d);
	}
} /* End of 'player_var_mngr_notify' function */

/* End of 'player.c' file */

