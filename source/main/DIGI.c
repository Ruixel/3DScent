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

#include <stdio.h>
#include <stdlib.h>

#include "ndsw.h"

#include "inferno.h"
#include "songs.h"
#include "screens.h"
#include "game.h"
#include "player.h"
#include "cfile.h"
#include "segpoint.h"
#include "ai.h"
#include "piggy.h"
#include "wall.h"
#include "joy.h"
#include "mem.h"
#include "sounds.h"
#include "mono.h"
#include "error.h"
#include "kconfig.h"
#include "newdemo.h"

#include "ndsfunc.h"
//#include "sound.h"

#define _DIGI_MAX_VOLUME			127

int digi_initialized = 0;
ubyte digi_paused = 0;
int midi_volume = 255;
int digi_volume = _DIGI_MAX_VOLUME;
int digi_midi_song_playing = 0;
int digi_last_midi_song = 0;
int digi_last_midi_song_loop = 0;

extern ubyte Config_master_volume;

// sound object stuff -- used for fans and the boss only??
#define SOF_USED			1 		// Set if this sample is used
#define SOF_PLAYING			2		// Set if this sample is playing on a channel
#define SOF_LINK_TO_OBJ		4		// Sound is linked to a moving object. If object dies, then finishes play and quits.
#define SOF_LINK_TO_POS		8		// Sound is linked to segment, pos
#define SOF_PLAY_FOREVER	16		// Play forever (or until level is stopped), otherwise plays once

#define SOUND_PLAYING			1
#define SOUND_OBJECT_PLAYING	2

typedef struct song_resource {
	short		midi_id;
	ubyte		lead_inst;
	ubyte		buffer_ahead;
	ushort		tempo;
	ushort		pitch_shift;
	ubyte		sound_voices;
	ubyte		max_notes;
	ushort		norm_voices;
} song_resource;

typedef struct sound_object {
	short			signature;		// A unique signature to this sound
	ubyte			flags;			// Used to tell if this slot is used and/or currently playing, and how long.
	fix				max_volume;		// Max volume that this sound is playing at
	fix				max_distance;	// The max distance that this sound can be heard at...
	int				volume;			// Volume that this sound is playing at
	int				pan;				// Pan value that this sound is playing at
	short			handle;			// What handle this sound is playing on.  Valid only if SOF_PLAYING is set.
	short			soundnum;		// The sound number that is playing
	union {	
		struct {
			short			segnum;				// Used if SOF_LINK_TO_POS field is used
			short			sidenum;				
			vms_vector		position;
		} pos;
		struct {
			short			objnum;				// Used if SOF_LINK_TO_OBJ field is used
			short			objsignature;
		} obj;
	} link_type;
} sound_object;

#define MAX_SOUND_OBJECTS 16
sound_object SoundObjects[MAX_SOUND_OBJECTS];
short next_signature=0;

void channel_start_sound (int c, int sndnum, int volume, int pan, int loop)
{
	digi_sound	*snd;

	if (c < 0)
		return;

	snd = &GameSounds[sndnum];
	if (!snd->data)
		piggy_sound_page_in (sndnum);

	//Snd_StartChannel (c, (u32)snd->data, snd->length >> 3, SOUND_FREQ (11025), volume, 0, (pan + 256) >> 2, 2, loop, 4);
}

void channel_stop (int c)
{
	if (c < 0)
		return;

	//Snd_StopChannel (c);
}

void channel_change_volume (int c, int volume)
{

	if (c < 0)
		return;

	//Snd_ChangeVolume (c, volume);
}

void channel_change_pan (int c, int pan)
{
	if (c < 0)
		return;

	//Snd_ChangePan (c, (pan + 256) >> 2);
}

void digi_reset_digi_sounds()
{
	if ( !digi_initialized )
		return;

	//Snd_StopAllChannels ();
}

int digi_xlat_sound( int soundno )
{
	if ( soundno < 0 ) return -1;

	if (Sounds[soundno] == 255)
		return -1;

	return (int)(Sounds[soundno]);
}

void digi_play_sample_3d( int sndnum, int angle, int volume, int no_dups )
{
	int vol;
	int i = sndnum, demo_angle, c;

	if (Newdemo_state == ND_STATE_RECORDING) {
		demo_angle = fixmuldiv(angle, F1_0, 255);
		if (no_dups)
			newdemo_record_sound_3d_once(sndnum, demo_angle, volume);
		else
			newdemo_record_sound_3d(sndnum, demo_angle, volume);
	}
	if (!digi_initialized) return;
	if (digi_paused) {
		digi_resume_all();
		if (digi_paused)
			return;
	}
	if ( sndnum < 0 ) return;
	i = digi_xlat_sound(sndnum);
	if (i == -1) return;

	vol = fixmuldiv(volume, digi_volume, F1_0);

	c = -1;//Snd_GetFreeChannel ();
	if (c == -1)
		return;
	channel_start_sound (c, i, vol, angle, 0);
}

void digi_play_sample( int sndnum, fix max_volume )
{
	int i, c, vol;

	if (Newdemo_state == ND_STATE_RECORDING)
		newdemo_record_sound(sndnum);

	if (!digi_initialized) return;
	if (digi_paused) {
		digi_resume_all();
		if (digi_paused)
			return;
	}
	if ( sndnum < 0 ) return;

	i = digi_xlat_sound(sndnum);
	if (i == -1) return;

	vol = fixmuldiv(max_volume, digi_volume, F1_0);

	c = -1;//Snd_GetFreeChannel ();
	if (c == -1)
		return;
	channel_start_sound (c, i, vol, 0, 0);
}

void digi_play_sample_once( int sndnum, fix max_volume )
{
	digi_play_sample( sndnum, F1_0 );
}

void digi_set_digi_volume(int dvolume)
{
	if ( !digi_initialized ) return;

	dvolume = dvolume * 127 / 8;
	if ( dvolume > _DIGI_MAX_VOLUME )
		digi_volume = _DIGI_MAX_VOLUME;
	else if ( dvolume < 0 )
		digi_volume = 0;
	else
		digi_volume = dvolume;

	digi_sync_sounds();
}

void digi_stop_current_song()
{
	digi_midi_song_playing = 0;
}

void digi_play_midi_song(int songnum, int loop )
{
//	song_resource *song;

	if (!digi_initialized) return;

	digi_last_midi_song = songnum;
	digi_last_midi_song_loop = loop;

	digi_stop_current_song();
	
	if (midi_volume < 1)
		return;

	digi_midi_song_playing = 1;
}

void digi_set_midi_volume(int n)
{
	int old_volume = midi_volume;
	
	if (!digi_initialized) return;

	if (n < 0)
		midi_volume = 0;
	else if (n > 255)
		midi_volume = 255;
	else
		midi_volume = n;
		
	if (!old_volume && midi_volume) {
		digi_play_midi_song(digi_last_midi_song, digi_last_midi_song_loop);
	} else if (old_volume && !midi_volume) {
		digi_stop_current_song();
	}
}

void digi_set_volume(int dvolume, int mvolume)
{
	if (!digi_initialized) return;
	digi_set_digi_volume(dvolume);
	digi_set_midi_volume(mvolume);
}

void digi_set_master_volume( int volume )
{
	if (!digi_initialized) return;

	if ( volume > 8 )
		Config_master_volume = 8;
	else if ( volume < 0 )
		Config_master_volume = 0;
	else
		Config_master_volume = volume;	

	//Snd_ChangeMasterVolume (volume * _DIGI_MAX_VOLUME / 8);
}

void digi_reset()
{
}

void digi_resume_all()
{
	if (!digi_initialized) return;

	if (digi_paused) {
		digi_paused = 0;
	}
}

void digi_pause_all()
{
	int i;
	sound_object	*sndobj;
	
	if (!digi_initialized) return;

	if (!digi_paused) {	
		for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
		{
			if ( (sndobj->flags & SOF_USED ) && (sndobj->flags & SOF_PLAYING) && (sndobj->flags && SOF_PLAY_FOREVER) ) {
					channel_stop (sndobj->handle);
				sndobj->flags &= ~SOF_PLAYING;		// Mark sound as not playing
			}
		}
		digi_paused = 1;
	}
}

void digi_stop_all()
{
	int i;
	sound_object	*sndobj;

	if (!digi_initialized)	return;

	//Snd_StopAllChannels ();

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
		sndobj->flags = 0;
}

void digi_set_max_channels(int n)
{
}

void digi_start_sound_object(int i)
{
	int		c;
	sound_object	*sndobj;

	if (!digi_initialized) return;

	c = -1;//Snd_GetFreeChannel ();
	if (c == -1)
		return;

	sndobj = &SoundObjects[i];
	channel_start_sound (c, sndobj->soundnum, fixmuldiv (sndobj->volume, digi_volume, F1_0), sndobj->pan, 1);
	sndobj->flags |= SOF_PLAYING;
	sndobj->handle = c;
}

void digi_get_sound_loc( vms_matrix * listener, vms_vector * listener_pos, int listener_seg, vms_vector * sound_pos, int sound_seg, fix max_volume, int *volume, int *pan, fix max_distance )
{	  
	vms_vector	vector_to_sound;
	fix angle_from_ear, cosang,sinang;
	fix distance;
	fix path_distance;

	*volume = 0;
	*pan = 0;

	if (!digi_initialized) return;
	
	max_distance = (max_distance*5)/4;		// Make all sounds travel 1.25 times as far.

	//	Warning: Made the vm_vec_normalized_dir be vm_vec_normalized_dir_quick and got illegal values to acos in the fang computation.
	distance = vm_vec_normalized_dir_quick( &vector_to_sound, sound_pos, listener_pos );
		
	if (distance < max_distance )	{
		int num_search_segs = f2i(max_distance/20);
		if ( num_search_segs < 1 ) num_search_segs = 1;

		path_distance = find_connected_distance(listener_pos, listener_seg, sound_pos, sound_seg, num_search_segs, WID_RENDPAST_FLAG );
		//path_distance = distance;
		if ( path_distance > -1 )	{
			*volume = max_volume - (path_distance/f2i(max_distance));
			//mprintf( (0, "Sound path distance %.2f, volume is %d / %d\n", f2fl(distance), *volume, max_volume ));
			if (*volume > 0 )	{
				angle_from_ear = vm_vec_delta_ang_norm(&listener->rvec,&vector_to_sound,&listener->uvec);
				fix_sincos(angle_from_ear,&sinang,&cosang);
				//mprintf( (0, "volume is %.2f\n", f2fl(*volume) ));
				if (Config_channels_reversed) cosang *= -1;
				*pan = fixmuldiv(cosang, 255, F1_0);
			} else {
				*volume = 0;
			}
		}
	}																					  
}

int digi_link_sound_to_object( int soundnum, short objnum, int forever, fix max_volume )
{																									// 10 segs away
	return digi_link_sound_to_object2( soundnum, objnum, forever, max_volume, 256*F1_0  );
}

int digi_link_sound_to_pos( int soundnum, short segnum, short sidenum, vms_vector * pos, int forever, fix max_volume )
{
	return digi_link_sound_to_pos2( soundnum, segnum, sidenum, pos, forever, max_volume, F1_0 * 256 );
}

int digi_link_sound_to_object2( int org_soundnum, short objnum, int forever, fix max_volume, fix  max_distance )
{
	int i,volume,pan;
	object * objp;
	int soundnum;
	sound_object	*sndobj;

	soundnum = digi_xlat_sound(org_soundnum);
	
	if (!digi_initialized) return -1;
	if ( max_volume < 0 ) return -1;
	if (soundnum < 0 ) return -1;
	if ((objnum<0)||(objnum>Highest_object_index))
		return -1;

	objp = &Objects[objnum];
	if ( !forever )	{
		// Hack to keep sounds from building up...
		digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
			               &objp->pos, objp->segnum, max_volume,
						   &volume, &pan, max_distance );
		digi_play_sample_3d( org_soundnum, pan, volume, 0 );
		return -1;
	}

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
	{
		if (sndobj->flags==0)
			break;
	}
	
	if (i==MAX_SOUND_OBJECTS) {
		mprintf((1, "Too many sound objects!\n" ));
		return -1;
	}

	sndobj->signature=next_signature++;
	sndobj->flags = SOF_USED | SOF_LINK_TO_OBJ;
	if ( forever )
		sndobj->flags |= SOF_PLAY_FOREVER;
	sndobj->link_type.obj.objnum = objnum;
	sndobj->link_type.obj.objsignature = objp->signature;
	sndobj->max_volume = max_volume;
	sndobj->max_distance = max_distance;
	sndobj->volume = 0;
	sndobj->pan = 0;
	sndobj->soundnum = (short)soundnum;

	digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
                       &objp->pos, objp->segnum, sndobj->max_volume,
                       &sndobj->volume, &sndobj->pan, sndobj->max_distance );

	if ( sndobj->volume > 0)
		digi_start_sound_object(i);

	return sndobj->signature;
}

int digi_link_sound_to_pos2( int org_soundnum, short segnum, short sidenum, vms_vector * pos, int forever, fix max_volume, fix max_distance )
{
	int i, volume, pan;
	int soundnum; 
	sound_object	*sndobj;

	soundnum = digi_xlat_sound(org_soundnum);

	if (!digi_initialized) return -1;
	if ( max_volume < 0 ) return -1;

	if (soundnum < 0 ) return -1;
	if ((segnum<0)||(segnum>Highest_segment_index))
		return -1;

	if ( !forever )	{
		// Hack to keep sounds from building up...
		digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, pos, segnum, max_volume, &volume, &pan, max_distance );
		digi_play_sample_3d( org_soundnum, pan, volume, 0 );
		return -1;
	}

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
	{
		if (sndobj->flags==0)
			break;
	}
	
	if (i==MAX_SOUND_OBJECTS) {
		mprintf((1, "Too many sound objects!\n" ));
		return -1;
	}

	sndobj->signature=next_signature++;
	sndobj->flags = SOF_USED | SOF_LINK_TO_POS;
	if ( forever )
		sndobj->flags |= SOF_PLAY_FOREVER;
	sndobj->link_type.pos.segnum = segnum;
	sndobj->link_type.pos.sidenum = sidenum;
	sndobj->link_type.pos.position = *pos;
	sndobj->soundnum = (short)soundnum;
	sndobj->max_volume = max_volume;
	sndobj->max_distance = max_distance;
	sndobj->volume = 0;
	sndobj->pan = 0;
	digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
                       &sndobj->link_type.pos.position, sndobj->link_type.pos.segnum, sndobj->max_volume,
                       &sndobj->volume, &sndobj->pan, sndobj->max_distance );
	
	if ( sndobj->volume > 0)
		digi_start_sound_object(i);

	return sndobj->signature;
}

void digi_sync_sounds()
{
	int i;
	int oldvolume, oldpan;
	sound_object	*sndobj;

	if (!digi_initialized) return;

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
	{
		if ( sndobj->flags & SOF_USED )	{
			oldvolume = sndobj->volume;
			oldpan = sndobj->pan;
/* never happens
			if ( !(sndobj->flags & SOF_PLAY_FOREVER) )	{
			 	// Check if its done.
				if (sndobj->flags & SOF_PLAYING) {
					if ( channel_done_playing (sndobj->handle) ) {
						sndobj->flags = 0;	// Mark as dead, so some other sound can use this sound
						continue;		// Go on to next sound...
					}
				}
			}			
*/		
			if ( sndobj->flags & SOF_LINK_TO_POS )	{
				digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
                                &sndobj->link_type.pos.position, sndobj->link_type.pos.segnum, sndobj->max_volume,
                                &sndobj->volume, &sndobj->pan, sndobj->max_distance );

			} else if ( sndobj->flags & SOF_LINK_TO_OBJ )	{
				object * objp;
	
				objp = &Objects[sndobj->link_type.obj.objnum];
		
				if ((objp->type==OBJ_NONE) || (objp->signature!=sndobj->link_type.obj.objsignature))	{
					// The object that this is linked to is dead, so just end this sound if it is looping.
					if ( (sndobj->flags & SOF_PLAYING)  && (sndobj->flags & SOF_PLAY_FOREVER))	{
						channel_stop (sndobj->handle);
					}
					sndobj->flags = 0;	// Mark as dead, so some other sound can use this sound
					continue;		// Go on to next sound...
				} else {
					digi_get_sound_loc( &Viewer->orient, &Viewer->pos, Viewer->segnum, 
	                                &objp->pos, objp->segnum, sndobj->max_volume,
                                   &sndobj->volume, &sndobj->pan, sndobj->max_distance );
				}
			}

			if (oldvolume != sndobj->volume) 	{
				if ( sndobj->volume < 1 )	{
					// Sound is too far away, so stop it from playing.
					if ((sndobj->flags & SOF_PLAYING)&&(sndobj->flags & SOF_PLAY_FOREVER))	{
						channel_stop (sndobj->handle);
						sndobj->flags &= ~SOF_PLAYING;		// Mark sound as not playing
						continue;
					}
				} else {
					if (!(sndobj->flags & SOF_PLAYING))	{
						digi_start_sound_object(i);
					} else {
						channel_change_volume (sndobj->handle, fixmuldiv(sndobj->volume, digi_volume,F1_0));
//						ChangeSoundVolume(SoundObjects[i].soundnum, fixmuldiv(SoundObjects[i].volume,digi_volume,F1_0) );
					}
				}
			}
				
			if (oldpan != sndobj->pan) 	{
				if (sndobj->flags & SOF_PLAYING) {
					channel_change_pan (sndobj->handle, sndobj->pan);
//					ChangeSoundStereoPosition( SoundObjects[i].soundnum, SoundObjects[i].pan );
				}
			}
		}
	}
}

void digi_kill_sound_linked_to_segment( int segnum, int sidenum, int soundnum )
{
	int i,killed;
	sound_object	*sndobj;

	soundnum = digi_xlat_sound(soundnum);

	if (!digi_initialized) return;

	killed = 0;

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
	{
		if ( (sndobj->flags & SOF_USED) && (sndobj->flags & SOF_LINK_TO_POS) )	{
			if ((sndobj->link_type.pos.segnum == segnum) && (sndobj->soundnum==(short)soundnum ) && (sndobj->link_type.pos.sidenum==sidenum) )	{
				if ( sndobj->flags & SOF_PLAYING )	{
					channel_stop (sndobj->handle);
				}
				sndobj->flags = 0;	// Mark as dead, so some other sound can use this sound
				killed++;
			}
		}
	}
	// If this assert happens, it means that there were 2 sounds
	// that got deleted. Weird, get John.
	if ( killed > 1 )	{
		mprintf( (1, "ERROR: More than 1 sounds were deleted from seg %d\n", segnum ));
	}
}

void digi_kill_sound_linked_to_object( int objnum )
{
	int i,killed;
	sound_object	*sndobj;

	if (!digi_initialized) return;

	killed = 0;

	for (i=0, sndobj=SoundObjects; i<MAX_SOUND_OBJECTS; i++ , sndobj++)
	{
		if ( (sndobj->flags & SOF_USED) && (sndobj->flags & SOF_LINK_TO_OBJ ) )	{
			if (sndobj->link_type.obj.objnum == objnum)	{
				if ( sndobj->flags & SOF_PLAYING )	{
					channel_stop (sndobj->handle);
				}
				sndobj->flags = 0;	// Mark as dead, so some other sound can use this sound
				killed++;
			}
		}
	}
	// If this assert happens, it means that there were 2 sounds
	// that got deleted. Weird, get John.
	if ( killed > 1 )	{
		mprintf( (1, "ERROR: More than 1 sounds were deleted from object %d\n", objnum ));
	}
}
/*
void digi_close()
{
	if (!digi_initialized)
		return;
	digi_stop_current_song();

	digi_initialized = 0;
}
*/
void digi_init_sounds()
{
	if (!digi_initialized)
		return;
	//Snd_StopAllChannels ();
}

int digi_init()
{
 	digi_initialized = 1;

	digi_set_master_volume(Config_master_volume);
	digi_set_volume(digi_volume, midi_volume);
	digi_reset_digi_sounds();
//	atexit(digi_close);

	return 0;
}
