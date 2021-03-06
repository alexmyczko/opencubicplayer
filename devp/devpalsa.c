/** OpenCP Module Player
 * copyright (c) '94-'10 Stian Skjelstad <stian@nixia.no>
 *
 * ALSA (Advanced Linux Sound Architecture) Player device
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  revision history: (please note changes here)
 */

// #define ALSA_DEBUG_OUTPUT 1

#include "config.h"

#ifdef ALSA_DEBUG_OUTPUT
static int debug_output = -1;
#endif
#ifdef ALSA_DEBUG
#define debug_printf(...) fprintf (stderr, __VA_ARGS__)
#else
#define debug_printf(format, args...) ((void)0)
#endif
#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#include <alsa/mixer.h>
#include <alsa/pcm.h>
#include <alsa/pcm_plugin.h>
#include <string.h>
#include "types.h"
#include "boot/plinkman.h"
#include "boot/psetting.h"
#include "cpiface/vol.h"
#include "dev/imsdev.h"
#include "dev/player.h"
#include "filesel/dirdb.h"
#include "filesel/mdb.h"
#include "filesel/modlist.h"
#include "filesel/pfilesel.h"
#include "stuff/framelock.h"
#include "stuff/imsrtns.h"
#include "stuff/poutput.h"

static snd_pcm_t *alsa_pcm = NULL;
static snd_mixer_t *mixer = NULL;

static snd_pcm_status_t *alsa_pcm_status=NULL;
#if SND_LIB_VERSION < 0x01000e
static snd_pcm_info_t *pcm_info = NULL;
#endif
static snd_pcm_hw_params_t *hwparams = NULL;
static snd_pcm_sw_params_t *swparams = NULL;

struct sounddevice plrAlsa;
/*static struct deviceinfo currentcard;*/
static struct mdbreaddirregstruct readdirAlsa;
static void alsaOpenDevice(void);

static char alsaCardName[DEVICE_NAME_MAX+1];
static char alsaMixerName[DEVICE_NAME_MAX+1];

static struct dmDrive *dmSETUP;

/* stolen from devposs */
#define MAX_ALSA_MIXER 256
static char *playbuf;
static int buflen; /* in bytes */
static volatile int kernpos, cachepos, bufpos; /* in bytes */
static volatile int cachelen, kernlen; /* to make life easier */
static struct ocpvolstruct mixer_entries[MAX_ALSA_MIXER];
static int alsa_mixers_n=0;
/*  playbuf     kernpos  cachepos   bufpos      buflen
 *    |           | kernlen | cachelen |          |
 *
 *  on flush, we update all variables> *  on getbufpos we return kernpos-(1 sample) as safe point to buffer up to
 *  on getplaypos we use last known kernpos if locked, else update kernpos
 */

static volatile uint32_t playpos; /* how many samples have we done totally */
static int stereo;
static int bit16;

static volatile int busy=0;
uint32_t custom_dsp_mdb_ref=0xffffffff;
uint32_t custom_mixer_mdb_ref=0xffffffff;

/* 1.0.14rc1 added support for snd_device_name_hint */
#if SND_LIB_VERSION < 0x01000e
static int alsa_1_0_11_or_better;
#endif

static int mlDrawBox(void)
{
	int mlTop=plScrHeight/2-2;
	unsigned int i;

	displayvoid(mlTop+1, 5, plScrWidth-10);
	displayvoid(mlTop+2, 5, plScrWidth-10);
	displayvoid(mlTop+3, 5, plScrWidth-10);
	displaystr(mlTop, 4, 0x04, "\xda", 1);
	for (i=5;i<(plScrWidth-5);i++)
		displaystr(mlTop, i, 0x04, "\xc4", 1);
	displaystr(mlTop, plScrWidth-5, 0x04, "\xbf", 1);
	displaystr(mlTop+1, 4, 0x04, "\xb3", 1);
	displaystr(mlTop+2, 4, 0x04, "\xb3", 1);
	displaystr(mlTop+3, 4, 0x04, "\xb3", 1);
	displaystr(mlTop+1, plScrWidth-5, 0x04, "\xb3", 1);
	displaystr(mlTop+2, plScrWidth-5, 0x04, "\xb3", 1);
	displaystr(mlTop+3, plScrWidth-5, 0x04, "\xb3", 1);
	displaystr(mlTop+4, 4, 0x04, "\xc0", 1);
	for (i=5;i<(plScrWidth-5);i++)
		displaystr(mlTop+4, i, 0x04, "\xc4", 1);
	displaystr(mlTop+4, plScrWidth-5, 0x04, "\xd9", 1);

	return mlTop;
}
/* stolen from devposs */
static int getbufpos(void)
{
	int retval;

	if (busy++)
	{
		/* can't escape if already set, and shouldn't happen */
	}

	if (kernpos==bufpos)
	{
		if (cachelen|kernlen)
		{
			retval=kernpos;
			busy--;
			return retval;
		}
	}
/*
	if ((!cachelen)&&(!kernlen))
		retval=(kernpos+buflen-(0<<(bit16+stereo)))%buflen;
	else*/
		retval=(kernpos+buflen-(1<<(bit16+stereo)))%buflen;
	busy--;
	return retval;
}
/* more or less stolen from devposs */
static int getplaypos(void)
{
	int retval;

	if (busy++)
	{
	} else {
		snd_pcm_sframes_t tmp;
		int err;

		debug_printf("ALSA_getplaypos()\n");

		err=snd_pcm_status(alsa_pcm, alsa_pcm_status);
		debug_printf("      snd_pcm_status(alsa_pcm, alsa_pcm_status) = %s\n", snd_strerror(-err));

		if (err < 0)
		{
			fprintf(stderr, "ALSA: snd_pcm_status() failed: %s\n", snd_strerror(-err));
		} else {
			tmp=snd_pcm_status_get_delay(alsa_pcm_status);

			debug_printf("      snd_pcm_status_get_delay(alsa_pcm_status = %d\n", (int)tmp);
			if (tmp<0)
			{ /* we ignore buffer-underruns */
				tmp=0;
			} else if (tmp==0)
			{ /* ALSA sometimes (atlast on Stians Ubuntu laptop) gives odelay==0 always */
				snd_pcm_sframes_t tmp1 = snd_pcm_status_get_avail_max(alsa_pcm_status);
				snd_pcm_sframes_t tmp2 = snd_pcm_status_get_avail(alsa_pcm_status);
				tmp = tmp1 - tmp2;

				debug_printf("      (no delay available) fallback:\n");
				debug_printf("      snd_pcm_status_get_avail_max() = %d\n", (int)tmp1);
				debug_printf("      snd_pcm_status_get_avail() = %d\n", (int)tmp2);
				debug_printf("      => %d\n", (int)tmp);

				if (tmp<0)
				{
					tmp=0;
				}
			}
			tmp<<=(bit16+stereo);

			if (tmp<kernlen)
			{
				kernlen=tmp;
			}
			kernpos=(cachepos-kernlen+buflen)%buflen;
		}
	}
	retval=kernpos;
	busy--;

	debug_printf(" => %d\n", retval);

	return retval;
}

/* more or less stolen from devposs */
static void flush(void)
{
	int result, n, odelay, tmp;
	int err;

	if (busy++)
	{
		busy--;
		return;
	}

	debug_printf("ALSA_flush()\n");

	err=snd_pcm_status(alsa_pcm, alsa_pcm_status);
	debug_printf("      snd_pcm_status(alsa_pcm, alsa_pcm_status) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_status() failed: %s\n", snd_strerror(-err));
		busy--;
		return;
	}

	odelay=snd_pcm_status_get_delay(alsa_pcm_status);
	debug_printf("      snd_pcm_status_get_delay(alsa_pcm_status) = %d\n", odelay);

	if (odelay<0)
	{ /* we ignore buffer-underruns */
		odelay=0;
	} else if (odelay==0)
	{ /* ALSA sometimes (atlast on Stians Ubuntu laptop) gives odelay==0 always */
		snd_pcm_sframes_t tmp1 = snd_pcm_status_get_avail_max(alsa_pcm_status);
		snd_pcm_sframes_t tmp2 = snd_pcm_status_get_avail(alsa_pcm_status);
		odelay = tmp1 - tmp2;

		debug_printf("      (no delay available) fallback:\n");
		debug_printf("      snd_pcm_status_get_avail_max() = %d\n", (int)tmp1);
		debug_printf("      snd_pcm_status_get_avail() = %d\n", (int)tmp2);
		debug_printf("      => %d\n", (int)odelay);

		if (odelay<0)
		{
			odelay=0;
		}
	}
	odelay<<=(bit16+stereo);

	if (odelay>kernlen)
	{
		odelay=kernlen;
	} else if ((odelay<kernlen))
	{
		kernlen=odelay;
		kernpos=(cachepos-kernlen+buflen)%buflen;
	}

	if (!cachelen)
	{
		busy--;
		return;
	}

	if (bufpos<=cachepos)
		n=buflen-cachepos;
	else
		n=bufpos-cachepos;

	/* TODO, check kernel-size
	if (n>info.bytes)
		n=info.bytes;
	*/

	if (n<=0)
	{
		busy--;
		return;
	}

	tmp = snd_pcm_status_get_avail(alsa_pcm_status);

	debug_printf ("      snd_pcm_status_get_avail() = %d\n", tmp);

	tmp = tmp << (bit16+stereo);

	if (n > tmp)
	{
		n = tmp;
	}
	result=snd_pcm_writei(alsa_pcm, playbuf+cachepos, n>>(bit16+stereo));

	debug_printf ("      snd_pcm_writei (%d) = %d\n", n>>(bit16+stereo), result);

#ifdef ALSA_DEBUG_OUTPUT
	if (result > 0)
	{
		write (debug_output, playbuf+cachepos, result<<(bit16+stereo));
	}
#endif
	if (result<0)
	{
		if (result==-EPIPE)
		{
			fprintf(stderr, "ALSA: Machine is too slow, calling snd_pcm_prepare()\n");
			snd_pcm_prepare(alsa_pcm); /* TODO, can this fail? */
			debug_printf ("      snd_pcm_prepare()\n");
		}
		busy--;
		return;
	}
	result<<=(bit16+stereo);
	cachepos=(cachepos+result+buflen)%buflen;
	playpos+=result;
	cachelen-=result;
	kernlen+=result;

	busy--;
}
/* stolen from devposs */
static void advance(unsigned int pos)
{
	debug_printf ("ALSA_advance: oldpos=%d newpos=%d add=%d (busy=%d)\n", bufpos, pos, (pos-bufpos+buflen)%buflen, busy);

	busy++;

	cachelen+=(pos-bufpos+buflen)%buflen;
	bufpos=pos;

	debug_printf ("         cachelen=%d kernlen=%d sum=%d len=%d\n", cachelen, kernlen, cachelen+kernlen, buflen);

	busy--;
}
/* stolen from devposs */
static uint32_t gettimer(void)
{
	long tmp=playpos;
	int odelay;

	if (busy++)
	{
		odelay=kernlen;
	} else {
		int err;

		debug_printf("ALSA_gettimer()");

		err=snd_pcm_status(alsa_pcm, alsa_pcm_status);

		debug_printf ("      snd_pcm_status(alsa_pcm, alsa_pcm_status) = %s\n", snd_strerror(-err));

		if (err<0)
		{
			fprintf(stderr, "ALSA: snd_pcm_status() failed: %s\n", snd_strerror(-err));
			odelay=kernlen;
		} else {

			odelay=snd_pcm_status_get_delay(alsa_pcm_status);
			debug_printf ("      snd_pcm_status_get_delay() = %d\n", odelay);

			if (odelay<0)
			{ /* we ignore buffer-underruns */
				odelay=0;
			} else if (odelay==0)
			{ /* ALSA sometimes (atlast on Stians Ubuntu laptop) gives odelay==0 always */
				snd_pcm_sframes_t tmp1 = snd_pcm_status_get_avail_max(alsa_pcm_status);
				snd_pcm_sframes_t tmp2 = snd_pcm_status_get_avail(alsa_pcm_status);
				odelay = tmp1 - tmp2;

				debug_printf("      (no delay available) fallback:\n");
				debug_printf("      snd_pcm_status_get_avail_max() = %d\n", (int)tmp1);
				debug_printf("      snd_pcm_status_get_avail() = %d\n", (int)tmp2);
				debug_printf("      => %d\n", (int)odelay);

				if (odelay<0)
				{
					odelay=0;
				}
			}

			odelay<<=(bit16+stereo);
			if (odelay>kernlen)
			{
				odelay=kernlen;
			} else if ((odelay<kernlen))
			{
				kernlen=odelay;
				kernpos=(cachepos-kernlen+buflen)%buflen;
			}
		}
	}

	tmp-=odelay;
	busy--;
	return imuldiv(tmp, 65536>>(stereo+bit16), plrRate);
}

static FILE *alsaSelectMixer(struct modlistentry *entry)
{
	char *t;
	int card;
	char *name;

	dirdbGetName_internalstr (entry->dirdbfullpath, &name);

	if (!strcmp(name, "default.dev"))
	{
		strcpy(alsaMixerName, "default");
	} else if (!strcmp(name, "none.dev"))
	{
		strcpy(alsaMixerName, "");
	} else if (!strcmp(name, "custom.dev"))
	{
		int mlTop=mlDrawBox();
		char str[DEVICE_NAME_MAX+1];
		unsigned int curpos;
		unsigned int cmdlen;
		int insmode=1;
		unsigned int scrolled=0;

		strcpy(str, alsaMixerName);

		displaystr(mlTop+1, 5, 0x0b, "Give me something to crunch!!", 29);
		displaystr(mlTop+3, 5, 0x0b, "-- Finish with enter, abort with escape --", 32);

		curpos=strlen(str);
		cmdlen=strlen(str);
		setcurshape(1);

		while (1)
		{
			uint16_t key;
			displaystr(mlTop+2, 5, 0x8f, str+scrolled, plScrWidth-10);
			setcur(mlTop+2, 5+curpos-scrolled);

			while (!ekbhit())
				framelock();
			key=egetch();

			if ((key>=0x20)&&(key<=0x7f))
			{
				if (insmode)
				{
					if ((cmdlen+1)<sizeof(str))
					{
						memmove(str+curpos+1, str+curpos, cmdlen-curpos+1);
						str[curpos++]=key;
						cmdlen++;
					}
				} else if (curpos==cmdlen)
				{
					if ((cmdlen+1)<(sizeof(str)))
					{
						str[curpos++]=key;
						str[curpos]=0;
						cmdlen++;
					}
				} else
					str[curpos++]=key;
			} else switch (key)
			{
			case KEY_ESC:
				setcurshape(0);
				return NULL;
			case KEY_LEFT:
				if (curpos)
					curpos--;
				break;
			case KEY_RIGHT:
				if (curpos<cmdlen)
					curpos++;
				break;
			case KEY_HOME:
				curpos=0;
				break;
			case KEY_END:
				curpos=cmdlen;
				break;
			case KEY_INSERT:
				{
					insmode=!insmode;
					setcurshape(insmode?1:2);
				}
				break;
			case KEY_DELETE:
				if (curpos!=cmdlen)
				{
					memmove(str+curpos, str+curpos+1, cmdlen-curpos);
					cmdlen--;
				}
				break;
			case KEY_BACKSPACE:
				if (curpos)
				{
					memmove(str+curpos-1, str+curpos, cmdlen-curpos+1);
					curpos--;
					cmdlen--;
				}
				break;
			case _KEY_ENTER:
				strcpy(alsaMixerName, str);
				setcurshape(0);
				goto out;
			}
			while ((curpos-scrolled)>=(plScrWidth-10))
				scrolled+=8;
			while (((signed)curpos-(signed)scrolled)<0)
				scrolled-=8;
		}
	} else {
		if (!(t=strchr(name, ':')))
			return NULL;
		card=atoi(t+1);
		snprintf(alsaMixerName, sizeof(alsaMixerName), "hw:%d", card);
	}
out:

	fprintf(stderr, "ALSA: Selected mixer %s\n", alsaMixerName);

	if (custom_mixer_mdb_ref!=0xffffffff)
	{
		struct moduleinfostruct mi;
		mdbGetModuleInfo(&mi, custom_mixer_mdb_ref);
		snprintf(mi.modname, sizeof(mi.modname), "%s", alsaMixerName);
		mdbWriteModuleInfo(custom_mixer_mdb_ref, &mi);
	}

	return NULL;
}

static FILE *alsaSelectPcmOut(struct modlistentry *entry)
{
	char *name;

	dirdbGetName_internalstr (entry->dirdbfullpath, &name);

	if (!strcmp(name, "custom.dev"))
	{
		int mlTop=mlDrawBox();
		char str[DEVICE_NAME_MAX+1];
		unsigned int curpos;
		unsigned int cmdlen;
		int insmode=1;
		unsigned int scrolled=0;

		strcpy(str, alsaCardName);

		displaystr(mlTop+1, 5, 0x0b, "Give me something to crunch!!", 29);
		displaystr(mlTop+3, 5, 0x0b, "-- Finish with enter, abort with escape --", 32);

		curpos=strlen(str);
		cmdlen=strlen(str);
		setcurshape(1);

		while (1)
		{
			uint16_t key;
			displaystr(mlTop+2, 5, 0x8f, str+scrolled, plScrWidth-10);
			setcur(mlTop+2, 5+curpos-scrolled);

			while (!ekbhit())
				framelock();
			key=egetch();

			if ((key>=0x20)&&(key<=0x7f))
			{
				if (insmode)
				{
					if ((cmdlen+1)<sizeof(str))
					{
						memmove(str+curpos+1, str+curpos, cmdlen-curpos+1);
						str[curpos++]=key;
						cmdlen++;
					}
				} else if (curpos==cmdlen)
				{
					if ((cmdlen+1)<(sizeof(str)))
					{
						str[curpos++]=key;
						str[curpos]=0;
						cmdlen++;
					}
				} else
					str[curpos++]=key;
			} else switch (key)
			{
			case KEY_ESC:
				setcurshape(0);
				return NULL;
			case KEY_LEFT:
				if (curpos)
					curpos--;
				break;
			case KEY_RIGHT:
				if (curpos<cmdlen)
					curpos++;
				break;
			case KEY_HOME:
				curpos=0;
				break;
			case KEY_END:
				curpos=cmdlen;
				break;
			case KEY_INSERT:
				{
					insmode=!insmode;
					setcurshape(insmode?1:2);
				}
				break;
			case KEY_DELETE:
				if (curpos!=cmdlen)
				{
					memmove(str+curpos, str+curpos+1, cmdlen-curpos);
					cmdlen--;
				}
				break;
			case KEY_BACKSPACE:
				if (curpos)
				{
					memmove(str+curpos-1, str+curpos, cmdlen-curpos+1);
					curpos--;
					cmdlen--;
				}
				break;
			case _KEY_ENTER:
				strcpy(alsaCardName, str);
				setcurshape(0);
				goto out;
			}
			while ((curpos-scrolled)>=(plScrWidth-10))
				scrolled+=8;
			while (((signed)curpos-(signed)scrolled)<0)
				scrolled-=8;
		}
	}
/* 1.0.14rc1 added support for snd_device_name_hint */
#if SND_LIB_VERSION >= 0x01000e
	else if (!strncmp(name, "ALSA-PCM-", 9))
	{
		int len = strlen (name);

		if (len < (9+4)) /* should not happen */
			return NULL;
		len -= (9+4);
		if (len > DEVICE_NAME_MAX) /* whoopise */
			return NULL;
		strncpy(alsaCardName, name+9, len);
		alsaCardName[len] = 0;

		strncpy(alsaMixerName, name+9, len);
		alsaMixerName[len] = 0;
	}
#else
	else if (!strcmp(name, "default.dev"))
	{
		strcpy(alsaCardName, "default");
	} else {
		char *t;
		int card, device;

		if (!(t=strchr(name, ':')))
			return NULL;
		card=atoi(t+1);
		if (!(t=strchr(name, ',')))
			return NULL;
		device=atoi(t+1);
		if (alsa_1_0_11_or_better)
			snprintf(alsaCardName, sizeof(alsaCardName), "hw:%d,%d", card, device);
		else
			snprintf(alsaCardName, sizeof(alsaCardName), "plughw:%d,%d", card, device);
		snprintf(alsaMixerName, sizeof(alsaMixerName), "hw:%d", card);
	}
#endif
out:

	fprintf(stderr, "ALSA: Selected PCM %s\n", alsaCardName);
	fprintf(stderr, "ALSA: Selected Mixer %s\n", alsaMixerName);


	if (custom_dsp_mdb_ref!=0xffffffff)
	{
		struct moduleinfostruct mi;
		mdbGetModuleInfo(&mi, custom_dsp_mdb_ref);
		snprintf(mi.modname, sizeof(mi.modname), "%s", alsaCardName);
		mdbWriteModuleInfo(custom_dsp_mdb_ref, &mi);
	}
	if (custom_mixer_mdb_ref!=0xffffffff)
	{
		struct moduleinfostruct mi;
		mdbGetModuleInfo(&mi, custom_mixer_mdb_ref);
		snprintf(mi.modname, sizeof(mi.modname), "%s", alsaMixerName);
		mdbWriteModuleInfo(custom_mixer_mdb_ref, &mi);
	}

	return NULL;
}

/* 1.0.14rc1 added support for snd_device_name_hint */
#if SND_LIB_VERSION >= 0x01000e
static int list_cards(struct modlist *ml, const struct dmDrive *drive, uint32_t parent, const char *mask, unsigned long opt, int mixercaponly)
{
	int result;
	int count = 1;
	int retval = 0;
	int i;
	void **hints = 0;

	debug_printf ("ALSA_list_cards (ml=%p, mixercaponly=%d)\n", ml, mixercaponly);

	result=snd_device_name_hint(-1, "pcm", &hints);
	debug_printf ("      snd_device_name_hint() = %s\n", snd_strerror(-result));

	if (result)
	{
		return retval; /* zero cards found */
	}

	for (i=0; hints[i]; i++)
	{
		char *name, *descr, *io;
		name = snd_device_name_get_hint(hints[i], "NAME");
		descr = snd_device_name_get_hint(hints[i], "DESC");
		io = snd_device_name_get_hint(hints[i], "IOID");

		debug_printf ("       [%d] name=%s\n", i, name?name:"(NULL)");
		debug_printf ("            desc=%s\n", descr?descr:"(NULL)");
		debug_printf ("            io=%s\n", io?io:"(IO)");

		if (!name) /* should never happen */
		{
			goto end;
		}
		if (io && (!strcmp(io, "Input"))) /* ignore input only cards */
		{
			goto end;
		}

		if (ml)
		{
			char namebuffer[128];
			struct modlistentry entry;
			memset(&entry, 0, sizeof(entry));
			snprintf(entry.shortname, sizeof (entry.shortname), "dev-%03d.dev", count);
			snprintf(namebuffer, sizeof(namebuffer), "ALSA-PCM-%s.dev", name);
			entry.drive=drive;
			entry.dirdbfullpath=dirdbFindAndRef(parent, namebuffer);
			entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
			entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
			if (entry.mdb_ref!=0xffffffff)
			{
				struct moduleinfostruct mi;
				mdbGetModuleInfo(&mi, entry.mdb_ref);
				mi.flags1&=~MDB_VIRTUAL;
				mi.channels=2;
				snprintf(mi.modname, sizeof (mi.modname), "%s", name);
				mi.composer[0] = 0;
				mi.comment[0] = 0;

				if (descr)
				{
					char *n = strchr (descr, '\n');
					if (n)
					{
						int len = n - descr + 1;
						if (len >= sizeof(mi.composer))
						{
							len = sizeof(mi.composer);
						}
						snprintf(mi.composer, len, "%s", descr);
						snprintf(mi.comment, sizeof (mi.comment), "%s", n + 1);
					} else {
						if (strlen(descr) > sizeof (mi.composer))
						{
							n = descr+strlen(descr)-1;
							while (n-descr > 0)
							{
								if ((*n) != ' ')
								{
									n--;
									continue;
								}
								if (n-descr >= sizeof(mi.composer))
								{
									n--;
									continue;
								}
								snprintf(mi.composer, n-descr+1, "%s", descr);
								snprintf(mi.comment, sizeof (mi.comment), "%s", n + 1);
								break;
							}
						} else {
							snprintf(mi.composer, sizeof(mi.composer), "%s", descr);
						}
					}
				}
				mi.modtype=mtDEVp;
				mdbWriteModuleInfo(entry.mdb_ref, &mi);
			}
			entry.adb_ref=0xffffffff;
			entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=mixercaponly?alsaSelectMixer:alsaSelectPcmOut;
			modlist_append(ml, &entry);
			dirdbUnref(entry.dirdbfullpath);
		}
		if (strcmp(name, "default"))
		{
			retval++;
		}
		count++;
end:
		if (name)
		{
			free (name);
		}
		if (descr)
		{
			free (descr);
		}
		if (io)
		{
			free (io);
		}
	}

	debug_printf ("     snd_device_name_free_hint()\n");

	snd_device_name_free_hint (hints);

	debug_printf (" => %d\n", retval);

	return retval;
}

#else
static int list_devices_for_card(int card, struct modlist *ml, const struct dmDrive *drive, const uint32_t parent, const char *mask, unsigned long opt)
{
	char dev[64];
#ifdef ALSA_DEBUG
	char *card_name = 0;
#endif
	int err;
	int pcm_device=-1;
	int cards=0;
	snd_ctl_t *ctl;

	snprintf(dev, sizeof(dev), "hw:%i", card);

	err=snd_ctl_open(&ctl, dev, 0);

	debug_printf("      snd_ctl_open(&ctl, \"%s\", 0) = %s\n", dev, snd_strerror(-err));

	if (err<0)
	{
		return cards;
	}

#ifdef ALSA_DEBUG
	debug_printf("      snd_card_get_name(%i, &card_name) = ", card);
	if ((err=snd_card_get_name(card, &card_name))!=0)
	{
		debug_printf("failed: %s\n", snd_strerror(-err));
	} else {
		debug_printf("ok, %s name: %s\n", dev, card_name);
		free (card_name);
		card_name = 0;
	}
#endif

	while(1)
	{
		err=snd_ctl_pcm_next_device(ctl, &pcm_device);
		debug_printf("    snd_ctl_pcm_next_device(ctl, &pcm_device (%i)) = %s\n", pcm_device, snd_strerror(-err));
		if (err<0)
		{
			pcm_device=-1;
		}
		if (pcm_device<0)
		{
			break;
		}

		snd_pcm_info_set_device(pcm_info, pcm_device);
		debug_printf("      snd_pcm_info_set_device(pcm_info, %i)\n", pcm_device);

		snd_pcm_info_set_subdevice(pcm_info, 0);
		debug_printf("      snd_pcm_info_set_subdevice(pcm_info, 0)\n");

		snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_PLAYBACK);

		debug_printf("      snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_PLAYBACK)\n");

		err=snd_ctl_pcm_info(ctl, pcm_info);
		debug_printf("      snd_ctl_pcm_info(ctl, pcm_info) = %s\n", snd_strerror(-err));
		if (err<0)
		{
			if (err!=-ENOENT)
			{
				fprintf(stderr, "ALSA: snd_device_from_card(): snd_ctl_pcm_info(%d:%d) failed: %s\n", card, pcm_device, snd_strerror(-err));
			}
			continue;
		}

#ifdef ALSA_DEBUG
		card_name = (char *)snd_pcm_info_get_name (pcm_info);
		fprintf(stderr, "  => hw:%d,%d: name %s\n", card, pcm_device, card_name);
		card_name = 0;
#endif
		if (ml)
		{
			struct modlistentry entry;
			memset(&entry, 0, sizeof(entry));

			snprintf(entry.shortname, sizeof(entry.shortname), "hw:%d,%d.dev", card, pcm_device);
			entry.drive=drive;
			entry.dirdbfullpath=dirdbFindAndRef(parent, entry.shortname);
			entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
			entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
			if (entry.mdb_ref!=0xffffffff)
			{
				struct moduleinfostruct mi;
				mdbGetModuleInfo(&mi, entry.mdb_ref);
				mi.flags1&=~MDB_VIRTUAL;
				mi.channels=2;
				snprintf(mi.modname, sizeof(mi.modname), "%s", snd_pcm_info_get_name(pcm_info));
				mi.modtype=mtDEVp;
				mdbWriteModuleInfo(entry.mdb_ref, &mi);
			}
			entry.adb_ref=0xffffffff;
			entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=alsaSelectPcmOut;
			modlist_append(ml, &entry);
			dirdbUnref(entry.dirdbfullpath);
		}
		cards++;
	}

	snd_ctl_close(ctl);
	debug_printf("      snd_ctl_close(ctl)\n");
	return cards;
}

static int list_cards(struct modlist *ml, const struct dmDrive *drive, uint32_t parent, const char *mask, unsigned long opt, int mixercaponly)
{
	int card=-1;
	int result;
	int cards=0;

	if (ml)
	{
		struct modlistentry entry;
		memset(&entry, 0, sizeof(entry));

		snprintf(entry.shortname, sizeof(entry.shortname), "default.dev");
		entry.drive=drive;
		entry.dirdbfullpath=dirdbFindAndRef(parent, entry.shortname);
		entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
		entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
		if (entry.mdb_ref!=0xffffffff)
		{
			struct moduleinfostruct mi;
			mdbGetModuleInfo(&mi, entry.mdb_ref);
			mi.flags1&=~MDB_VIRTUAL;
			mi.channels=2;
			snprintf(mi.modname, sizeof(mi.modname), "default output");
			mi.modtype=mtDEVp;
			mdbWriteModuleInfo(entry.mdb_ref, &mi);
		}
		entry.adb_ref=0xffffffff;
		entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=mixercaponly?alsaSelectMixer:alsaSelectPcmOut;
		modlist_append(ml, &entry);
		dirdbUnref(entry.dirdbfullpath);
		if (mixercaponly) {
			snprintf(entry.shortname, sizeof(entry.shortname), "none.dev");
			entry.drive=drive;
			entry.dirdbfullpath=dirdbFindAndRef(parent, entry.shortname);
			entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
			entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
			if (entry.mdb_ref!=0xffffffff)
			{
				struct moduleinfostruct mi;
				mdbGetModuleInfo(&mi, entry.mdb_ref);
				mi.flags1&=~MDB_VIRTUAL;
				mi.channels=2;
				mi.modname[0]=0;
				mi.modtype=mtDEVp;
				mdbWriteModuleInfo(entry.mdb_ref, &mi);
			}
			entry.adb_ref=0xffffffff;
			entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=alsaSelectMixer;
			modlist_append(ml, &entry);
			dirdbUnref(entry.dirdbfullpath);

		}
		snprintf(entry.shortname, sizeof(entry.shortname), "custom.dev");
		entry.drive=drive;
		entry.dirdbfullpath=dirdbFindAndRef(parent, entry.shortname);
		entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
		if (mixercaponly)
			custom_mixer_mdb_ref=entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
		else
			custom_dsp_mdb_ref=entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
		if (entry.mdb_ref!=0xffffffff)
		{
			struct moduleinfostruct mi;
			mdbGetModuleInfo(&mi, entry.mdb_ref);
			mi.flags1&=~MDB_VIRTUAL;
			mi.channels=2;
			snprintf(mi.modname, sizeof(mi.modname), "%s", mixercaponly?alsaMixerName:alsaCardName);
			mi.modtype=mtDEVp;
			mdbWriteModuleInfo(entry.mdb_ref, &mi);
		}
		entry.adb_ref=0xffffffff;
		entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=mixercaponly?alsaSelectMixer:alsaSelectPcmOut;
		modlist_append(ml, &entry);
		dirdbUnref(entry.dirdbfullpath);

	}

	result=snd_card_next(&card);
	debug_printf("      snd_card_next(&card (%i)) = %s\n", card, snd_strerror(-result));

	if (result)
	{
		return cards;
	}

	while (card>-1)
	{

		if (mixercaponly)
		{
			if (ml)
			{
				struct modlistentry entry;
				memset(&entry, 0, sizeof(entry));
				char *card_name = 0;
				int err;

				err=snd_card_get_name(card, &card_name);
				debug_printf ("      snd_card_get_name() = %s\n", snd_strerror(-err));
				if (err)
				{
					card_name=strdup("Unknown card");
				}

				snprintf(entry.shortname, sizeof(entry.shortname), "hw:%d.dev", card);
				entry.drive=drive;
				entry.dirdbfullpath=dirdbFindAndRef(parent, entry.shortname);
				entry.flags=MODLIST_FLAG_FILE|MODLIST_FLAG_VIRTUAL;
				entry.mdb_ref=mdbGetModuleReference(entry.shortname, 0);
				if (entry.mdb_ref!=0xffffffff)
				{
					struct moduleinfostruct mi;
					mdbGetModuleInfo(&mi, entry.mdb_ref);
					mi.flags1&=~MDB_VIRTUAL;
					mi.channels=2;
					snprintf(mi.modname, sizeof(mi.modname), "%s", card_name);
					mi.modtype=mtDEVp;
					mdbWriteModuleInfo(entry.mdb_ref, &mi);
				}
				entry.adb_ref=0xffffffff;
				entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=alsaSelectMixer;
				modlist_append(ml, &entry);
				dirdbUnref(entry.dirdbfullpath);
				free (card_name);
				card_name = 0;
			}
		} else {
			cards+=list_devices_for_card(card, ml, drive, parent, mask, opt);
		}

		result=snd_card_next(&card);
		debug_printf(stderr, "      snd_card_next(&card (%i)) = ", card);

		if (result)
		{
			return cards;
		}
	}
	return cards;
}
#endif

/* plr API start */

static void SetOptions(unsigned int rate, int opt)
{
	int err;
	snd_pcm_format_t format;
	unsigned int val;
	/* start with setting default values, if we bail out */
	plrRate=rate;
	plrOpt=opt;

	alsaOpenDevice();
	if (!alsa_pcm)
		return;

	debug_printf ("ALSA_SetOptions (rate=%d, opt=0x%x%s%s)\n", rate, opt, (opt&PLR_16BIT)?" 16bit":" 8bit", (opt&PLR_SIGNEDOUT)?" signed":" unsigned");

	err=snd_pcm_hw_params_any(alsa_pcm, hwparams);
	debug_printf("      snd_pcm_hw_params_any(alsa_pcm, hwparams) = %s\n", snd_strerror(err<0?-err:0));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params_any() failed: %s\n", snd_strerror(-err));
		return;
	}

	err=snd_pcm_hw_params_set_access(alsa_pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	debug_printf("      snd_pcm_hw_params_set_access(alsa_pcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) = %s\n", snd_strerror(-err));
	if (err)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params_set_access() failed: %s\n", snd_strerror(-err));
		return;
	}

	if (opt&PLR_16BIT)
	{
		if (opt&PLR_SIGNEDOUT)
		{
			format=SND_PCM_FORMAT_S16;
		} else {
			format=SND_PCM_FORMAT_U16;
		}
	} else {
		if (opt&PLR_SIGNEDOUT)
		{
			format=SND_PCM_FORMAT_S8;
		} else {
			format=SND_PCM_FORMAT_U8;
		}
	}

	err=snd_pcm_hw_params_set_format(alsa_pcm, hwparams, format);
	debug_printf("      snd_pcm_hw_params_set_format(alsa_pcm, hwparams, format %i) = %s\n", format, snd_strerror(-err));
	if (err)
	{
		err=snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_S16);
		debug_printf("      snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_S16) = %s\n", snd_strerror(-err));
		if (err==0)
		{
			opt|=PLR_16BIT|PLR_SIGNEDOUT;
		} else {
			err=snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_U16);
			debug_printf("      snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_U16) = %s\n", snd_strerror(-err));
			if (err==0)
			{
				opt&=~(PLR_16BIT|PLR_SIGNEDOUT);
				opt|=PLR_16BIT;
			} else {
				err=snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_S8);
				debug_printf("      snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_S8) = %s\n", snd_strerror(-err));
				if (err==0)
				{
					opt&=~(PLR_16BIT|PLR_SIGNEDOUT);
					opt|=PLR_SIGNEDOUT;
				} else
				{
					err=snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_U8);
					debug_printf("      snd_pcm_hw_params_set_format(alsa_pcm, hwparams, SND_PCM_FORMAT_U8) = %s\n", snd_strerror(-err));
					if (err==0)
					{
						opt&=~(PLR_16BIT|PLR_SIGNEDOUT);
					} else {
						fprintf(stderr, "ALSA: snd_pcm_hw_params_set_format() failed: %s\n", snd_strerror(-err));
						return;
					}
				}
			}
		}
	}

	bit16=!!(opt&PLR_16BIT);
	if (opt&PLR_STEREO)
	{
		val=2;
	} else {
		val=1;
	}

	err=snd_pcm_hw_params_set_channels_near(alsa_pcm, hwparams, &val);
	debug_printf("      snd_pcm_hw_params_set_channels_near(alsa_pcm, hwparams, &channels=%i) = %s\n", val, snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params_set_channels_near() failed: %s\n", snd_strerror(-err));
		return;
	}
	if (val==1)
	{
		stereo=0;
		opt&=~PLR_STEREO;
	} else if (val==2)
	{
		stereo=1;
		opt|=PLR_STEREO;
	} else {
		fprintf(stderr, "ALSA: snd_pcm_hw_params_set_channels_near() gave us %d channels\n", val);
		return;
	}

	err=snd_pcm_hw_params_set_rate_near(alsa_pcm, hwparams, &rate, 0);
	debug_printf("      snd_pcm_hw_params_set_rate_near(alsa_pcm, hwparams, &rate = %i, 0) = %s\n", rate, snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params_set_rate_near() failed: %s\n", snd_strerror(-err));
		return;
	}
	if (rate==0)
	{
		fprintf(stderr, "ALSA: No usable samplerate available.\n");
		return;
	}

	val = 500000;
	err=snd_pcm_hw_params_set_buffer_time_near(alsa_pcm, hwparams, &val, 0);
	fprintf(stderr, "ALSA snd_pcm_hw_params_set_buffer_time_near(alsa_pcm, hwparams, 500000 uS => %d, 0) = %s\n", val, snd_strerror(-err));
	if (err)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params_set_buffer_time_near() failed: %s\n", snd_strerror(-err));
		return;
	}

	err=snd_pcm_hw_params(alsa_pcm, hwparams);
	debug_printf("      snd_pcm_hw_params(alsa_pcm, hwparams) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_hw_params() failed: %s\n", snd_strerror(-err));
		return;
	}

	err=snd_pcm_sw_params_current(alsa_pcm, swparams);
	debug_printf("       snd_pcm_sw_params_current(alsa_pcm, swparams) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_sw_params_any() failed: %s\n", snd_strerror(-err));
		return;
	}

	err=snd_pcm_sw_params(alsa_pcm, swparams);
	fprintf(stderr, "ALSA snd_pcm_sw_params(alsa_pcm, swparams) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_pcm_sw_params() failed: %s\n", snd_strerror(-err));
		return;
	}
	plrRate=rate;
	plrOpt=opt;
}

#ifdef PLR_DEBUG
static char *alsaDebug(void)
{
	static char buffer[100];
	strcpy(buffer, "devpalsa: ");
	convnum(cachelen, buffer+9, 10, 5, 1);
	strcat(buffer, "/");
	convnum(kernlen, buffer+15, 10, 5, 1);
	strcat(buffer, "/");
	convnum(buflen, buffer+21, 10, 5, 1);
	return buffer;
}
#endif

static int alsaPlay(void **buf, unsigned int *len)
{
	if (!alsa_pcm)
		return 0;

	if ((*len)<(plrRate&~3))
		*len=plrRate&~3;
	if ((*len)>(plrRate*4))
		*len=plrRate*4;
	playbuf=*buf=malloc(*len);

	memsetd(*buf, (plrOpt&PLR_SIGNEDOUT)?0:(plrOpt&PLR_16BIT)?0x80008000:0x80808080, (*len)>>2);

	buflen=*len;
	bufpos=0;
	cachepos=0;
	cachelen=0;
	playpos=0;
	kernpos=0;
	kernlen=0;

	plrGetBufPos=getbufpos;
	plrGetPlayPos=getplaypos;
	plrIdle=flush;
	plrAdvanceTo=advance;
	plrGetTimer=gettimer;
#ifdef PLR_DEBUG
	plrDebug=alsaDebug;
#endif

#ifdef ALSA_DEBUG_OUTPUT
	debug_output = open ("test-alsa.raw", O_WRONLY | O_TRUNC | O_CREAT, S_IRUSR | S_IWUSR);
#endif

	return 1;
}

static void alsaStop(void)
{
#ifdef PLR_DEBUG
	plrDebug=0;
#endif
	free(playbuf);

#ifdef ALSA_DEBUG_OUTPUT
	close (debug_output);
	debug_output = -1;
#endif

}

/* plr API stop */


/* driver front API start */

static void alsaOpenDevice(void)
{
	int err;
	snd_mixer_elem_t *current;

	alsa_mixers_n=0;

	/* if any device already open, please close it */
	debug_printf ("alsaOpenDevice()\n");
	if (alsa_pcm)
	{
		err=snd_pcm_drain(alsa_pcm);
		debug_printf("      snd_pcm_drain(alsa_pcm) = %s\n", snd_strerror(-err));

		err=snd_pcm_close(alsa_pcm);
		debug_printf("      snd_pcm_close(alsa_pcm) = %s\n", snd_strerror(-err));
		alsa_pcm=NULL;
	}

	if (mixer)
	{
		err=snd_mixer_close(mixer);
		fprintf(stderr, "      snd_mixer_close(mixer) = %s\n", snd_strerror(-err));
		mixer=NULL;
	}

	/* open PCM device */
	err=snd_pcm_open(&alsa_pcm, alsaCardName, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	debug_printf("      snd_pcm_open(&alsa_pcm, device = \"%s\", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) = %s\n", alsaCardName, snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: failed to open pcm device (%s): %s\n", alsaCardName, snd_strerror(-err));
		alsa_pcm=NULL;
		return;
	}

	/* Any mixer to open ? */
	if (!strlen(alsaMixerName))
		return;

	err=snd_mixer_open(&mixer, 0);
	debug_printf("      snd_mixer_open(&mixer, 0) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_mixer_open() failed: %s\n", snd_strerror(-err));
		return;
	}

	err=snd_mixer_attach(mixer, alsaMixerName);
	debug_printf("      snd_mixer_attach(mixer, device = \"%s\") = %s\n", alsaMixerName, snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_mixer_attach() failed: %s\n", snd_strerror(-err));
		err=snd_mixer_close(mixer);
		debug_printf("      snd_mixer_close(mixer) = %s\n", snd_strerror(-err));
		mixer=NULL;
		return;
	}

	err=snd_mixer_selem_register(mixer, NULL, NULL);
	debug_printf("      snd_mixer_selem_register(mixer, NULL, NULL) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_mixer_selem_register() failed: %s\n", snd_strerror(-err));

		err=snd_mixer_close(mixer);
		debug_printf("      snd_mixer_close(mixer) = %s\n", snd_strerror(-err));
		mixer=NULL;
		return;
	}

	err=snd_mixer_load(mixer);
	debug_printf("      snd_mixer_load(mixer) = %s\n", snd_strerror(-err));
	if (err<0)
	{
		fprintf(stderr, "ALSA: snd_mixer_load() failed: %s\n", snd_strerror(-err));

		err=snd_mixer_close(mixer);
		debug_printf("      snd_mixer_close(mixer) = %s\n", snd_strerror(-err));
		mixer=NULL;
		return;
	}

	current = snd_mixer_first_elem(mixer);
	debug_printf ("      snd_mixer_first_elem(mixer) = %p\n", current);
	while (current)
	{
		debug_printf ("        snd_mixer_selem_is_active(current) = %d\n", snd_mixer_selem_is_active(current));
		debug_printf ("        snd_mixer_selem_has_playback_volume = %d\n", snd_mixer_selem_has_playback_volume(current));
		if (snd_mixer_selem_is_active(current) &&
			snd_mixer_selem_has_playback_volume(current) &&
			(alsa_mixers_n<MAX_ALSA_MIXER))
		{
			long int a, b;
			long int min, max;
			snd_mixer_selem_get_playback_volume(current, SND_MIXER_SCHN_FRONT_LEFT, &a);
			snd_mixer_selem_get_playback_volume(current, SND_MIXER_SCHN_FRONT_RIGHT, &b);
			mixer_entries[alsa_mixers_n].val=(a+b)>>1;
			snd_mixer_selem_get_playback_volume_range(current, &min, &max);
			mixer_entries[alsa_mixers_n].min=min;
			mixer_entries[alsa_mixers_n].max=max;
			mixer_entries[alsa_mixers_n].step=1;
			mixer_entries[alsa_mixers_n].log=0;
			mixer_entries[alsa_mixers_n].name=snd_mixer_selem_get_name(current);

			debug_printf ("          name=%s\n", mixer_entries[alsa_mixers_n].name);
			debug_printf ("          SND_MIXER_SCHN_FRONT_LEFT  = %ld\n", a);
			debug_printf ("          SND_MIXER_SCHN_FRONT_RIGHT = %ld\n", b);
			debug_printf ("          min=%ld max=%ld\n", min, max);

			alsa_mixers_n++;
		}
		current = snd_mixer_elem_next(current);
		debug_printf ("      snd_mixer_elem_next(current) = %p\n", current);
	}
}

static int volalsaGetNumVolume(void)
{
	return alsa_mixers_n;
}

static int volalsaGetVolume(struct ocpvolstruct *v, int n)
{
	if (n<alsa_mixers_n)
	{
		memcpy(v, &mixer_entries[n], sizeof(mixer_entries[n]));
		return 1;
	}
	return 0;
}

static int volalsaSetVolume(struct ocpvolstruct *v, int n)
{
	int count=0, err;
	snd_mixer_elem_t *current;

	debug_printf ("volalsaSetVolume(v->val=%d n=%d)\n", v->val, n);

	current = snd_mixer_first_elem(mixer);
	while (current)
	{
		if (snd_mixer_selem_is_active(current) &&
			snd_mixer_selem_has_playback_volume(current))
		{
			if (count==n)
			{
				err = snd_mixer_selem_set_playback_volume(current, SND_MIXER_SCHN_FRONT_LEFT, v->val);
				debug_printf ("      snd_mixer_selem_set_playback_volume(current %s, SND_MIXER_SCHN_FRONT_LEFT,  %d) = %s\n", snd_mixer_selem_get_name (current), v->val, snd_strerror(-err));
				err = snd_mixer_selem_set_playback_volume(current, SND_MIXER_SCHN_FRONT_RIGHT, v->val);
				debug_printf ("      snd_mixer_selem_set_playback_volume(current %s, SND_MIXER_SCHN_FRONT_RIGHT, %d) = %s\n", snd_mixer_selem_get_name (current), v->val, snd_strerror(-err));
				mixer_entries[n].val=v->val;
				return 1;
			}
			count++;
		}
		current = snd_mixer_elem_next(current);
	}
	return 0;
}

static int alsaInit(const struct deviceinfo *c)
{
/* 1.0.14rc1 added support for snd_device_name_hint */
#if SND_LIB_VERSION < 0x01000e
	{
		const char *version = snd_asoundlib_version ();
		int major = 0;
		int minor = 0;
		int patch = 0;

		major = atoi(version);
		version = strchr(version, '.');
		if (version)
		{
			version++;
			minor = atoi(version);
			version = strchr(version, '.');
			if (version)
			{
				version++;
				patch = atoi(version);
			}
		}
		if (major > 255)
			major = 255;
		if (minor > 255)
			minor = 255;
		if (patch > 255)
			patch = 255;
		alsa_1_0_11_or_better = ((major << 16) | (minor << 8) | patch) >= 0x01000b;
	}
#endif
/*
	memcpy(&currentcard, card, sizeof(struct deviceinfo));*/
	dmSETUP=RegisterDrive("setup:");

	plrSetOptions=SetOptions;
	plrPlay=alsaPlay;
	plrStop=alsaStop;

	alsaOpenDevice();
	if (!alsa_pcm)
		return 0;

	SetOptions(44100, PLR_16BIT|PLR_STEREO);
	return 1;
}

static int alsaDetect(struct deviceinfo *card)
{
	int cards = list_cards(NULL, NULL, DIRDB_NOPARENT, NULL, 0, 0);
	card->devtype=&plrAlsa;
	snprintf(card->path, sizeof(card->path), "%s", cfGetProfileString("devpALSA", "path", "default"));
	snprintf(alsaCardName, sizeof(alsaCardName), "%s", card->path);
	snprintf(card->mixer, sizeof(card->mixer), "%s", cfGetProfileString("devpALSA", "mixer", "default"));
	snprintf(alsaMixerName, sizeof(alsaMixerName), "%s", card->mixer);
/*
	card->irq=-1;
	card->irq2=-1;
	card->dma=-1;
	card->dma2=-1;*/
	card->subtype=-1;
	card->mem=0;
	card->chan=2;

	return cards>0;
}

static void alsaClose(void)
{
	plrPlay=0;
}

/* driver font API stop */

static int alsaReadDir(struct modlist *ml, const struct dmDrive *drive, const uint32_t path, const char *mask, unsigned long opt)
{
	struct modlistentry entry;
	uint32_t dmalsa;

	if (drive!=dmSETUP)
		return 1;

	memset(&entry, 0, sizeof(entry));

	dmalsa=dirdbFindAndRef(dmSETUP->basepath, "ALSA");

	if (path==dmSETUP->basepath)
	{
		strcpy(entry.shortname, "ALSA");
		entry.drive=drive;
		entry.dirdbfullpath=dmalsa;
		entry.flags=MODLIST_FLAG_DIR;
		entry.mdb_ref=0xffffffff;
		entry.adb_ref=0xffffffff;
		entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=0;
		modlist_append(ml, &entry);
	} else {
		uint32_t dmpcmout,dmmixer;
		dmpcmout=dirdbFindAndRef(dmalsa, "PCM.OUT");
		dmmixer=dirdbFindAndRef(dmalsa, "MIXER");
		if (path==dmalsa)
		{
			strcpy(entry.shortname, "PCM.OUT");
			entry.drive=drive;
			entry.dirdbfullpath=dmpcmout;
			entry.flags=MODLIST_FLAG_DIR;
			entry.mdb_ref=0xffffffff;
			entry.adb_ref=0xffffffff;
			entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=0;
			modlist_append(ml, &entry);

			strcpy(entry.shortname, "MIXER");
			entry.drive=drive;
			entry.dirdbfullpath=dmmixer;
			entry.flags=MODLIST_FLAG_DIR;
			entry.mdb_ref=0xffffffff;
			entry.adb_ref=0xffffffff;
			entry.Read=0; entry.ReadHeader=0; entry.ReadHandle=0;
			modlist_append(ml, &entry);

		} else if (path==dmpcmout)
			list_cards(ml, drive, path, mask, opt, 0);
		else if (path==dmmixer)
			list_cards(ml, drive, path, mask, opt, 1);
		dirdbUnref(dmpcmout);
		dirdbUnref(dmmixer);
	}
	dirdbUnref(dmalsa);
	return 1;
}

static void __attribute__((constructor))init(void)
{
	int err;
	mdbRegisterReadDir(&readdirAlsa);
	if ((err = snd_pcm_status_malloc(&alsa_pcm_status)))
	{
		fprintf(stderr, "snd_pcm_status_malloc() failed, %s\n", snd_strerror(-err));
		exit(0);
	}
#if SND_LIB_VERSION < 0x01000e
	if ((err = snd_pcm_info_malloc(&pcm_info)))
	{
		fprintf(stderr, "snd_pcm_info_malloc() failed, %s\n", snd_strerror(-err));
		exit(0);
	}
#endif
	if ((err = snd_pcm_hw_params_malloc(&hwparams)))
	{
		fprintf(stderr, "snd_pcm_hw_params_malloc failed, %s\n", snd_strerror(-err));
		exit(0);
	}
	if ((err = snd_pcm_sw_params_malloc(&swparams)))
	{
		fprintf(stderr, "snd_pcm_sw_params_malloc failed, %s\n", snd_strerror(-err));
		exit(0);
	}
}

static void __attribute__((destructor))fini(void)
{
	int err;

	mdbUnregisterReadDir(&readdirAlsa);

	debug_printf("ALSA_fini()\n");

	if (alsa_pcm)
	{
		err=snd_pcm_drain(alsa_pcm);
		debug_printf("      snd_pcm_drain(alsa_pcm) = %s\n", snd_strerror(-err));

		err=snd_pcm_close(alsa_pcm);
		debug_printf("      snd_pcm_close(alsa_pcm) = %s\n", snd_strerror(-err));

		alsa_pcm=NULL;
	}

	if (mixer)
	{
		err=snd_mixer_close(mixer);
		debug_printf("      snd_mixer_close(mixer) = %s\n", snd_strerror(-err));
		mixer=NULL;
	}

	if (alsa_pcm_status)
	{
		snd_pcm_status_free(alsa_pcm_status);
		alsa_pcm_status = NULL;
	}

#if SND_LIB_VERSION < 0x01000e
	if (pcm_info)
	{
		snd_pcm_info_free(pcm_info);
		pcm_info = NULL;
	}
#endif

	if (hwparams)
	{
		snd_pcm_hw_params_free(hwparams);
		hwparams=NULL;
	}

	if (swparams)
	{
		snd_pcm_sw_params_free(swparams);
		swparams=NULL;
	}

	snd_config_update_free_global ();

	alsa_mixers_n=0;
}

struct sounddevice plrAlsa={SS_PLAYER, 1, "ALSA device driver", alsaDetect, alsaInit, alsaClose, NULL};
struct ocpvolregstruct volalsa={volalsaGetNumVolume, volalsaGetVolume, volalsaSetVolume};
static struct mdbreaddirregstruct readdirAlsa = {alsaReadDir MDBREADDIRREGSTRUCT_TAIL};

char *dllinfo="driver plrAlsa; volregs volalsa";
struct linkinfostruct dllextinfo = {.name = "devpalsa", .desc = "OpenCP Player Device: ALSA (c) 2005-10 Stian Skjelstad", .ver = DLLVERSION, .size = 0};
