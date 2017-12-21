/*
THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.  
COPYRIGHT 1993-1998 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/
/*
 * $Source: f:/miner/source/main/rcs/config.c $
 * $Revision: 2.2 $
 * $Author: john $
 * $Date: 1995/03/27 09:42:59 $
 * 
 * contains routine(s) to read in the configuration file which contains
 * game configuration stuff like detail level, sound card, etc
 * 
 * $Log: config.c $
 * Revision 2.2  1995/03/27  09:42:59  john
 * Added VR Settings in config file.
 * 
 * Revision 2.1  1995/03/16  11:20:40  john
 * Put in support for Crystal Lake soundcard.
 * 
 * Revision 2.0  1995/02/27  11:30:13  john
 * New version 2.0, which has no anonymous unions, builds with
 * Watcom 10.0, and doesn't require parsing BITMAPS.TBL.
 * 
 * Revision 1.14  1995/02/11  16:19:36  john
 * Added code to make the default mission be the one last played.
 * 
 * Revision 1.13  1995/01/18  13:23:24  matt
 * Made curtom detail level vars initialize properly at load
 * 
 * Revision 1.12  1995/01/04  22:15:36  matt
 * Fixed stupid bug using scanf() to read bytes
 * 
 * Revision 1.11  1995/01/04  13:14:21  matt
 * Made custom detail level settings save in config file
 * 
 * Revision 1.10  1994/12/12  21:35:09  john
 * *** empty log message ***
 * 
 * Revision 1.9  1994/12/12  21:31:51  john
 * Made volume work better by making sure volumes are valid
 * and set correctly at program startup.
 * 
 * Revision 1.8  1994/12/12  13:58:01  john
 * MAde -nomusic work.
 * Fixed GUS hang at exit by deinitializing digi before midi.
 * 
 * Revision 1.7  1994/12/08  10:01:33  john
 * Changed the way the player callsign stuff works.
 * 
 * Revision 1.6  1994/12/01  11:24:07  john
 * Made volume/gamma/joystick sliders all be the same length.  0-->8.
 * 
 * Revision 1.5  1994/11/29  02:01:07  john
 * Added code to look at -volume command line arg.
 * 
 * Revision 1.4  1994/11/14  20:14:11  john
 * Fixed some warnings.
 * 
 * Revision 1.3  1994/11/14  19:51:01  john
 * Added joystick cal values to descent.cfg.
 * 
 * Revision 1.2  1994/11/14  17:53:09  allender
 * read and write descent.cfg file
 * 
 * Revision 1.1  1994/11/14  16:28:08  allender
 * Initial revision
 * 
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "types.h"
#include "game.h"
#include "digi.h"
#include "kconfig.h"
#include "palette.h"
#include "joy.h"
#include "args.h"
#include "player.h"
#include "mission.h"
/*
#pragma off (unreferenced)
static char rcsid[] = "$Id: config.c 2.2 1995/03/27 09:42:59 john Exp $";
#pragma on (unreferenced)
*/
static char *digi_volume_str = "DigiVolume";
static char *midi_volume_str = "MidiVolume";
static char *master_volume_str = "MasterVolume";
static char *detail_level_str = "DetailLevel";
static char *gamma_level_str = "GammaLevel";
static char *stereo_rev_str = "StereoReverse";
static char *last_player_str = "LastPlayer";
static char *last_mission_str = "LastMission";

char config_last_player[CALLSIGN_LEN+1] = "";
char config_last_mission[MISSION_NAME_LEN+1] = "";

int Config_digi_type = 0;
int Config_midi_type = 0;

int Config_vr_type = 0;
int Config_vr_tracking = 0;

ubyte Config_master_volume = 4;

extern byte	Object_complexity, Object_detail, Wall_detail, Wall_render_depth, Debris_amount, SoundChannels;

void set_custom_detail_vars(void);

int ReadConfigFile()
{
	FILE *infile;
	char line[80], *token, *value, *ptr;
	ubyte gamma;
	int joy_axis_min[4];
	int joy_axis_center[4];
	int joy_axis_max[4];

	strcpy( config_last_player, "" );

	joy_axis_min[0] = joy_axis_min[1] = joy_axis_min[2] = joy_axis_min[3] = 0;
	joy_axis_max[0] = joy_axis_max[1] = joy_axis_max[2] = joy_axis_max[3] = 0;
	joy_axis_center[0] = joy_axis_center[1] = joy_axis_center[2] = joy_axis_center[3] = 0;
	joy_set_cal_vals(joy_axis_min, joy_axis_center, joy_axis_max);

	Config_digi_volume = 4;
	Config_midi_volume = 4;
	Config_master_volume = 4;
	Config_control_type = 0;
	Config_channels_reversed = 0;

	infile = fopen("descent.cfg", "rt");
	if (infile == NULL) {
		return 1;
	}
	while (!feof(infile)) {
		memset(line, 0, 80);
		fgets(line, 80, infile);
		ptr = &(line[0]);
		while (isspace(*ptr))
			ptr++;
		if (*ptr != '\0') {
			token = strtok(ptr, "=");
			value = strtok(NULL, "=");
			if (!strcmp(token, digi_volume_str))
				Config_digi_volume = strtol(value, NULL, 10);
			else if (!strcmp(token, midi_volume_str))
				Config_midi_volume = strtol(value, NULL, 10);
			else if (!strcmp(token, master_volume_str))
				Config_master_volume = strtol(value, NULL, 10);
			else if (!strcmp(token, stereo_rev_str))
				Config_channels_reversed = strtol(value, NULL, 10);
			else if (!strcmp(token, gamma_level_str)) {
				gamma = strtol(value, NULL, 10);
				gr_palette_set_gamma( gamma );
			}
			else if (!strcmp(token, detail_level_str)) {
				Detail_level = strtol(value, NULL, 10);
				if (Detail_level == NUM_DETAIL_LEVELS-1) {
					int count,dummy,oc,od,wd,wrd,da,sc;

					count = sscanf (value, "%d,%d,%d,%d,%d,%d,%d\n",&dummy,&oc,&od,&wd,&wrd,&da,&sc);

					if (count == 7) {
						Object_complexity = oc;
						Object_detail = od;
						Wall_detail = wd;
						Wall_render_depth = wrd;
						Debris_amount = da;
						SoundChannels = sc;
						set_custom_detail_vars();
					}
				}
			}
			else if (!strcmp(token, last_player_str))	{
				char * p;
				strncpy( config_last_player, value, CALLSIGN_LEN );
				p = strchr( config_last_player, '\n');
				if ( p ) *p = 0;
			}
			else if (!strcmp(token, last_mission_str))	{
				char * p;
				strncpy( config_last_mission, value, MISSION_NAME_LEN );
				p = strchr( config_last_mission, '\n');
				if ( p ) *p = 0;
			}
		}
	}

	fclose(infile);
/*
	i = FindArg( "-volume" );
	
	if ( i > 0 )	{
		i = atoi( Args[i+1] );
		if ( i < 0 ) i = 0;
		if ( i > 100 ) i = 100;
		Config_digi_volume = (i*8)/100;
		Config_midi_volume = (i*8)/100;
	}
*/
	if ( Config_digi_volume > 8 ) Config_digi_volume = 8;
	if ( Config_midi_volume > 8 ) Config_midi_volume = 8;
	if ( Config_master_volume > 8 ) Config_master_volume = 8;

	digi_set_volume( (Config_digi_volume*127)/8, (Config_midi_volume*255)/8 );
	digi_set_master_volume( Config_master_volume );

	return 0;
}

int WriteConfigFile()
{
	FILE *infile;
	char str[256];
	ubyte gamma = gr_palette_get_gamma();

	infile = fopen("descent.cfg", "wt");
	if (infile == NULL) {
		return 1;
	}
	sprintf (str, "%s=%d\n", digi_volume_str, Config_digi_volume);
	fputs(str, infile);
	sprintf (str, "%s=%d\n", midi_volume_str, Config_midi_volume);
	fputs(str, infile);
	sprintf (str, "%s=%d\n", master_volume_str, Config_master_volume);
	fputs(str, infile);
	sprintf (str, "%s=%d\n", stereo_rev_str, Config_channels_reversed);
	fputs(str, infile);
	sprintf (str, "%s=%d\n", gamma_level_str, gamma);
	fputs(str, infile);
	if (Detail_level == NUM_DETAIL_LEVELS-1)
		sprintf (str, "%s=%d,%d,%d,%d,%d,%d,%d\n", detail_level_str, Detail_level,
				Object_complexity,Object_detail,Wall_detail,Wall_render_depth,Debris_amount,SoundChannels);
	else
		sprintf (str, "%s=%d\n", detail_level_str, Detail_level);
	fputs(str, infile);
	sprintf (str, "%s=%s\n", last_player_str, Players[Player_num].callsign );
	fputs(str, infile);
	sprintf (str, "%s=%s\n", last_mission_str, config_last_mission );
	fputs(str, infile);
	fclose(infile);
	return 0;
}		
