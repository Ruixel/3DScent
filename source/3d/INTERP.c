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
 * $Source: Smoke:miner:source:3d::RCS:interp.c $
 * $Revision: 1.4 $
 * $Author: allender $
 * $Date: 1995/10/10 22:20:09 $
 * 
 * Polygon object interpreter
 * 
 * 3DS port modification: model vertices come from the model file in
 * object-local space. The original NDS port relied on hardware matrix-stack
 * transform to convert these to camera space. The 3DS renderer expects
 * camera-space vectors at g3_draw_tmap_tex / g3_draw_poly, so we transform
 * here using View_position and View_matrix (which already incorporate any
 * active g3_start_instance_matrix transform).
 */

#include <malloc.h>
#include <string.h>

#include "error.h"

#include "3d.h"
#include "globvars.h"
#include "gr.h"

#include "polyobj.h"
#include "mem.h"

#define OP_EOF			0	//eof
#define OP_DEFPOINTS	1	//defpoints
#define OP_FLATPOLY		2	//flat-shaded polygon
#define OP_TMAPPOLY		3	//texture-mapped polygon
#define OP_SORTNORM		4	//sort by normal
#define OP_RODBM		5	//rod bitmap
#define OP_SUBCALL		6	//call a subobject
#define OP_DEFP_START	7	//defpoints with start
#define OP_GLOW			8	//glow value for next poly

#define MAX_POINTS_PER_POLY		25

#define w(p)  (*((short *) (p)))
#define wp(p)  ((short *) (p))
#define vp(p)  ((vms_vector *) (p))

vms_angvec zero_angles = {0,0,0};

vms_vector  *list[1000];
vms_vector	*point_list[MAX_POINTS_PER_POLY];

int glow_num = -1;

// -----------------------------------------------------------------------------
// 3DS port: per-poly scratch buffer for object-space -> camera-space transform.
//
// Model vertices in the .pof model files are stored in object-local space.
// The renderer downstream (g3_draw_tmap_tex, g3_draw_poly) expects camera-
// space coordinates. We transform on the fly here using the same math
// g3_rotate_point uses, since View_matrix and View_position already
// incorporate any active g3_start_instance_matrix transform.
// -----------------------------------------------------------------------------
static vms_vector  Model_xformed_pts [MAX_POINTS_PER_POLY];
static vms_vector* Model_xformed_ptrs[MAX_POINTS_PER_POLY];

static inline void xform_model_pts(int nv, vms_vector** obj_pts)
{
	int i;
	for (i = 0; i < nv; i++)
	{
		vms_vector tempv;
		vm_vec_sub   (&tempv, obj_pts[i], &View_position);
		vm_vec_rotate(&Model_xformed_pts[i], &tempv, &View_matrix);
		Model_xformed_ptrs[i] = &Model_xformed_pts[i];
	}
}

//calls the object interpreter to render an object.  The object renderer
//is really a seperate pipeline. returns true if drew
ITCM_CODE bool g3_draw_polygon_model(void *model_ptr,grs_bitmap **model_bitmaps,vms_angvec *anim_angles,fix model_light,fix *glow_values)
{
	ubyte *p = model_ptr;

	glow_num = -1;		//glow off by default

	while (w(p) != OP_EOF)
	{
		switch (w(p))
		{
			case OP_DEFPOINTS: {
				int n = w(p+2);
				int	i;
				vms_vector	**l = list;

				p += 4;
				for (i=0; i<n; i++)
				{
					*l++ = vp(p);
					p += 12;
				}
				break;
			}

			case OP_DEFP_START: {
				int n = w(p+2);
				int s = w(p+4);
				vms_vector	**l = list + s;
				int	i;

				p += 8;
				for (i=0; i<n; i++)
				{
					*l++ = vp(p);
					p += 12;
				}
				break;
			}

			case OP_FLATPOLY: {
				int nv = w(p+2);
				int i;

				Assert( nv < MAX_POINTS_PER_POLY );

				gr_setcolor(w(p+28));

				for (i=0;i<nv;i++)
					point_list[i] = list[wp(p+30)[i]];

				// 3DS port: transform object-space verts to camera space
				xform_model_pts(nv, point_list);
				g3_draw_poly(nv, Model_xformed_ptrs);

				p += 30 + ((nv&~1)+1)*2;
				break;
			}

			case OP_TMAPPOLY: {
				int nv = w(p+2);
				g3s_uvl *uvl_list;
				int i;
				fix light;

				Assert( nv < MAX_POINTS_PER_POLY );

				//calculate light from surface normal
				if (glow_num < 0) {			//no glow
					light = -vm_vec_dot(&View_matrix.fvec,vp(p+16));
					light = f1_0/4 + (light*3)/4;
					light = fixmul(light,model_light);
				}
				else {				//yes glow
					light = glow_values[glow_num];
					glow_num = -1;
				}

				//now poke light into l values
				uvl_list = (g3s_uvl *) (p+30+((nv&~1)+1)*2);

				for (i=0;i<nv;i++)
				{
					point_list[i] = list[wp(p+30)[i]];
					uvl_list[i].l = light;
				}

				// 3DS port: transform object-space verts to camera space
				xform_model_pts(nv, point_list);
        // In OP_TMAPPOLY case, right before the g3_draw_tmap_func call:
				g3_draw_tmap_func(nv, Model_xformed_ptrs, uvl_list,
				                  model_bitmaps[w(p+28)]);

				p += 30 + ((nv&~1)+1)*2 + nv*12;
				break;
			}

			case OP_SORTNORM:
				g3_draw_polygon_model(p+w(p+30),model_bitmaps,anim_angles,model_light,glow_values);
				g3_draw_polygon_model(p+w(p+28),model_bitmaps,anim_angles,model_light,glow_values);

				p += 32;
				break;


			case OP_RODBM: {
				// NOTE: rods (laser bolts, energy beams) still pass raw
				// object-space vectors. g3_draw_rod_tmap is a separate
				// codepath we haven't audited yet. If rods render in the
				// wrong place, this is the spot to fix.
				g3_draw_rod_tmap(model_bitmaps[w(p+2)],vp(p+4),w(p+16),vp(p+20),w(p+32),f1_0);

				p+=36;
				break;
			}

			case OP_SUBCALL: {
				vms_angvec *a;

				if (anim_angles)
					a = &anim_angles[w(p+2)];
				else
					a = &zero_angles;

				g3_start_instance_angles(vp(p+4),a);

				g3_draw_polygon_model(p+w(p+16),model_bitmaps,anim_angles,model_light,glow_values);

				g3_done_instance();

				p += 20;
				break;

			}

			case OP_GLOW:
				if (glow_values)
					glow_num = w(p+2);

				p += 4;
				break;
		}
	}
	return 1;
}

#ifndef NDEBUG
int nest_count;
#endif

//alternate interpreter for morphing object
ITCM_CODE bool g3_draw_morphing_model(void *model_ptr,grs_bitmap **model_bitmaps,vms_angvec *anim_angles,fix model_light,vms_vector *new_points)
{
	ubyte *p = model_ptr;
	fix *glow_values = NULL;

	glow_num = -1;		//glow off by default

	while (w(p) != OP_EOF)
	{
		switch (w(p))
		{

			case OP_DEFPOINTS: {
				int n = w(p+2);
				int	i;
				vms_vector	**l = list;

				for (i=0; i<n; i++)
					*l++ = &new_points[i];

				p += n*sizeof(struct vms_vector) + 4;
				break;
			}

			case OP_DEFP_START: {
				int n = w(p+2);
				int s = w(p+4);
				int	i;
				vms_vector	**l = list + s;

				for (i=0; i<n; i++)
					*l++ = &new_points[i];

				p += n*sizeof (struct vms_vector) + 8;
				break;
			}

			case OP_FLATPOLY: {
				int nv = w(p+2);
				int i,ntris;

				gr_setcolor(w(p+28));
				
				for (i=0;i<2;i++)
					point_list[i] = list[wp(p+30)[i]];

				for (ntris=nv-2;ntris;ntris--) {
					point_list[2] = list[wp(p+30)[i++]];

					// 3DS port: transform object-space verts to camera space.
					// Each tri gets its own transform pass since point_list
					// changes between iterations.
					xform_model_pts(3, point_list);
					g3_draw_poly(3, Model_xformed_ptrs);

					point_list[1] = point_list[2];
				}

				p += 30 + ((nv&~1)+1)*2;
				break;
			}

			case OP_TMAPPOLY: {
				int nv = w(p+2);
				g3s_uvl *uvl_list;
				g3s_uvl morph_uvls[3];
				int i,ntris;
				fix light;

				//calculate light from surface normal

				if (glow_num < 0) {			//no glow
					light = -vm_vec_dot(&View_matrix.fvec,vp(p+16));
					light = f1_0/4 + (light*3)/4;
					light = fixmul(light,model_light);
				}
				else {				//yes glow
					light = glow_values[glow_num];
					glow_num = -1;
				}

				//now poke light into l values

				uvl_list = (g3s_uvl *) (p+30+((nv&~1)+1)*2);

				for (i=0;i<3;i++)
					morph_uvls[i].l = light;

				for (i=0;i<2;i++) {
					point_list[i] = list[wp(p+30)[i]];

					morph_uvls[i].u = uvl_list[i].u;
					morph_uvls[i].v = uvl_list[i].v;
				}

				for (ntris=nv-2;ntris;ntris--) {
					point_list[2] = list[wp(p+30)[i]];
					morph_uvls[2].u = uvl_list[i].u;
					morph_uvls[2].v = uvl_list[i].v;
					i++;

					// 3DS port: transform object-space verts to camera space.
					// NOTE: original called g3_draw_tmap_tex with uvl_list,
					// but morph_uvls was computed and unused - looks like a
					// pre-existing bug. Preserving original behavior.
					xform_model_pts(3, point_list);
					g3_draw_tmap_tex(3, Model_xformed_ptrs, uvl_list,
					                 model_bitmaps[w(p+28)]);

					point_list[1] = point_list[2];
					morph_uvls[1].u = morph_uvls[2].u;
					morph_uvls[1].v = morph_uvls[2].v;
				}

				p += 30 + ((nv&~1)+1)*2 + nv*12;
				break;
			}

			case OP_SORTNORM:
				g3_draw_morphing_model(p+w(p+30),model_bitmaps,anim_angles,model_light,new_points);
				g3_draw_morphing_model(p+w(p+28),model_bitmaps,anim_angles,model_light,new_points);

				p += 32;
				break;


			case OP_RODBM: {
				// See note in g3_draw_polygon_model about rods.
				g3_draw_rod_tmap(model_bitmaps[w(p+2)],vp(p+4),w(p+16),vp(p+20),w(p+32),f1_0);

				p+=36;
				break;
			}

			case OP_SUBCALL: {
				vms_angvec *a;

				if (anim_angles)
					a = &anim_angles[w(p+2)];
				else
					a = &zero_angles;

				g3_start_instance_angles(vp(p+4),a);

				g3_draw_polygon_model(p+w(p+16),model_bitmaps,anim_angles,model_light,glow_values);

				g3_done_instance();

				p += 20;
				break;
			}

			case OP_GLOW:
				if (glow_values)
					glow_num = w(p+2);

				p += 4;
				break;
		}
	}
	return 1;
}

int		*refs, num_refs;
int		*refs_ptr;
int		*eofs, num_eofs;

void polygon_model_fix_align_walk (polymodel *pm, int idx)
{
	ubyte	*p, *s, *d;
	int		i, j, shift;
	ubyte	*newmodel;

	while (idx < pm->model_data_size)
	{
		p = pm->model_data + idx;
		switch (w(p))
		{
		case OP_DEFPOINTS:	{
			int n = w(p + 2);
			idx += n * sizeof(struct vms_vector) + 4;
			break;
		}

		case OP_DEFP_START:	{
			int n = w(p + 2);
			idx += n * sizeof(struct vms_vector) + 8;
			break;
		}

		case OP_FLATPOLY:	{
			int nv = w(p + 2);
			idx += 30 + ((nv & ~ 1) + 1) * 2;					
			break;
		}

		case OP_TMAPPOLY:	{
			int nv = w(p + 2);
			idx += 30 + ((nv & ~1) + 1) * 2 + nv * 12;
			break;
			}

		case OP_SORTNORM:
			refs_ptr[num_refs] = idx + w(p + 28);
			refs[num_refs++] = idx + 28;
			refs_ptr[num_refs] = idx + w(p + 30);
			refs[num_refs++] = idx + 30;

			idx += 32;
			break;

		case OP_RODBM:
			idx += 36;
			break;

		case OP_SUBCALL:
			refs_ptr[num_refs] = idx + w(p + 16);
			refs[num_refs++] = idx + 16;
			idx += 20;
			break;

		case OP_GLOW:
			idx += 4;
			break;

		case OP_EOF:
			eofs[num_eofs++] = idx;
			idx += 2;
			break;

		default:
			Int3 ();
		}
	}

	for (i=0; i<num_refs; i++)
	{
		idx = refs[i];
		for (j=0; j<num_eofs; j++)
		{
			if (idx < eofs[j])
				break;
		}

		shift = 0;
		idx = refs_ptr[i];
		for (; j<num_eofs; j++)
		{
			if (idx > eofs[j])
			{
				shift += 2;
			}
			else
				break;
		}
		*(short *)(pm->model_data + refs[i]) += shift;
	}

	newmodel = d = malloc (pm->model_data_size + (num_eofs - 1) * 2);
	s = pm->model_data;
	j = eofs[0] + 2;
	memcpy (d, s, j);
	d += j + 2;
	s += j;
	for (j=0; j<MAX_SUBMODELS; j++)
	{
		if (pm->submodel_ptrs[j] > eofs[0])
			pm->submodel_ptrs[j] += 2;
	}
	for (i=1; i<num_eofs-1; i++)
	{
		j = eofs[i] - eofs[i - 1];
		memcpy (d, s, j);
		d += j + 2;
		s += j;
		for (j=0; j<MAX_SUBMODELS; j++)
		{
			if (pm->submodel_ptrs[j] > eofs[i])
				pm->submodel_ptrs[j] += 2;
		}
	}
	free (pm->model_data);
	pm->model_data = newmodel;
}

void polygon_model_fix_align (void)
{
	int	i;
	int	*buf = malloc ((1024 * 4) + (1024 * 4) + (256 * 4));

	refs = buf;
	refs_ptr = buf + 1024;
	eofs = buf + 2048;

	for (i=0; i<N_polygon_models; i++)
	{
		num_refs = num_eofs = 0;
		polygon_model_fix_align_walk (&Polygon_models[i], 0);
	}

	free (buf);
}
