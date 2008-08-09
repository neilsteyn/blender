/*
 * $Id$
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2007 by Nicholas Bishop
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_vec_types.h"
#include "DNA_view3d_types.h"

#include "BIF_editmesh.h"

#include "BLI_arithb.h"
#include "BLI_blenlib.h"
#include "BLI_editVert.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_customdata.h"
#include "BKE_depsgraph.h"
#include "BKE_DerivedMesh.h"
#include "BKE_global.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_object.h"
#include "BKE_subsurf.h"

#include "blendef.h"
#include "editmesh.h"

#include <math.h>
#include <string.h>

/* Returns the active multires level (currently applied to the mesh) */
MultiresLevel *current_level(Multires *mr)
{
	return BLI_findlink(&mr->levels, mr->current - 1);
}

/* Returns the nth multires level, starting at 1 */
MultiresLevel *multires_level_n(Multires *mr, int n)
{
	if(mr)
		return BLI_findlink(&mr->levels, n - 1);
	else
		return NULL;
}

/* Does not actually free lvl itself */
void multires_free_level(MultiresLevel *lvl)
{
	if(lvl) {
		if(lvl->faces) MEM_freeN(lvl->faces);
		if(lvl->edges) MEM_freeN(lvl->edges);
		if(lvl->colfaces) MEM_freeN(lvl->colfaces);
	}
}

void multires_free(Multires *mr)
{
	if(mr) {
		MultiresLevel* lvl= mr->levels.first;

		/* Free the first-level data */
		if(lvl) {
			CustomData_free(&mr->vdata, lvl->totvert);
			CustomData_free(&mr->fdata, lvl->totface);
			MEM_freeN(mr->edge_flags);
			MEM_freeN(mr->edge_creases);
		}

		while(lvl) {
			multires_free_level(lvl);			
			lvl= lvl->next;
		}

		MEM_freeN(mr->verts);

		BLI_freelistN(&mr->levels);

		MEM_freeN(mr);
	}
}

static MultiresLevel *multires_level_copy(MultiresLevel *orig)
{
	if(orig) {
		MultiresLevel *lvl= MEM_dupallocN(orig);
		
		lvl->next= lvl->prev= NULL;
		lvl->faces= MEM_dupallocN(orig->faces);
		lvl->colfaces= MEM_dupallocN(orig->colfaces);
		lvl->edges= MEM_dupallocN(orig->edges);
		
		return lvl;
	}
	return NULL;
}

Multires *multires_copy(Multires *orig)
{
	const CustomDataMask vdata_mask= CD_MASK_MDEFORMVERT;

	if(orig) {
		Multires *mr= MEM_dupallocN(orig);
		MultiresLevel *lvl;
		
		mr->levels.first= mr->levels.last= NULL;
		
		for(lvl= orig->levels.first; lvl; lvl= lvl->next)
			BLI_addtail(&mr->levels, multires_level_copy(lvl));

		mr->verts= MEM_dupallocN(orig->verts);
		
		lvl= mr->levels.first;
		if(lvl) {
			CustomData_copy(&orig->vdata, &mr->vdata, vdata_mask, CD_DUPLICATE, lvl->totvert);
			CustomData_copy(&orig->fdata, &mr->fdata, CD_MASK_MTFACE, CD_DUPLICATE, lvl->totface);
			mr->edge_flags= MEM_dupallocN(orig->edge_flags);
			mr->edge_creases= MEM_dupallocN(orig->edge_creases);
		}
		
		return mr;
	}
	return NULL;
}

static void multires_get_vert(MVert *out, EditVert *eve, MVert *m, int i)
{
	if(eve) {
		VecCopyf(out->co, eve->co);
		out->flag= 0;
		if(eve->f & SELECT) out->flag |= 1;
		if(eve->h) out->flag |= ME_HIDE;
		eve->tmp.l= i;
	}
	else
		*out= *m;
}

void eed_to_medge_flag(EditEdge *eed, short *flag, char *crease)
{
	if(!eed || !flag) return;

	/* Would be nice if EditMesh edge flags could be unified with Mesh flags! */
	*flag= (eed->f & SELECT) | ME_EDGERENDER;
	if(eed->f2<2) *flag |= ME_EDGEDRAW;
	if(eed->f2==0) *flag |= ME_LOOSEEDGE;
	if(eed->sharp) *flag |= ME_SHARP;
	if(eed->seam) *flag |= ME_SEAM;
	if(eed->h & EM_FGON) *flag |= ME_FGON;
	if(eed->h & 1) *flag |= ME_HIDE;
	
	*crease= (char)(255.0*eed->crease);
}

static void multires_get_edge(MultiresEdge *e, EditEdge *eed, MEdge *m, short *flag, char *crease)
{
	if(eed) {
		e->v[0]= eed->v1->tmp.l;
		e->v[1]= eed->v2->tmp.l;
		eed_to_medge_flag(eed, flag, crease);
	} else {		
		e->v[0]= m->v1;
		e->v[1]= m->v2;
		*flag= m->flag;
		*crease= m->crease;
	}
}

static void multires_get_face(MultiresFace *f, EditFace *efa, MFace *m)
{
	if(efa) {
		MFace tmp;
		int j;
		tmp.v1= efa->v1->tmp.l;
		tmp.v2= efa->v2->tmp.l;
		tmp.v3= efa->v3->tmp.l;
		tmp.v4= 0;
		if(efa->v4) tmp.v4= efa->v4->tmp.l;
		test_index_face(&tmp, NULL, 0, efa->v4?4:3);
		for(j=0; j<4; ++j) f->v[j]= (&tmp.v1)[j];

		/* Flags */
		f->flag= efa->flag;
		if(efa->f & 1) f->flag |= ME_FACE_SEL;
		else f->flag &= ~ME_FACE_SEL;
		if(efa->h) f->flag |= ME_HIDE;
		f->mat_nr= efa->mat_nr;
	} else {		
		f->v[0]= m->v1;
		f->v[1]= m->v2;
		f->v[2]= m->v3;
		f->v[3]= m->v4;
		f->flag= m->flag;
		f->mat_nr= m->mat_nr;
	}
}

/* For manipulating vertex colors / uvs */
static void mcol_to_multires(MultiresColFace *mrf, MCol *mcol)
{
	char i;
	for(i=0; i<4; ++i) {
		mrf->col[i].a= mcol[i].a;
		mrf->col[i].r= mcol[i].r;
		mrf->col[i].g= mcol[i].g;
		mrf->col[i].b= mcol[i].b;
	}
}

/* 1 <= count <= 4 */
static void multires_col_avg(MultiresCol *avg, MultiresCol cols[4], char count)
{
	unsigned i;
	avg->a= avg->r= avg->g= avg->b= 0;
	for(i=0; i<count; ++i) {
		avg->a+= cols[i].a;
		avg->r+= cols[i].r;
		avg->g+= cols[i].g;
		avg->b+= cols[i].b;
	}
	avg->a/= count;
	avg->r/= count;
	avg->g/= count;
	avg->b/= count;
}

static void multires_col_avg2(MultiresCol *avg, MultiresCol *c1, MultiresCol *c2)
{
	MultiresCol in[2];
	in[0]= *c1;
	in[1]= *c2;
	multires_col_avg(avg,in,2);
}

void multires_load_cols(Mesh *me)
{
	MultiresLevel *lvl= BLI_findlink(&me->mr->levels,me->mr->current-1), *cur;
	EditMesh *em= G.obedit ? G.editMesh : NULL;
	CustomData *src= em ? &em->fdata : &me->fdata;
	EditFace *efa= NULL;
	unsigned i,j;

	if(!CustomData_has_layer(src, CD_MCOL) && !CustomData_has_layer(src, CD_MTFACE)) return;

	/* Add texcol data */
	for(cur= me->mr->levels.first; cur; cur= cur->next)
		if(!cur->colfaces)
			cur->colfaces= MEM_callocN(sizeof(MultiresColFace)*cur->totface,"ColFaces");

	me->mr->use_col= CustomData_has_layer(src, CD_MCOL);

	if(em) efa= em->faces.first;
	for(i=0; i<lvl->totface; ++i) {
		MultiresColFace *f= &lvl->colfaces[i];

		if(me->mr->use_col)
			mcol_to_multires(f, em ? CustomData_em_get(src, efa->data, CD_MCOL) : &me->mcol[i*4]);
		
		if(em) efa= efa->next;
	}

	/* Update higher levels */
	lvl= lvl->next;
	while(lvl) {
		MultiresColFace *cf= lvl->colfaces;
		for(i=0; i<lvl->prev->totface; ++i) {
			const char sides= lvl->prev->faces[i].v[3]?4:3;
			MultiresCol cntr;
			
			/* Find average color of 4 (or 3 for triangle) verts */
			multires_col_avg(&cntr,lvl->prev->colfaces[i].col,sides);
			
			for(j=0; j<sides; ++j) {
				MultiresColFace *pf= &lvl->prev->colfaces[i];

				multires_col_avg2(&cf->col[0],
						  &pf->col[j],
						  &pf->col[j==0?sides-1:j-1]);
				cf->col[1]= pf->col[j];
				multires_col_avg2(&cf->col[2],
						  &pf->col[j],
						  &pf->col[j==sides-1?0:j+1]);
				cf->col[3]= cntr;
				
				++cf;
			}
		}
		lvl= lvl->next;
	}

	/* Update lower levels */
	lvl= me->mr->levels.last;
	lvl= lvl->prev;
	while(lvl) {
		unsigned curf= 0;
		for(i=0; i<lvl->totface; ++i) {
			MultiresFace *f= &lvl->faces[i];
			for(j=0; j<(f->v[3]?4:3); ++j) {
				lvl->colfaces[i].col[j]= lvl->next->colfaces[curf].col[1];
				++curf;
			}
		}
		lvl= lvl->prev;
	}
}

void multires_create(Object *ob, Mesh *me)
{
	MultiresLevel *lvl;
	EditMesh *em= G.obedit ? G.editMesh : NULL;
	EditVert *eve= NULL;
	EditFace *efa= NULL;
	EditEdge *eed= NULL;
	int i;
	
	lvl= MEM_callocN(sizeof(MultiresLevel), "multires level");

	if(me->pv) mesh_pmv_off(ob, me);

	me->mr= MEM_callocN(sizeof(Multires), "multires data");
	
	BLI_addtail(&me->mr->levels,lvl);
	me->mr->current= 1;
	me->mr->level_count= 1;
	me->mr->edgelvl= 1;
	me->mr->pinlvl= 1;
	me->mr->renderlvl= 1;
	
	/* Load mesh (or editmesh) into multires data */

	/* Load vertices and vdata (MDeformVerts) */
	lvl->totvert= em ? BLI_countlist(&em->verts) : me->totvert;
	me->mr->verts= MEM_callocN(sizeof(MVert)*lvl->totvert,"multires verts");
	multires_update_customdata(me->mr->levels.first, em, em ? &em->vdata : &me->vdata,
	                           &me->mr->vdata, CD_MDEFORMVERT);
	if(em) eve= em->verts.first;
	for(i=0; i<lvl->totvert; ++i) {
		multires_get_vert(&me->mr->verts[i], eve, &me->mvert[i], i);
		if(em) eve= eve->next;
	}

	/* Load faces and fdata (MTFaces) */
	lvl->totface= em ? BLI_countlist(&em->faces) : me->totface;
	lvl->faces= MEM_callocN(sizeof(MultiresFace)*lvl->totface,"multires faces");
	multires_update_customdata(me->mr->levels.first, em, em ? &em->fdata : &me->fdata,
	                           &me->mr->fdata, CD_MTFACE);
	if(em) efa= em->faces.first;
	for(i=0; i<lvl->totface; ++i) {
		multires_get_face(&lvl->faces[i], efa, &me->mface[i]);
		if(em) efa= efa->next;
	}

	/* Load edges and edge_flags */
	lvl->totedge= em ? BLI_countlist(&em->edges) : me->totedge;
	lvl->edges= MEM_callocN(sizeof(MultiresEdge)*lvl->totedge,"multires edges");
	me->mr->edge_flags= MEM_callocN(sizeof(short)*lvl->totedge, "multires edge flags");
	me->mr->edge_creases= MEM_callocN(sizeof(short)*lvl->totedge, "multires edge creases");
	if(em) eed= em->edges.first;
	for(i=0; i<lvl->totedge; ++i) {
		multires_get_edge(&lvl->edges[i], eed, &me->medge[i], &me->mr->edge_flags[i], &me->mr->edge_creases[i]);
		if(em) eed= eed->next;
	}

	multires_load_cols(me);
}

typedef struct MultiresMapNode {
	struct MultiresMapNode *next, *prev;
	unsigned Index;
} MultiresMapNode;

/* CATMULL-CLARK
   ============= */

typedef struct MultiApplyData {
	/* Smooth faces */
	float *corner1, *corner2, *corner3, *corner4;
	char quad;

	/* Smooth edges */
	char boundary;
	float edge_face_neighbor_midpoints_accum[3];
	unsigned edge_face_neighbor_midpoints_total;
	float *endpoint1, *endpoint2;

	/* Smooth verts */
	/* uses 'char boundary' */
	float *original;
	int edge_count;
	float vert_face_neighbor_midpoints_average[3];
	float vert_edge_neighbor_midpoints_average[3];
	float boundary_edges_average[3];
} MultiApplyData;

/* END CATMULL-CLARK
   ================= */

static void multires_update_faces(Mesh *me, EditMesh *em)
{
	MultiresLevel *cr_lvl= current_level(me->mr), *pr_lvl= NULL,
		      *last_lvl= me->mr->levels.last;
	char *pr_flag_damaged= NULL, *cr_flag_damaged= NULL, *or_flag_damaged= NULL,
	     *pr_mat_damaged= NULL, *cr_mat_damaged= NULL, *or_mat_damaged= NULL, *swap= NULL;
	EditFace *efa= NULL;
	unsigned i,j,curf;

	/* Find for each face whether flag/mat has changed */
	pr_flag_damaged= MEM_callocN(sizeof(char) * last_lvl->totface, "flag_damaged 1");
	cr_flag_damaged= MEM_callocN(sizeof(char) * last_lvl->totface, "flag_damaged 1");
	pr_mat_damaged= MEM_callocN(sizeof(char) * last_lvl->totface, "mat_damaged 1");
	cr_mat_damaged= MEM_callocN(sizeof(char) * last_lvl->totface, "mat_damaged 1");
	if(em) efa= em->faces.first;
	for(i=0; i<cr_lvl->totface; ++i) {
		MultiresFace mftmp;
		multires_get_face(&mftmp, efa, &me->mface[i]);
		if(cr_lvl->faces[i].flag != mftmp.flag)
			cr_flag_damaged[i]= 1;
		if(cr_lvl->faces[i].mat_nr != mftmp.mat_nr)
			cr_mat_damaged[i]= 1;

		/* Update current level */
		cr_lvl->faces[i].flag= mftmp.flag;
		cr_lvl->faces[i].mat_nr= mftmp.mat_nr;

		if(em) efa= efa->next;
	}
	or_flag_damaged= MEM_dupallocN(cr_flag_damaged);
	or_mat_damaged= MEM_dupallocN(cr_mat_damaged);

	/* Update lower levels */
	cr_lvl= cr_lvl->prev;
	while(cr_lvl) {
		swap= pr_flag_damaged;
		pr_flag_damaged= cr_flag_damaged;
		cr_flag_damaged= swap;

		swap= pr_mat_damaged;
		pr_mat_damaged= cr_mat_damaged;
		cr_mat_damaged= swap;

		curf= 0;
		for(i=0; i<cr_lvl->totface; ++i) {
			const int sides= cr_lvl->faces[i].v[3] ? 4 : 3;
			
			/* Check damages */
			for(j=0; j<sides; ++j, ++curf) {
				if(pr_flag_damaged[curf]) {
					cr_lvl->faces[i].flag= cr_lvl->next->faces[curf].flag;
					cr_flag_damaged[i]= 1;
				}
				if(pr_mat_damaged[curf]) {
					cr_lvl->faces[i].mat_nr= cr_lvl->next->faces[curf].mat_nr;
					cr_mat_damaged[i]= 1;
				}
			}
		}

		cr_lvl= cr_lvl->prev;
	}
	
	/* Clear to original damages */
	if(cr_flag_damaged) MEM_freeN(cr_flag_damaged);
	if(cr_mat_damaged) MEM_freeN(cr_mat_damaged);
	cr_flag_damaged= or_flag_damaged;
	cr_mat_damaged= or_mat_damaged;
	
	/* Update higher levels */
	pr_lvl= current_level(me->mr);
	cr_lvl= pr_lvl->next;
	while(cr_lvl) {
		swap= pr_flag_damaged;
		pr_flag_damaged= cr_flag_damaged;
		cr_flag_damaged= swap;

		swap= pr_mat_damaged;
		pr_mat_damaged= cr_mat_damaged;
		cr_mat_damaged= swap;

		/* Update faces */
		for(i=0, curf= 0; i<pr_lvl->totface; ++i) {
			const int sides= cr_lvl->prev->faces[i].v[3] ? 4 : 3;
			for(j=0; j<sides; ++j, ++curf) {
				if(pr_flag_damaged[i]) {
					cr_lvl->faces[curf].flag= pr_lvl->faces[i].flag;
					cr_flag_damaged[curf]= 1;
				}
				if(pr_mat_damaged[i]) {
					cr_lvl->faces[curf].mat_nr= pr_lvl->faces[i].mat_nr;
					cr_mat_damaged[curf]= 1;
				}
			}
		}

		pr_lvl= pr_lvl->next;
		cr_lvl= cr_lvl->next;
	}

	if(pr_flag_damaged) MEM_freeN(pr_flag_damaged);
	if(cr_flag_damaged) MEM_freeN(cr_flag_damaged);
	if(pr_mat_damaged) MEM_freeN(pr_mat_damaged);
	if(cr_mat_damaged) MEM_freeN(cr_mat_damaged);
}

static void multires_update_colors(Mesh *me, EditMesh *em)
{
	MultiresLevel *lvl= BLI_findlink(&me->mr->levels,me->mr->current-1);
	MultiresCol *pr_deltas= NULL, *cr_deltas= NULL;
	CustomData *src= em ? &em->fdata : &me->fdata;
	EditFace *efa= NULL;
	unsigned i,j,curf= 0;
	
	if(me->mr->use_col) {
		/* Calc initial deltas */
		cr_deltas= MEM_callocN(sizeof(MultiresCol)*lvl->totface*4,"initial color/uv deltas");

		if(em) efa= em->faces.first;
		for(i=0; i<lvl->totface; ++i) {
			MCol *col= em ? CustomData_em_get(src, efa->data, CD_MCOL) : &me->mcol[i*4];
			for(j=0; j<4; ++j) {
				if(me->mr->use_col) {
					cr_deltas[i*4+j].a= col[j].a - lvl->colfaces[i].col[j].a;
					cr_deltas[i*4+j].r= col[j].r - lvl->colfaces[i].col[j].r;
					cr_deltas[i*4+j].g= col[j].g - lvl->colfaces[i].col[j].g;
					cr_deltas[i*4+j].b= col[j].b - lvl->colfaces[i].col[j].b;
				}
			}
			if(em) efa= efa->next;
		}
		
		/* Update current level */
		if(em) efa= em->faces.first;
		for(i=0; i<lvl->totface; ++i) {
			MultiresColFace *f= &lvl->colfaces[i];

			if(me->mr->use_col)
				mcol_to_multires(f, em ? CustomData_em_get(src, efa->data, CD_MCOL) : &me->mcol[i*4]);
			
			if(em) efa= efa->next;
		}
		
		/* Update higher levels */
		lvl= lvl->next;
		while(lvl) {
			/* Set up new deltas, but keep the ones from the previous level */
			if(pr_deltas) MEM_freeN(pr_deltas);
			pr_deltas= cr_deltas;
			cr_deltas= MEM_callocN(sizeof(MultiresCol)*lvl->totface*4,"color deltas");

			curf= 0;
			for(i=0; i<lvl->prev->totface; ++i) {
				const char sides= lvl->prev->faces[i].v[3]?4:3;
				MultiresCol cntr;
				
				/* Find average color of 4 (or 3 for triangle) verts */
				multires_col_avg(&cntr,&pr_deltas[i*4],sides);
				
				for(j=0; j<sides; ++j) {
					multires_col_avg2(&cr_deltas[curf*4],
							  &pr_deltas[i*4+j],
							  &pr_deltas[i*4+(j==0?sides-1:j-1)]);
					cr_deltas[curf*4+1]= pr_deltas[i*4+j];
					multires_col_avg2(&cr_deltas[curf*4+2],
							  &pr_deltas[i*4+j],
							  &pr_deltas[i*4+(j==sides-1?0:j+1)]);
					cr_deltas[curf*4+3]= cntr;
					++curf;
				}
			}

			for(i=0; i<lvl->totface; ++i) {
				for(j=0; j<4; ++j) {
					lvl->colfaces[i].col[j].a+= cr_deltas[i*4+j].a;
					lvl->colfaces[i].col[j].r+= cr_deltas[i*4+j].r;
					lvl->colfaces[i].col[j].g+= cr_deltas[i*4+j].g;
					lvl->colfaces[i].col[j].b+= cr_deltas[i*4+j].b;
				}
			}

			lvl= lvl->next;
		}
		if(pr_deltas) MEM_freeN(pr_deltas);
		if(cr_deltas) MEM_freeN(cr_deltas);
		
		/* Update lower levels */
		lvl= me->mr->levels.last;
		lvl= lvl->prev;
		while(lvl) {
			MultiresColFace *nf= lvl->next->colfaces;
			for(i=0; i<lvl->totface; ++i) {
				MultiresFace *f= &lvl->faces[i];
				for(j=0; j<(f->v[3]?4:3); ++j) {
					lvl->colfaces[i].col[j]= nf->col[1];
					++nf;
				}
			}
			lvl= lvl->prev;
		}
	}
}

void multires_update_levels(Mesh *me, const int render)
{
	EditMesh *em= (!render && G.obedit) ? G.editMesh : NULL;

	multires_update_first_level(me, em);
	multires_update_faces(me, em);
	multires_update_colors(me, em);
}

static void check_colors(Mesh *me)
{
	CustomData *src= G.obedit ? &G.editMesh->fdata : &me->fdata;
	const char col= CustomData_has_layer(src, CD_MCOL);

	/* Check if vertex colors have been deleted or added */
	if(me->mr->use_col && !col)
		me->mr->use_col= 0;
	else if(!me->mr->use_col && col) {
		me->mr->use_col= 1;
		multires_load_cols(me);
	}
}

static float clamp_component(const float c)
{
	if(c<0) return 0;
	else if(c>255) return 255;
	else return c;
}

void multires_to_mcol(MultiresColFace *f, MCol mcol[4])
{
	unsigned char j;
	for(j=0; j<4; ++j) {
		mcol->a= clamp_component(f->col[j].a);
		mcol->r= clamp_component(f->col[j].r);
		mcol->g= clamp_component(f->col[j].g);
		mcol->b= clamp_component(f->col[j].b);
		++mcol;
	}
}

void multires_level_to_mesh(Object *ob, Mesh *me, const int render)
{
	MultiresLevel *lvl= BLI_findlink(&me->mr->levels,me->mr->current-1);
	int i;
	EditMesh *em= (!render && G.obedit) ? G.editMesh : NULL;
	
	if(em)
		return;

	CustomData_free_layer_active(&me->vdata, CD_MVERT, me->totvert);
	CustomData_free_layer_active(&me->edata, CD_MEDGE, me->totedge);
	CustomData_free_layer_active(&me->fdata, CD_MFACE, me->totface);
	CustomData_free_layer_active(&me->vdata, CD_MDEFORMVERT, me->totvert);
	CustomData_free_layers(&me->fdata, CD_MTFACE, me->totface);
	CustomData_free_layers(&me->fdata, CD_MCOL, me->totface);
		
	me->totvert= lvl->totvert;
	me->totface= lvl->totface;
	me->totedge= lvl->totedge;

	CustomData_add_layer(&me->vdata, CD_MVERT, CD_CALLOC, NULL, me->totvert);
	CustomData_add_layer(&me->edata, CD_MEDGE, CD_CALLOC, NULL, me->totedge);
	CustomData_add_layer(&me->fdata, CD_MFACE, CD_CALLOC, NULL, me->totface);
	mesh_update_customdata_pointers(me);

	/* Vertices/Edges/Faces */
	
	for(i=0; i<lvl->totvert; ++i) {
		me->mvert[i]= me->mr->verts[i];
	}
	for(i=0; i<lvl->totedge; ++i) {
		me->medge[i].v1= lvl->edges[i].v[0];
		me->medge[i].v2= lvl->edges[i].v[1];
		me->medge[i].flag &= ~ME_HIDE;
	}
	for(i=0; i<lvl->totface; ++i) {
		me->mface[i].v1= lvl->faces[i].v[0];
		me->mface[i].v2= lvl->faces[i].v[1];
		me->mface[i].v3= lvl->faces[i].v[2];
		me->mface[i].v4= lvl->faces[i].v[3];
		me->mface[i].flag= lvl->faces[i].flag;
		me->mface[i].flag &= ~ME_HIDE;
		me->mface[i].mat_nr= lvl->faces[i].mat_nr;
	}
	
	/* Edge flags */
	if(lvl==me->mr->levels.first) {
		for(i=0; i<lvl->totedge; ++i) {
			me->medge[i].flag= me->mr->edge_flags[i];
			me->medge[i].crease= me->mr->edge_creases[i];
		}
	} else {
		MultiresLevel *lvl1= me->mr->levels.first;
		const int last= lvl1->totedge * pow(2, me->mr->current-1);
		for(i=0; i<last; ++i) {
			const int ndx= i / pow(2, me->mr->current-1);
			
			me->medge[i].flag= me->mr->edge_flags[ndx];
			me->medge[i].crease= me->mr->edge_creases[ndx];
		}
	}

	multires_customdata_to_mesh(me, em, lvl, &me->mr->vdata, em ? &em->vdata : &me->vdata, CD_MDEFORMVERT);
	multires_customdata_to_mesh(me, em, lvl, &me->mr->fdata, em ? &em->fdata : &me->fdata, CD_MTFACE);

	/* Colors */
	if(me->mr->use_col) {
		CustomData *src= &me->fdata;
		
		if(me->mr->use_col) me->mcol= CustomData_add_layer(src, CD_MCOL, CD_CALLOC, NULL, me->totface);
		
		for(i=0; i<lvl->totface; ++i) {
			if(me->mr->use_col)
				multires_to_mcol(&lvl->colfaces[i], &me->mcol[i*4]);
		}
			
	}
	
	mesh_update_customdata_pointers(me);
	
	multires_edge_level_update(ob,me);
	DAG_object_flush_update(G.scene, ob, OB_RECALC_DATA);
	mesh_calc_normals(me->mvert, me->totvert, me->mface, me->totface, NULL);
}

void multires_set_level(Object *ob, Mesh *me, const int render)
{
	if(me->pv) mesh_pmv_off(ob, me);

	check_colors(me);
	multires_update_levels(me, render);

	me->mr->current= me->mr->newlvl;
	if(me->mr->current<1) me->mr->current= 1;
	else if(me->mr->current>me->mr->level_count) me->mr->current= me->mr->level_count;

	multires_level_to_mesh(ob, me, render);
}

/* Update the edge visibility flags to only show edges on or below the edgelvl */
void multires_edge_level_update(Object *ob, Mesh *me)
{
	if(!G.obedit) {
		MultiresLevel *cr_lvl= BLI_findlink(&me->mr->levels,me->mr->current-1);
		MultiresLevel *edge_lvl= BLI_findlink(&me->mr->levels,me->mr->edgelvl-1);
		const int threshold= edge_lvl->totedge * pow(2, me->mr->current - me->mr->edgelvl);
		unsigned i;

		for(i=0; i<cr_lvl->totedge; ++i) {
			const int ndx= me->pv ? me->pv->edge_map[i] : i;
			if(ndx != -1) { /* -1= hidden edge */
				if(me->mr->edgelvl >= me->mr->current || i<threshold)
					me->medge[ndx].flag |= ME_EDGEDRAW | ME_EDGERENDER;
				else
					me->medge[ndx].flag &= ~ME_EDGEDRAW & ~ME_EDGERENDER;
			}
		}
		
		DAG_object_flush_update(G.scene, ob, OB_RECALC_DATA);
	}
}

void create_vert_face_map(ListBase **map, IndexNode **mem, const MFace *mface, const int totvert, const int totface)
{
	int i,j;
	IndexNode *node = NULL;
	
	(*map) = MEM_callocN(sizeof(ListBase) * totvert, "vert face map");
	(*mem) = MEM_callocN(sizeof(IndexNode) * totface*4, "vert face map mem");
	node = *mem;
	
	/* Find the users */
	for(i = 0; i < totface; ++i){
		for(j = 0; j < (mface[i].v4?4:3); ++j, ++node) {
			node->index = i;
			BLI_addtail(&(*map)[((unsigned int*)(&mface[i]))[j]], node);
		}
	}
}

/* MULTIRES MODIFIER */
static const int multires_max_levels = 13;
static const int multires_quad_tot[] = {4, 9, 25, 81, 289, 1089, 4225, 16641, 66049, 263169, 1050625, 4198401, 16785409};
static const int multires_tri_tot[]  = {3, 7, 19, 61, 217, 817,  3169, 12481, 49537, 197377, 787969,  3148801, 12589057};
static const int multires_side_tot[] = {2, 3, 5,  9,  17,  33,   65,   129,   257,   513,    1025,    2049,    4097};

int multiresModifier_switch_level(Object *ob, const int distance)
{
	ModifierData *md = NULL;
	MultiresModifierData *mmd = NULL;

	for(md = ob->modifiers.first; md; md = md->next) {
		if(md->type == eModifierType_Multires)
			mmd = (MultiresModifierData*)md;
	}

	if(mmd) {
		mmd->lvl += distance;
		if(mmd->lvl < 1) mmd->lvl = 1;
		else if(mmd->lvl > mmd->totlvl) mmd->lvl = mmd->totlvl;
		DAG_object_flush_update(G.scene, ob, OB_RECALC_DATA);
		object_handle_update(ob);
		return 1;
	}
	else
		return 0;
}

void multiresModifier_join(Object *ob)
{
	Base *base = NULL;
	int highest_lvl = 0;

	/* First find the highest level of subdivision */
	base = FIRSTBASE;
	while(base) {
		if(TESTBASELIB_BGMODE(base) && base->object->type==OB_MESH) {
			ModifierData *md;
			for(md = base->object->modifiers.first; md; md = md->next) {
				if(md->type == eModifierType_Multires) {
					int totlvl = ((MultiresModifierData*)md)->totlvl;
					if(totlvl > highest_lvl)
						highest_lvl = totlvl;

					/* Ensure that all updates are processed */
					multires_force_update(base->object);
				}
			}
		}
		base = base->next;
	}

	/* No multires meshes selected */
	if(highest_lvl == 0)
		return;

	/* Subdivide all the displacements to the highest level */
	base = FIRSTBASE;
	while(base) {
		if(TESTBASELIB_BGMODE(base) && base->object->type==OB_MESH) {
			ModifierData *md = NULL;
			MultiresModifierData *mmd = NULL;

			for(md = base->object->modifiers.first; md; md = md->next) {
				if(md->type == eModifierType_Multires)
					mmd = (MultiresModifierData*)md;
			}

			/* If the object didn't have multires enabled, give it a new modifier */
			if(!mmd) {
				ModifierData *md = base->object->modifiers.first;
				
				while(md && modifierType_getInfo(md->type)->type == eModifierTypeType_OnlyDeform)
					md = md->next;
				
				mmd = (MultiresModifierData*)modifier_new(eModifierType_Multires);
				BLI_insertlinkbefore(&base->object->modifiers, md, mmd);
			}

			if(mmd) {
				int i;

				/* TODO: subdivision should be doable in one step rather than iteratively. */
				for(i = mmd->totlvl; i < highest_lvl; ++i)
					multiresModifier_subdivide(mmd, base->object);
			}
		}
		base = base->next;
	}
}

static void Mat3FromColVecs(float mat[][3], float v1[3], float v2[3], float v3[3])
{
	VecCopyf(mat[0], v1);
	VecCopyf(mat[1], v2);
	VecCopyf(mat[2], v3);
}

static void calc_ts_mat(float out[][3], float center[3], float spintarget[3], float normal[3])
{
	float tan[3], cross[3];

	VecSubf(tan, spintarget, center);
	Normalize(tan);

	Crossf(cross, normal, tan);

	Mat3FromColVecs(out, tan, cross, normal);
}

static void face_center(float *out, float *a, float *b, float *c, float *d)
{
	VecAddf(out, a, b);
	VecAddf(out, out, c);
	if(d)
		VecAddf(out, out, d);

	VecMulf(out, d ? 0.25 : 1.0 / 3.0);
}

static void calc_norm(float *norm, float *a, float *b, float *c, float *d)
{
	if(d)
		CalcNormFloat4(a, b, c, d, norm);
	else
		CalcNormFloat(a, b, c, norm);
}

static void calc_face_ts_mat(float out[][3], float *v1, float *v2, float *v3, float *v4)
{
	float center[3], norm[3];

	face_center(center, v1, v2, v3, v4);
	calc_norm(norm, v1, v2, v3, v4);
	calc_ts_mat(out, center, v1, norm);
}

static void calc_face_ts_mat_dm(float out[][3], float (*orco)[3], MFace *f)
{
	calc_face_ts_mat(out, orco[f->v1], orco[f->v2], orco[f->v3], (f->v4 ? orco[f->v4] : NULL));
}

static void calc_face_ts_partial(float center[3], float target[3], float norm[][3], float (*orco)[3], MFace *f)
{
	face_center(center, orco[f->v1], orco[f->v2], orco[f->v3], (f->v4 ? orco[f->v4] : NULL));
	VecCopyf(target, orco[f->v1]);
}

DerivedMesh *multires_subdisp_pre(DerivedMesh *mrdm, int distance)
{
	DerivedMesh *final;
	SubsurfModifierData smd;

	memset(&smd, 0, sizeof(SubsurfModifierData));
	smd.levels = distance;
	final = subsurf_make_derived_from_derived_with_multires(mrdm, &smd, NULL, 0, NULL, 0, 0);

	return final;
}

void VecAddUf(float a[3], float b[3])
{
	a[0] += b[0];
	a[1] += b[1];
	a[2] += b[2];
}

static void multires_subdisp(DerivedMesh *orig, Mesh *me, DerivedMesh *final, int lvl, int totlvl,
			     int totsubvert, int totsubedge, int totsubface, int addverts)
{
	DerivedMesh *mrdm;
	MultiresModifierData mmd_sub;
	MVert *mvs = CDDM_get_verts(final);
	MVert *mvd, *mvd_f1, *mvs_f1, *mvd_f3, *mvd_f4;
	MVert *mvd_f2, *mvs_f2, *mvs_e1, *mvd_e1, *mvs_e2;
	int totvert;
	int slo1 = multires_side_tot[lvl - 1];
	int sll = slo1 / 2;
	int slo2 = multires_side_tot[totlvl - 2];
	int shi2 = multires_side_tot[totlvl - 1];
	int skip = multires_side_tot[totlvl - lvl] - 1;
	int i, j, k;

	mmd_sub.lvl = mmd_sub.totlvl = totlvl;
	mrdm = multires_dm_create_from_derived(&mmd_sub, orig, me, 0, 0);
		
	mvd = CDDM_get_verts(mrdm);
	/* Need to map from ccg to mrdm */
	totvert = mrdm->getNumVerts(mrdm);

	if(!addverts) {
		for(i = 0; i < totvert; ++i) {
			float z[3] = {0,0,0};
			VecCopyf(mvd[i].co, z);
		}
	}

	/* Load base verts */
	for(i = 0; i < me->totvert; ++i)
		VecAddUf(mvd[totvert - me->totvert + i].co, mvs[totvert - me->totvert + i].co);

	mvd_f1 = mvd;
	mvs_f1 = mvs;
	mvd_f2 = mvd;
	mvs_f2 = mvs + totvert - totsubvert;
	mvs_e1 = mvs + totsubface * (skip-1) * (skip-1);

	for(i = 0; i < me->totface; ++i) {
		const int end = me->mface[i].v4 ? 4 : 3;
		int x, y, x2, y2, mov;

		mvd_f1 += 1 + end * (slo2-2); //center+edgecross
		mvd_f3 = mvd_f4 = mvd_f1;

		for(j = 0; j < end; ++j) {
			mvd_f1 += (skip/2 - 1) * (slo2 - 2) + (skip/2 - 1);
			/* Update sub faces */
			for(y = 0; y < sll; ++y) {
				for(x = 0; x < sll; ++x) {
					/* Face center */
					VecAddUf(mvd_f1->co, mvs_f1->co);
					mvs_f1 += 1;

					/* Now we hold the center of the subface at mvd_f1
					   and offset it to the edge cross and face verts */

					/* Edge cross */
					for(k = 0; k < 4; ++k) {
						if(k == 0) mov = -1;
						else if(k == 1) mov = slo2 - 2;
						else if(k == 2) mov = 1;
						else if(k == 3) mov = -(slo2 - 2);

						for(x2 = 1; x2 < skip/2; ++x2) {
							VecAddUf((mvd_f1 + mov * x2)->co, mvs_f1->co);
							++mvs_f1;
						}
					}

					/* Main face verts */
					for(k = 0; k < 4; ++k) {
						int movx, movy;

						if(k == 0) { movx = -1; movy = -(slo2 - 2); }
						else if(k == 1) { movx = slo2 - 2; movy = -1; }
						else if(k == 2) { movx = 1; movy = slo2 - 2; }
						else if(k == 3) { movx = -(slo2 - 2); movy = 1; }

						for(y2 = 1; y2 < skip/2; ++y2) {
							for(x2 = 1; x2 < skip/2; ++x2) {
								VecAddUf((mvd_f1 + movy * y2 + movx * x2)->co, mvs_f1->co);
								++mvs_f1;
							}
						}
					}
							
					mvd_f1 += skip;
				}
				mvd_f1 += (skip - 1) * (slo2 - 2) - 1;
			}
			mvd_f1 -= (skip - 1) * (slo2 - 2) - 1 + skip;
			mvd_f1 += (slo2 - 2) * (skip/2-1) + skip/2-1 + 1;
		}

		/* update face center verts */
		VecAddUf(mvd_f2->co, mvs_f2->co);

		mvd_f2 += 1;
		mvs_f2 += 1;

		/* update face edge verts */
		for(j = 0; j < end; ++j) {
			MVert *restore;

			/* Super-face edge cross */
			for(k = 0; k < skip-1; ++k) {
				VecAddUf(mvd_f2->co, mvs_e1->co);
				mvd_f2++;
				mvs_e1++;
			}
			for(x = 1; x < sll; ++x) {
				VecAddUf(mvd_f2->co, mvs_f2->co);
				mvd_f2++;
				mvs_f2++;

				for(k = 0; k < skip-1; ++k) {
					VecAddUf(mvd_f2->co, mvs_e1->co);
					mvd_f2++;
					mvs_e1++;
				}
			}

			restore = mvs_e1;
			for(y = 0; y < sll - 1; ++y) {
				for(x = 0; x < sll; ++x) {
					for(k = 0; k < skip - 1; ++k) {
						VecAddUf(mvd_f3[(skip-1)+(y*skip) + (x*skip+k)*(slo2-2)].co,
							 mvs_e1->co);
						++mvs_e1;
					}
					mvs_e1 += skip-1;
				}
			}
			
			mvs_e1 = restore + skip - 1;
			for(y = 0; y < sll - 1; ++y) {
				for(x = 0; x < sll; ++x) {
					for(k = 0; k < skip - 1; ++k) {
						VecAddUf(mvd_f3[(slo2-2)*(skip-1)+(x*skip)+k + y*skip*(slo2-2)].co,
							 mvs_e1->co);
						++mvs_e1;
					}
					mvs_e1 += skip - 1;
				}
			}

			mvd_f3 += (slo2-2)*(slo2-2);
			mvs_e1 -= skip - 1;
		}

		/* update base (2) face verts */
		for(j = 0; j < end; ++j) {
			mvd_f2 += (slo2 - 1) * (skip - 1);
			for(y = 0; y < sll - 1; ++y) {
				for(x = 0; x < sll - 1; ++x) {
					VecAddUf(mvd_f2->co, mvs_f2->co);
					mvd_f2 += skip;
					++mvs_f2;
				}
				mvd_f2 += (slo2 - 1) * (skip - 1);
			}
			mvd_f2 -= (skip - 1);
		}
	}

	/* edges */
	mvd_e1 = mvd + totvert - me->totvert - me->totedge * (shi2-2);
	mvs_e2 = mvs + totvert - me->totvert - me->totedge * (slo1-2);
	for(i = 0; i < me->totedge; ++i) {
		for(j = 0; j < skip - 1; ++j) {
			VecAddUf(mvd_e1->co, mvs_e1->co);
			mvd_e1++;
			mvs_e1++;
		}
		for(j = 0; j < slo1 - 2; j++) {
			VecAddUf(mvd_e1->co, mvs_e2->co);
			mvd_e1++;
			mvs_e2++;
			
			for(k = 0; k < skip - 1; ++k) {
				VecAddUf(mvd_e1->co, mvs_e1->co);
				mvd_e1++;
				mvs_e1++;
			}
		}
	}

	/*if(addverts) {
		for(i = 0; i < totvert; ++i)
			VecAddf(mvd[i].co, mvd[i].co, addverts[i].co);
			}*/

	final->needsFree = 1;
	final->release(final);
	mrdm->needsFree = 1;
	*MultiresDM_get_flags(mrdm) |= MULTIRES_DM_UPDATE_ALWAYS;
	mrdm->release(mrdm);
}

void multiresModifier_subdivide(MultiresModifierData *mmd, Object *ob)
{
	DerivedMesh *final = NULL;
	int totsubvert, totsubface, totsubedge;
	Mesh *me = get_mesh(ob);
	MDisps *mdisps;
	int i, slo, shi;

	if(mmd->totlvl == multires_max_levels) {
		// TODO
		return;
	}

	multires_force_update(ob);

	++mmd->lvl;
	++mmd->totlvl;

	slo = multires_side_tot[mmd->totlvl - 2];
	shi = multires_side_tot[mmd->totlvl - 1];

	mdisps = CustomData_get_layer(&me->fdata, CD_MDISPS);
	if(!mdisps)
		mdisps = CustomData_add_layer(&me->fdata, CD_MDISPS, CD_DEFAULT, NULL, me->totface);


	if(mdisps->disps) {
		DerivedMesh *orig, *mrdm;
		MultiresModifierData mmd_sub;

		orig = CDDM_from_mesh(me, NULL);
		mmd_sub.lvl = mmd_sub.totlvl = mmd->totlvl - 1;
		mrdm = multires_dm_create_from_derived(&mmd_sub, orig, me, 0, 0);
		totsubvert = mrdm->getNumVerts(mrdm);
		totsubedge = mrdm->getNumEdges(mrdm);
		totsubface = mrdm->getNumFaces(mrdm);
		orig->needsFree = 1;
		orig->release(orig);
		
		final = multires_subdisp_pre(mrdm, 1);
		mrdm->needsFree = 1;
		mrdm->release(mrdm);
	}

	for(i = 0; i < me->totface; ++i) {
		//const int totdisp = (me->mface[i].v4 ? multires_quad_tot[totlvl] : multires_tri_tot[totlvl]);
		const int totdisp = multires_quad_tot[mmd->totlvl - 1];
		float (*disps)[3] = MEM_callocN(sizeof(float) * 3 * totdisp, "multires disps");

		if(mdisps[i].disps)
			MEM_freeN(mdisps[i].disps);

		mdisps[i].disps = disps;
		mdisps[i].totdisp = totdisp;
	}


	if(final) {
		DerivedMesh *orig;

		orig = CDDM_from_mesh(me, NULL);

		multires_subdisp(orig, me, final, mmd->totlvl - 1, mmd->totlvl, totsubvert, totsubedge, totsubface, 0);

		orig->needsFree = 1;
		orig->release(orig);
	}
}

void multiresModifier_setLevel(void *mmd_v, void *ob_v)
{
	MultiresModifierData *mmd = mmd_v;
	Object *ob = ob_v;
	Mesh *me = get_mesh(ob);

	if(me && mmd) {
		// TODO
	}
}

void multires_displacer_init(MultiresDisplacer *d, DerivedMesh *dm,
			     const int face_index, const int invert)
{
	float inv[3][3];

	d->face = MultiresDM_get_mesh(dm)->mface + face_index;
	/* Get the multires grid from customdata and calculate the TS matrix */
	d->grid = (MDisps*)dm->getFaceDataArray(dm, CD_MDISPS);
	if(d->grid)
		d->grid += face_index;
	calc_face_ts_mat_dm(d->mat, MultiresDM_get_orco(dm), d->face);
	if(invert) {
		Mat3Inv(inv, d->mat);
		Mat3CpyMat3(d->mat, inv);
	}

	calc_face_ts_partial(d->mat_center, d->mat_target, d->mat_norms, MultiresDM_get_orco(dm), d->face);
	d->mat_norms = MultiresDM_get_vertnorm(dm);

	d->spacing = pow(2, MultiresDM_get_totlvl(dm) - MultiresDM_get_lvl(dm));
	d->sidetot = multires_side_tot[MultiresDM_get_totlvl(dm) - 1];
	d->invert = invert;
}

void multires_displacer_weight(MultiresDisplacer *d, const float w)
{
	d->weight = w;
}

void multires_displacer_anchor(MultiresDisplacer *d, const int type, const int side_index)
{
	d->sidendx = side_index;
	d->x = d->y = d->sidetot / 2;
	d->type = type;

	if(type == 2) {
		if(side_index == 0)
			d->y -= d->spacing;
		else if(side_index == 1)
			d->x += d->spacing;
		else if(side_index == 2)
			d->y += d->spacing;
		else if(side_index == 3)
			d->x -= d->spacing;
	}
	else if(type == 3) {
		if(side_index == 0) {
			d->x -= d->spacing;
			d->y -= d->spacing;
		}
		else if(side_index == 1) {
			d->x += d->spacing;
			d->y -= d->spacing;
		}
		else if(side_index == 2) {
			d->x += d->spacing;
			d->y += d->spacing;
	}
		else if(side_index == 3) {
			d->x -= d->spacing;
			d->y += d->spacing;
		}
	}

	d->ax = d->x;
	d->ay = d->y;
}

void multires_displacer_anchor_edge(MultiresDisplacer *d, int v1, int v2, int x)
{
	const int mov = d->spacing * x;

	d->type = 4;

	if(v1 == d->face->v1) {
		d->x = 0;
		d->y = 0;
		if(v2 == d->face->v2)
			d->x += mov;
		else
			d->y += mov;
	}
	else if(v1 == d->face->v2) {
		d->x = d->sidetot - 1;
		d->y = 0;
		if(v2 == d->face->v1)
			d->x -= mov;
		else
			d->y += mov;
	}
	else if(v1 == d->face->v3) {
		d->x = d->sidetot - 1;
		d->y = d->sidetot - 1;
		if(v2 == d->face->v2)
			d->y -= mov;
		else
			d->x -= mov;
	}
	else if(v1 == d->face->v4) {
		d->x = 0;
		d->y = d->sidetot - 1;
		if(v2 == d->face->v3)
			d->x += mov;
		else
			d->y -= mov;
	}
}

void multires_displacer_anchor_vert(MultiresDisplacer *d, const int v)
{
	const int e = d->sidetot - 1;

	d->type = 5;

	d->x = d->y = 0;
	if(v == d->face->v2)
		d->x = e;
	else if(v == d->face->v3)
		d->x = d->y = e;
	else if(v == d->face->v4)
		d->y = e;
}

void multires_displacer_jump(MultiresDisplacer *d)
{
	if(d->sidendx == 0) {
		d->x -= d->spacing;
		d->y = d->ay;
	}
	else if(d->sidendx == 1) {
		d->x = d->ax;
		d->y -= d->spacing;
	}
	else if(d->sidendx == 2) {
		d->x += d->spacing;
		d->y = d->ay;
	}
	else if(d->sidendx == 3) {
		d->x = d->ax;
		d->y += d->spacing;
	}
}

void multires_displace(MultiresDisplacer *d, float co[3])
{
	float disp[3];
	float *data;

	if(!d->grid || !d->grid->disps) return;

	data = d->grid->disps[d->y * d->sidetot + d->x];

	if(d->invert)
		VecSubf(disp, co, d->subco->co);
	else
		VecCopyf(disp, data);

	{
		float norm[3];
		float mat[3][3], inv[3][3];

		norm[0] = d->subco->no[0] / 32767.0f;
		norm[1] = d->subco->no[1] / 32767.0f;
		norm[2] = d->subco->no[2] / 32767.0f;

		calc_ts_mat(mat, d->mat_center, d->mat_target, norm);
		if(d->invert) {
			Mat3Inv(inv, mat);
			Mat3CpyMat3(mat, inv);
		}
			
		Mat3MulVecfl(mat, disp);
	}

	if(d->invert) {
		VecCopyf(data, disp);
		
	}
	else {
		if(d->type == 4 || d->type == 5)
			VecMulf(disp, d->weight);
		VecAddf(co, co, disp);
	}

	if(d->type == 2) {
		if(d->sidendx == 0)
			d->y -= d->spacing;
		else if(d->sidendx == 1)
			d->x += d->spacing;
		else if(d->sidendx == 2)
			d->y += d->spacing;
		else if(d->sidendx == 3)
			d->x -= d->spacing;
	}
	else if(d->type == 3) {
		if(d->sidendx == 0)
			d->y -= d->spacing;
		else if(d->sidendx == 1)
			d->x += d->spacing;
		else if(d->sidendx == 2)
			d->y += d->spacing;
		else if(d->sidendx == 3)
			d->x -= d->spacing;
	}
}

/* Returns 0 on success, 1 if the src's totvert doesn't match */
int multiresModifier_reshape(MultiresModifierData *mmd, Object *dst, Object *src)
{
	Mesh *src_me = get_mesh(src);
	DerivedMesh *mrdm = dst->derivedFinal;

	if(mrdm && mrdm->getNumVerts(mrdm) == src_me->totvert) {
		MVert *mvert = CDDM_get_verts(mrdm);
		int i;

		for(i = 0; i < src_me->totvert; ++i)
			VecCopyf(mvert[i].co, src_me->mvert[i].co);
		mrdm->needsFree = 1;
		mrdm->release(mrdm);
		dst->derivedFinal = NULL;

		return 0;
	}

	return 1;
}

static void multiresModifier_disp_run(DerivedMesh *dm, MVert *subco, int invert)
{
	const int lvl = MultiresDM_get_lvl(dm);
	const int gridFaces = multires_side_tot[lvl - 2] - 1;
	const int edgeSize = multires_side_tot[lvl - 1] - 1;
	MVert *mvert = CDDM_get_verts(dm);
	MEdge *medge = MultiresDM_get_mesh(dm)->medge;
	MFace *mface = MultiresDM_get_mesh(dm)->mface;
	ListBase *map = MultiresDM_get_vert_face_map(dm);
	MultiresDisplacer d;
	int i, S, x, y;

	if(subco)
		d.subco = subco;

	for(i = 0; i < MultiresDM_get_mesh(dm)->totface; ++i) {
		const int numVerts = mface[i].v4 ? 4 : 3;
			
		multires_displacer_init(&d, dm, i, invert);
		multires_displacer_anchor(&d, 1, 0);
		multires_displace(&d, mvert->co);
		++mvert;
		++d.subco;

		for(S = 0; S < numVerts; ++S) {
			multires_displacer_anchor(&d, 2, S);
			for(x = 1; x < gridFaces; ++x) {
				multires_displace(&d, mvert->co);
				++mvert;
				++d.subco;
			}
		}

		for(S = 0; S < numVerts; S++) {
			multires_displacer_anchor(&d, 3, S);
			for(y = 1; y < gridFaces; y++) {
				for(x = 1; x < gridFaces; x++) {
					multires_displace(&d, mvert->co);
					++mvert;
					++d.subco;
				}
				multires_displacer_jump(&d);
			}
		}
	}

	for(i = 0; i < MultiresDM_get_mesh(dm)->totedge; ++i) {
		const MEdge *e = &medge[i];
		for(x = 1; x < edgeSize; ++x) {
			IndexNode *n1, *n2;
			int numFaces = 0;
			for(n1 = map[e->v1].first; n1; n1 = n1->next) {
				for(n2 = map[e->v2].first; n2; n2 = n2->next) {
					if(n1->index == n2->index)
						++numFaces;
				}
			}
			multires_displacer_weight(&d, 1.0f / numFaces);
			/* TODO: Better to have these loops outside the x loop */
			for(n1 = map[e->v1].first; n1; n1 = n1->next) {
				for(n2 = map[e->v2].first; n2; n2 = n2->next) {
					if(n1->index == n2->index) {
						multires_displacer_init(&d, dm, n1->index, invert);
						multires_displacer_anchor_edge(&d, e->v1, e->v2, x);
						multires_displace(&d, mvert->co);
					}
				}
			}
			++mvert;
			++d.subco;
		}
	}
		
	for(i = 0; i < MultiresDM_get_mesh(dm)->totvert; ++i) {
		IndexNode *n;
		multires_displacer_weight(&d, 1.0f / BLI_countlist(&map[i]));
		for(n = map[i].first; n; n = n->next) {
			multires_displacer_init(&d, dm, n->index, invert);
			multires_displacer_anchor_vert(&d, i);
			multires_displace(&d, mvert->co);
		}
		++mvert;
		++d.subco;
	}

	if(!invert)
		CDDM_calc_normals(dm);
}

static void multiresModifier_update(DerivedMesh *dm)
{
	MDisps *mdisps;
	MVert *mvert;
	MEdge *medge;
	MFace *mface;
	int i;

	if(!(G.f & G_SCULPTMODE) && !(*MultiresDM_get_flags(dm) & MULTIRES_DM_UPDATE_ALWAYS)) return;

	mdisps = dm->getFaceDataArray(dm, CD_MDISPS);

	if(mdisps) {
		SubsurfModifierData smd;
		const int lvl = MultiresDM_get_lvl(dm);
		const int totlvl = MultiresDM_get_totlvl(dm);
		Mesh *me = MultiresDM_get_mesh(dm);
		DerivedMesh *orig, *subco_dm;
		
		mvert = CDDM_get_verts(dm);
		medge = MultiresDM_get_mesh(dm)->medge;
		mface = MultiresDM_get_mesh(dm)->mface;

		orig = CDDM_from_mesh(me, NULL);

		if(lvl < totlvl) {
			/* Propagate disps upwards */
			DerivedMesh *final;
			MVert *verts_new;
			MultiresModifierData mmd;
			MVert *cur_lvl_orig_verts = NULL;
			
			/* Regenerate the current level's vertex coordinates without sculpting */
			mmd.totlvl = totlvl;
			mmd.lvl = lvl;
			subco_dm = multires_dm_create_from_derived(&mmd, orig, me, 0, 0);
			*MultiresDM_get_flags(subco_dm) |= MULTIRES_DM_UPDATE_BLOCK;
			cur_lvl_orig_verts = CDDM_get_verts(subco_dm);

			/* Subtract the original vertex cos from the new vertex cos */
			verts_new = CDDM_get_verts(dm);
			for(i = 0; i < dm->getNumVerts(dm); ++i)
				VecSubf(verts_new[i].co, verts_new[i].co, cur_lvl_orig_verts[i].co);

			final = multires_subdisp_pre(dm, totlvl - lvl);

			multires_subdisp(orig, me, final, lvl, totlvl, dm->getNumVerts(dm), dm->getNumEdges(dm),
					 dm->getNumFaces(dm), 1);
		}
		else {
			/* Regenerate the current level's vertex coordinates without displacements */
			memset(&smd, 0, sizeof(SubsurfModifierData));
			smd.levels = lvl - 1;
			subco_dm = subsurf_make_derived_from_derived_with_multires(orig, &smd, NULL, 0, NULL, 0, 0);

			multiresModifier_disp_run(dm, CDDM_get_verts(subco_dm), 1);
		}
		
		orig->release(orig);
		subco_dm->release(subco_dm);
	}
}

void multires_force_update(Object *ob)
{
	if(ob->derivedFinal) {
		ob->derivedFinal->needsFree =1;
		ob->derivedFinal->release(ob->derivedFinal);
		ob->derivedFinal = NULL;
	}
}

struct DerivedMesh *multires_dm_create_from_derived(MultiresModifierData *mmd, DerivedMesh *dm, Mesh *me,
						    int useRenderParams, int isFinalCalc)
{
	SubsurfModifierData smd;
	MultiresSubsurf ms = {me, mmd->totlvl, mmd->lvl};
	DerivedMesh *result;
	int i;

	memset(&smd, 0, sizeof(SubsurfModifierData));
	smd.levels = smd.renderLevels = mmd->lvl - 1;

	result = subsurf_make_derived_from_derived_with_multires(dm, &smd, &ms, useRenderParams, NULL, isFinalCalc, 0);
	for(i = 0; i < result->getNumVerts(result); ++i)
		MultiresDM_get_subco(result)[i] = CDDM_get_verts(result)[i];
	multiresModifier_disp_run(result, MultiresDM_get_subco(result), 0);
	MultiresDM_set_update(result, multiresModifier_update);

	return result;
}
