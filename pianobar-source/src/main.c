/*
Copyright (c) 2008-2013
	Lars-Dominik Braun <lars@6xq.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "config.h"

/* system includes */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* fork () */
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <ctype.h>
/* open () */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
/* tcset/getattr () */
#include <termios.h>
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>
/* waitpid () */
#include <sys/types.h>
#include <sys/wait.h>

/* pandora.com library */
#include <piano.h>

#include "main.h"
#include "terminal.h"
#include "ui.h"
#include "ui_dispatch.h"
#include "ui_readline.h"

/*	authenticate user
 */
static bool BarMainLoginUser (BarApp_t *app) {
	PianoReturn_t pRet;
	CURLcode wRet;
	PianoRequestDataLogin_t reqData;
	bool ret;

	reqData.user = app->settings.username;
	reqData.password = app->settings.password;
	reqData.step = 0;

	BarUiMsg (&app->settings, MSG_INFO, "Login... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_LOGIN, &reqData, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "userlogin", NULL, NULL, &app->player,
			NULL, pRet, wRet);

	return ret;
}

/*	ask for username/password if none were provided in settings
 */
static bool BarMainGetLoginCredentials (BarSettings_t *settings,
		BarReadlineFds_t *input) {
	bool usernameFromConfig = true;

	if (settings->username == NULL) {
		char nameBuf[100];

		BarUiMsg (settings, MSG_QUESTION, "Email: ");
		BarReadlineStr (nameBuf, sizeof (nameBuf), input, BAR_RL_DEFAULT);
		settings->username = strdup (nameBuf);
		usernameFromConfig = false;
	}

	if (settings->password == NULL) {
		char passBuf[100];

		if (usernameFromConfig) {
			BarUiMsg (settings, MSG_QUESTION, "Email: %s\n", settings->username);
		}

		if (settings->passwordCmd == NULL) {
			BarUiMsg (settings, MSG_QUESTION, "Password: ");
			BarReadlineStr (passBuf, sizeof (passBuf), input, BAR_RL_NOECHO);
			/* write missing newline */
			puts ("");
			settings->password = strdup (passBuf);
		} else {
			pid_t chld;
			int pipeFd[2];

			BarUiMsg (settings, MSG_INFO, "Requesting password from external helper... ");

			if (pipe (pipeFd) == -1) {
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				return false;
			}

			chld = fork ();
			if (chld == 0) {
				/* child */
				close (pipeFd[0]);
				dup2 (pipeFd[1], fileno (stdout));
				execl ("/bin/sh", "/bin/sh", "-c", settings->passwordCmd, (char *) NULL);
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				close (pipeFd[1]);
				exit (1);
			} else if (chld == -1) {
				BarUiMsg (settings, MSG_NONE, "Error: %s\n", strerror (errno));
				return false;
			} else {
				/* parent */
				int status;

				close (pipeFd[1]);
				memset (passBuf, 0, sizeof (passBuf));
				read (pipeFd[0], passBuf, sizeof (passBuf)-1);
				close (pipeFd[0]);

				/* drop trailing newlines */
				ssize_t len = strlen (passBuf)-1;
				while (len >= 0 && passBuf[len] == '\n') {
					passBuf[len] = '\0';
					--len;
				}

				waitpid (chld, &status, 0);
				if (WEXITSTATUS (status) == 0) {
					settings->password = strdup (passBuf);
					BarUiMsg (settings, MSG_NONE, "Ok.\n");
				} else {
					BarUiMsg (settings, MSG_NONE, "Error: Exit status %i.\n", WEXITSTATUS (status));
					return false;
				}
			}
		} /* end else passwordCmd */
	}

	return true;
}

/*	get station list
 */
static bool BarMainGetStations (BarApp_t *app) {
	PianoReturn_t pRet;
	CURLcode wRet;
	bool ret;

	BarUiMsg (&app->settings, MSG_INFO, "Get stations... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_GET_STATIONS, NULL, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "usergetstations", NULL, NULL, &app->player,
			app->ph.stations, pRet, wRet);
	return ret;
}

/*	get initial station from autostart setting or user input
 */
static void BarMainGetInitialStation (BarApp_t *app) {
	/* try to get autostart station */
	if (app->settings.autostartStation != NULL) {
		app->curStation = PianoFindStationById (app->ph.stations,
				app->settings.autostartStation);
		if (app->curStation == NULL) {
			BarUiMsg (&app->settings, MSG_ERR,
					"Error: Autostart station not found.\n");
		}
	}
	/* no autostart? ask the user */
	if (app->curStation == NULL) {
		app->curStation = BarUiSelectStation (app, app->ph.stations,
				"Select station: ", NULL, app->settings.autoselect);
	}
	if (app->curStation != NULL) {
		BarUiPrintStation (&app->settings, app->curStation);
	}
}

/*	wait for user input
 */
static void BarMainHandleUserInput (BarApp_t *app) {
	char buf[2];
	if (BarReadline (buf, sizeof (buf), NULL, &app->input,
			BAR_RL_FULLRETURN | BAR_RL_NOECHO | BAR_RL_NOINT, 1) > 0) {
		BarUiDispatch (app, buf[0], app->curStation, app->playlist, true,
				BAR_DC_GLOBAL);
	}
}

/*	fetch new playlist
 */
static void BarMainGetPlaylist (BarApp_t *app) {
	PianoReturn_t pRet;
	CURLcode wRet;
	PianoRequestDataGetPlaylist_t reqData;
	reqData.station = app->curStation;
	reqData.quality = app->settings.audioQuality;

	BarUiMsg (&app->settings, MSG_INFO, "Receiving new playlist... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_GET_PLAYLIST,
			&reqData, &pRet, &wRet)) {
		app->curStation = NULL;
	} else {
		app->playlist = reqData.retPlaylist;
		if (app->playlist == NULL) {
			BarUiMsg (&app->settings, MSG_INFO, "No tracks left.\n");
			app->curStation = NULL;
		}
	}
	BarUiStartEventCmd (&app->settings, "stationfetchplaylist",
			app->curStation, app->playlist, &app->player, app->ph.stations,
			pRet, wRet);
}

/*	start new player thread
 */
static void BarMainStartPlayback (BarApp_t *app, pthread_t *playerThread) {
	assert (app != NULL);
	assert (playerThread != NULL);

	const PianoSong_t * const curSong = app->playlist;
	assert (curSong != NULL);

	BarUiPrintSong (&app->settings, curSong, app->curStation->isQuickMix ?
			PianoFindStationById (app->ph.stations,
			curSong->stationId) : NULL);

	static const char httpPrefix[] = "http://";
	/* avoid playing local files */
	if (curSong->audioUrl == NULL ||
			strncmp (curSong->audioUrl, httpPrefix, strlen (httpPrefix)) != 0) {
		BarUiMsg (&app->settings, MSG_ERR, "Invalid song url.\n");
	} else {
		/* setup player */
		memset (&app->player, 0, sizeof (app->player));

		app->player.url = curSong->audioUrl;
		app->player.gain = curSong->fileGain;
		app->player.settings = &app->settings;
		app->player.songDuration = curSong->length;
		pthread_mutex_init (&app->player.pauseMutex, NULL);
		pthread_cond_init (&app->player.pauseCond, NULL);

		assert (interrupted == &app->doQuit);
		interrupted = &app->player.interrupted;

		/* throw event */
		BarUiStartEventCmd (&app->settings, "songstart",
				app->curStation, curSong, &app->player, app->ph.stations,
				PIANO_RET_OK, CURLE_OK);

		/* prevent race condition, mode must _not_ be DEAD if
		 * thread has been started */
		app->player.mode = PLAYER_WAITING;
		/* start player */
		pthread_create (playerThread, NULL, BarPlayerThread,
				&app->player);
	}
}

/*	player is done, clean up
 */
static void BarMainPlayerCleanup (BarApp_t *app, pthread_t *playerThread) {
	void *threadRet;

	BarUiStartEventCmd (&app->settings, "songfinish", app->curStation,
			app->playlist, &app->player, app->ph.stations, PIANO_RET_OK,
			CURLE_OK);

	/* FIXME: pthread_join blocks everything if network connection
	 * is hung up e.g. */
	pthread_join (*playerThread, &threadRet);
	pthread_cond_destroy (&app->player.pauseCond);
	pthread_mutex_destroy (&app->player.pauseMutex);

	if (threadRet == (void *) PLAYER_RET_OK) {
		app->playerErrors = 0;
	} else if (threadRet == (void *) PLAYER_RET_SOFTFAIL) {
		++app->playerErrors;
		if (app->playerErrors >= app->settings.maxPlayerErrors) {
			/* don't continue playback if thread reports too many error */
			app->curStation = NULL;
		}
	} else {
		app->curStation = NULL;
	}

	memset (&app->player, 0, sizeof (app->player));

	assert (interrupted == &app->player.interrupted);
	interrupted = &app->doQuit;
}

/*	print song duration
 */
static void BarMainPrintTime (BarApp_t *app) {
	unsigned int songRemaining;
	char sign;

	if (app->player.songPlayed <= app->player.songDuration) {
		songRemaining = app->player.songDuration - app->player.songPlayed;
		sign = '-';
	} else {
		/* longer than expected */
		songRemaining = app->player.songPlayed - app->player.songDuration;
		sign = '+';
	}
	BarUiMsg (&app->settings, MSG_TIME, "%c%02u:%02u/%02u:%02u\r",
			sign, songRemaining / 60, songRemaining % 60,
			app->player.songDuration / 60,
			app->player.songDuration % 60);
}

static bool autoRateSong (BarApp_t *app, char *trackToken, char *stationId, char *rating) {
	if (trackToken == NULL || stationId == NULL || rating == NULL) {
		return false;
	}

	printf("Autorating song...\n");

	PianoSong_t song = {
		.trackToken = trackToken,
		.stationId = stationId,
		.head = NULL,
		.artist = "",
		.album = "",
		.audioUrl = "",
		.coverArt = "",
		.musicId = "",
		.title = "",
		.seedId = "",
		.feedbackId = "",
		.detailUrl = "",
		.fileGain = 0,
		.length = 0,
		.rating = PIANO_RATE_LOVE,
		.audioFormat = PIANO_AF_UNKNOWN
	};

	PianoStation_t station = {
		.id = stationId,
		.head = NULL,
		.isCreator = true,
		.isQuickMix = false,
		.useQuickMix = false,
		.name = "",
		.seedId = ""
	};

	BarUiDispatchContext_t context = BAR_DC_SONG;

	if (strcmp(rating, "love") == 0) {
		BarUiActLoveSong(app, &station, &song, context);
	} else if (strcmp(rating, "ban") == 0) {
		BarUiActBanSong(app, &station, &song, context);
	} else {
		return false;
	}

	return true;
}

/*	main loop
 */
static void BarMainLoop (BarApp_t *app) {
	pthread_t playerThread;

	if (!BarMainGetLoginCredentials (&app->settings, &app->input)) {
		return;
	}

	if (!BarMainLoginUser (app)) {
		return;
	}

	if (!BarMainGetStations (app)) {
		return;
	}

	if (getenv("AUTORATE_TRACK_TOKEN") != NULL) {
		if (!autoRateSong (app, getenv("AUTORATE_TRACK_TOKEN"),
		                        getenv("AUTORATE_STATION_ID"),
		                        getenv("AUTORATE_RATING"))) {

			printf("Failed to autorate song.\n");
		}

		return;
	}

	BarMainGetInitialStation (app);

	/* little hack, needed to signal: hey! we need a playlist, but don't
	 * free anything (there is nothing to be freed yet) */
	memset (&app->player, 0, sizeof (app->player));

	while (!app->doQuit) {
		/* song finished playing, clean up things/scrobble song */
		if (app->player.mode == PLAYER_FINISHED) {
			if (app->player.interrupted != 0) {
				app->doQuit = 1;
			}
			BarMainPlayerCleanup (app, &playerThread);
		}

		/* check whether player finished playing and start playing new
		 * song */
		if (app->player.mode == PLAYER_DEAD && app->curStation != NULL &&
				!app->doQuit) {
			/* what's next? */
			if (app->playlist != NULL) {
				PianoSong_t *histsong = app->playlist;
				app->playlist = PianoListNextP (app->playlist);
				histsong->head.next = NULL;
				BarUiHistoryPrepend (app, histsong);
			}
			if (app->playlist == NULL) {
				BarMainGetPlaylist (app);
			}
			/* song ready to play */
			if (app->playlist != NULL) {
				BarMainStartPlayback (app, &playerThread);
			}
		}

		BarMainHandleUserInput (app);

		/* show time */
		if (app->player.mode == PLAYER_PLAYING) {
			BarMainPrintTime (app);
		}
	}

	if (app->player.mode != PLAYER_DEAD) {
		pthread_join (playerThread, NULL);
	}
}

sig_atomic_t *interrupted = NULL;

static void intHandler (int signal) {
	if (interrupted != NULL) {
		*interrupted += 1;
	}
}

static void BarMainSetupSigaction () {
	struct sigaction act = {
			.sa_handler = intHandler,
			.sa_flags = 0,
			};
	sigemptyset (&act.sa_mask);
	sigaction (SIGINT, &act, NULL);
}

int main (int argc, char **argv) {
	static BarApp_t app;

	memset (&app, 0, sizeof (app));

	/* save terminal attributes, before disabling echoing */
	BarTermInit ();

	/* signals */
	signal (SIGPIPE, SIG_IGN);
	BarMainSetupSigaction ();
	interrupted = &app.doQuit;

	/* init some things */
	gcry_check_version (NULL);
	gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
	gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
	BarPlayerInit ();

	BarSettingsInit (&app.settings);
	BarSettingsRead (&app.settings);

	PianoReturn_t pret;
	if ((pret = PianoInit (&app.ph, app.settings.partnerUser,
			app.settings.partnerPassword, app.settings.device,
			app.settings.inkey, app.settings.outkey)) != PIANO_RET_OK) {
		BarUiMsg (&app.settings, MSG_ERR, "Initialization failed:"
				" %s\n", PianoErrorToStr (pret));
		return 0;
	}

	BarUiMsg (&app.settings, MSG_NONE,
			"Welcome to " PACKAGE " (" VERSION ")! ");
	if (app.settings.keys[BAR_KS_HELP] == BAR_KS_DISABLED) {
		BarUiMsg (&app.settings, MSG_NONE, "\n");
	} else {
		BarUiMsg (&app.settings, MSG_NONE,
				"Press %c for a list of commands.\n",
				app.settings.keys[BAR_KS_HELP]);
	}

	curl_global_init (CURL_GLOBAL_DEFAULT);
	app.http = curl_easy_init ();
	assert (app.http != NULL);

	/* init fds */
	FD_ZERO(&app.input.set);
	app.input.fds[0] = STDIN_FILENO;
	FD_SET(app.input.fds[0], &app.input.set);

	/* open fifo read/write so it won't EOF if nobody writes to it */
	assert (sizeof (app.input.fds) / sizeof (*app.input.fds) >= 2);
	app.input.fds[1] = open (app.settings.fifo, O_RDWR);
	if (app.input.fds[1] != -1) {
		struct stat s;

		/* check for file type, must be fifo */
		fstat (app.input.fds[1], &s);
		if (!S_ISFIFO (s.st_mode)) {
			BarUiMsg (&app.settings, MSG_ERR, "File at %s is not a fifo\n", app.settings.fifo);
			close (app.input.fds[1]);
			app.input.fds[1] = -1;
		} else {
			FD_SET(app.input.fds[1], &app.input.set);
			BarUiMsg (&app.settings, MSG_INFO, "Control fifo at %s opened\n",
					app.settings.fifo);
		}
	}
	app.input.maxfd = app.input.fds[0] > app.input.fds[1] ? app.input.fds[0] :
			app.input.fds[1];
	++app.input.maxfd;

	BarMainLoop (&app);

	if (app.input.fds[1] != -1) {

		close (app.input.fds[1]);
	}

	/* write statefile */
	BarSettingsWrite (app.curStation, &app.settings);

	PianoDestroy (&app.ph);
	PianoDestroyPlaylist (app.songHistory);
	PianoDestroyPlaylist (app.playlist);
	curl_easy_cleanup (app.http);
	curl_global_cleanup ();
	BarPlayerDestroy ();
	BarSettingsDestroy (&app.settings);

	/* restore terminal attributes, zsh doesn't need this, bash does... */
	BarTermRestore ();

	return 0;
}

