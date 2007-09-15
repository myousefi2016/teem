/*
  Teem: Tools to process and visualize scientific data and images
  Copyright (C) 2006, 2005  Gordon Kindlmann
  Copyright (C) 2004, 2003, 2002, 2001, 2000, 1999, 1998  University of Utah

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public License
  (LGPL) as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  The terms of redistributing and/or modifying this software also
  include exceptions to the LGPL that facilitate static linking.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "gage.h"
#include "privateGage.h"

/*
******** gageContextNew()
**
** doesn't use biff
*/
gageContext *
gageContextNew() {
  gageContext *ctx;
  int i;
  
  ctx = (gageContext*)calloc(1, sizeof(gageContext));
  if (ctx) {
    ctx->verbose = gageDefVerbose;
    gageParmReset(&ctx->parm);
    for(i=gageKernelUnknown+1; i<gageKernelLast; i++) {
      ctx->ksp[i] = NULL;
    }
    for (i=0; i<GAGE_PERVOLUME_NUM; i++) {
      ctx->pvl[i] = NULL;
    }
    ctx->pvlNum = 0;
    ctx->stackIdx = AIR_NAN;
    ctx->stackKsp = NULL;
    gageKernelReset(ctx); /* placed here for logic of kernel flag */
    ctx->shape = gageShapeNew();
    for (i=0; i<GAGE_CTX_FLAG_NUM; i++) {
      ctx->flag[i] = AIR_FALSE;
    }
    ctx->needD[0] = ctx->needD[1] = ctx->needD[2] = AIR_FALSE;
    for (i=gageKernelUnknown+1; i<gageKernelLast; i++) {
      ctx->needK[i] = AIR_FALSE;
    }
    ctx->radius = -1;
    ctx->fsl = ctx->fw = NULL;
    ctx->off = NULL;
    gagePointReset(&ctx->point);
  }
  return ctx;
}

/*
******** gageContextCopy()
**
** gives you a new context, which behaves the same as the given context,
** with newly allocated pervolumes attached.  With the avoidance of
** padding to create a private copy of the volume, the gageContext is
** light-weight enough that there is no reason that this function can't
** return an independent and fully functioning copy of the context (whereas
** before you weren't allowed to do anything but gageProbe() on the on
** copied context).
*/
gageContext *
gageContextCopy(gageContext *ctx) {
  char me[]="gageContextCopy", err[BIFF_STRLEN];
  gageContext *ntx;
  int fd, i;
  unsigned int pvlIdx;

  ntx = (gageContext*)calloc(1, sizeof(gageContext));
  if (!ntx) {
    sprintf(err, "%s: couldn't make a gageContext", me);
    biffAdd(GAGE, err); return NULL;
  }
  /* we should probably restrict ourselves to gage API calls, but given the
     constant state of gage construction, this seems much simpler.
     Pointers are fixed below */
  memcpy(ntx, ctx, sizeof(gageContext));
  for (i=0; i<GAGE_KERNEL_NUM; i++) {
    ntx->ksp[i] = nrrdKernelSpecCopy(ctx->ksp[i]);
  }
  ntx->stackKsp = nrrdKernelSpecCopy(ctx->stackKsp);
  for (pvlIdx=0; pvlIdx<ntx->pvlNum; pvlIdx++) {
    ntx->pvl[pvlIdx] = _gagePerVolumeCopy(ctx->pvl[pvlIdx], 2*ctx->radius);
    if (!ntx->pvl[pvlIdx]) {
      sprintf(err, "%s: trouble copying pervolume %u", me, pvlIdx);
      biffAdd(GAGE, err); return NULL;
    }
  }
  ntx->shape = gageShapeCopy(ctx->shape);
  fd = 2*ntx->radius;
  ntx->fsl = (double *)calloc(fd*3, sizeof(double));
  ntx->fw = (double *)calloc(fd*3*GAGE_KERNEL_NUM, sizeof(double));
  ntx->off = (unsigned int *)calloc(fd*fd*fd, sizeof(unsigned int));
  if (!( ntx->fsl && ntx->fw && ntx->off )) {
    sprintf(err, "%s: couldn't allocate new filter caches for fd=%d",
            me, fd);
    biffAdd(GAGE, err); return NULL;
  }
  /* the content of the offset array needs to be copied because
     it won't be refilled simply by calls to gageProbe() */
  memcpy(ntx->off, ctx->off, fd*fd*fd*sizeof(unsigned int));

  /* make sure gageProbe() has to refill caches */
  gagePointReset(&ntx->point);

  return ntx;
}

/*
******** gageContextNix()
**
** responsible for freeing and clearing up everything hanging off a 
** context so that things can be returned to the way they were prior
** to gageContextNew().
**
** does not use biff
*/
gageContext *
gageContextNix(gageContext *ctx) {
  /* char me[]="gageContextNix"; */
  unsigned int pvlIdx;

  if (ctx) {
    gageKernelReset(ctx);
    for (pvlIdx=0; pvlIdx<ctx->pvlNum; pvlIdx++) {
      gagePerVolumeNix(ctx->pvl[pvlIdx]);
      /* no point in doing a detach, the whole context is going bye-bye */
    }
    ctx->shape = gageShapeNix(ctx->shape);
    ctx->fw = (double *)airFree(ctx->fw);
    ctx->fsl = (double *)airFree(ctx->fsl);
    ctx->off = (unsigned int *)airFree(ctx->off);
  }
  airFree(ctx);
  return NULL;
}

/*
******** gageKernelSet()
**
** sets one kernel in a gageContext; but the value of this function
** is all the error checking it does.
**
** Refers to ctx->checkIntegrals and acts appropriately.
**
** Does use biff.
**
** This is also the brains of gageStackKernelSet(), as invoked by
** passing which == GAGE_KERNEL_STACK
*/
int
gageKernelSet(gageContext *ctx, 
              int which, const NrrdKernel *k, const double *kparm) {
  char me[]="gageKernelSet", err[BIFF_STRLEN];
  int numParm;
  double support, integral;

  if (!(ctx && k && kparm)) {
    sprintf(err, "%s: got NULL pointer", me);
    biffAdd(GAGE, err); return 1;
  }
  if (GAGE_KERNEL_STACK != which) {
    if (airEnumValCheck(gageKernel, which)) {
      sprintf(err, "%s: \"which\" (%d) not in range [%d,%d]", me,
              which, gageKernelUnknown+1, gageKernelLast-1);
      biffAdd(GAGE, err); return 1;
    }
  }
  if (ctx->verbose) {
    if (GAGE_KERNEL_STACK != which) {
      fprintf(stderr, "%s: which = %d -> %s\n", me, which,
              airEnumStr(gageKernel, which));
    } else {
      fprintf(stderr, "%s: which = %d -> stack\n", me, which);
    }
  }
  numParm = k->numParm;
  if (!(AIR_IN_CL(0, numParm, NRRD_KERNEL_PARMS_NUM))) {
    sprintf(err, "%s: kernel's numParm (%d) not in range [%d,%d]",
            me, numParm, 0, NRRD_KERNEL_PARMS_NUM);
    biffAdd(GAGE, err); return 1;
  }
  support = k->support(kparm);
  if (!( support > 0 )) {
    sprintf(err, "%s: kernel's support (%g) not > 0", me, support);
    biffAdd(GAGE, err); return 1;
  }
  if (ctx->parm.checkIntegrals) {
    integral = k->integral(kparm);
    if (gageKernel00 == which ||
        gageKernel10 == which ||
        gageKernel20 == which ||
        GAGE_KERNEL_STACK == which) {
      if (!( integral > 0 )) {
        sprintf(err, "%s: reconstruction kernel's integral (%g) not > 0.0",
                me, integral);
        biffAdd(GAGE, err); return 1;
      }
    } else {
      /* its a derivative, so integral must be near zero */
      if (!( AIR_ABS(integral) <= ctx->parm.kernelIntegralNearZero )) {
        sprintf(err, "%s: derivative kernel's integral (%g) not within "
                "%g of 0.0",
                me, integral, ctx->parm.kernelIntegralNearZero);
        biffAdd(GAGE, err); return 1;
      }
    }
  }

  /* okay, enough enough, go set the kernel */
  if (GAGE_KERNEL_STACK != which) {
    if (!ctx->ksp[which]) {
      ctx->ksp[which] = nrrdKernelSpecNew();
    }
    nrrdKernelSpecSet(ctx->ksp[which], k, kparm);
  } else {
    if (!ctx->stackKsp) {
      ctx->stackKsp = nrrdKernelSpecNew();
    }
    nrrdKernelSpecSet(ctx->stackKsp, k, kparm);
  }
  ctx->flag[gageCtxFlagKernel] = AIR_TRUE;

  return 0;
}

int
gageStackKernelSet(gageContext *ctx, 
                   const NrrdKernel *k, const double *kparm) {
  char me[]="gageStackKernelSet", err[BIFF_STRLEN];

  if (gageKernelSet(ctx, GAGE_KERNEL_STACK, k, kparm)) {
    sprintf(err, "%s: trouble", me);
    biffAdd(GAGE, err); return 1;
  }

  return 0;
}

/*
******** gageKernelReset()
**
** reset kernels (including stackKsp) and parameters.
*/
void
gageKernelReset(gageContext *ctx) {
  /* char me[]="gageKernelReset"; */
  int i;

  if (ctx) {
    for(i=gageKernelUnknown+1; i<gageKernelLast; i++) {
      ctx->ksp[i] = nrrdKernelSpecNix(ctx->ksp[i]);
    }
    ctx->stackKsp = nrrdKernelSpecNix(ctx->stackKsp);
    ctx->flag[gageCtxFlagKernel] = AIR_TRUE;
  }
  return;
}

/*
******** gageParmSet()
**
** for setting the boolean-ish flags in the context in a safe and
** intelligent manner, since changing some of them can have many
** consequences
*/
void
gageParmSet(gageContext *ctx, int which, double val) {
  char me[]="gageParmSet";
  unsigned int pvlIdx;
  
  switch (which) {
  case gageParmVerbose:
    ctx->verbose = AIR_CAST(int, val);
    for (pvlIdx=0; pvlIdx<ctx->pvlNum; pvlIdx++) {
      ctx->pvl[pvlIdx]->verbose = AIR_CAST(int, val);
    }
    break;
  case gageParmRenormalize:
    ctx->parm.renormalize = val ? AIR_TRUE : AIR_FALSE;
    /* we have to make sure that any existing filter weights
       are not re-used; because gageUpdage() is not called mid-probing,
       we don't use the flag machinery.  Instead we just invalidate
       the last known fractional probe locations */
    gagePointReset(&ctx->point);
    break;
  case gageParmCheckIntegrals:
    ctx->parm.checkIntegrals = val ? AIR_TRUE : AIR_FALSE;
    /* no flags to set, simply affects future calls to gageKernelSet() */
    break;
  case gageParmK3Pack:
    ctx->parm.k3pack = val ? AIR_TRUE : AIR_FALSE;
    ctx->flag[gageCtxFlagK3Pack] = AIR_TRUE;
    break;
  case gageParmGradMagMin:
    ctx->parm.gradMagMin = val;
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmGradMagCurvMin:
    ctx->parm.gradMagCurvMin = val;
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmDefaultSpacing:
    ctx->parm.defaultSpacing = val;
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmCurvNormalSide:
    ctx->parm.curvNormalSide = AIR_CAST(int, val);
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmKernelIntegralNearZero:
    ctx->parm.kernelIntegralNearZero = val;
    /* no flag to set, simply affects future calls to gageKernelSet() */
    break;
  case gageParmRequireAllSpacings:
    ctx->parm.requireAllSpacings = AIR_CAST(int, val);
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmRequireEqualCenters:
    ctx->parm.requireEqualCenters = AIR_CAST(int, val);
    /* no flag to set, simply affects future calls to gageProbe() */
    break;
  case gageParmDefaultCenter:
    ctx->parm.defaultCenter = AIR_CAST(int, val);
    /* no flag to set, I guess, although the value here effects the 
       action of _gageShapeSet when called by gagePerVolumeAttach ... */
    break;
  case gageParmStackUse:
    ctx->parm.stackUse = AIR_CAST(int, val);
    /* no flag to set, right? simply affects future calls to gageProbe()? */
    /* HEY: no? because if you're turning on the stack behavior, you now
       should be doing the error checking to make sure that all the pvls
       have the same kind!  This should be caught by gageUpdate(), which is
       supposed to be called after changing anything, prior to gageProbe() */
    break;
  default:
    fprintf(stderr, "\n%s: which = %d not valid!!\n\n", me, which);
    break;
  }
  return;
}

/*
******** gagePerVolumeIsAttached()
**
*/
int
gagePerVolumeIsAttached(const gageContext *ctx, const gagePerVolume *pvl) {
  int ret;
  unsigned int pvlIdx;

  ret = AIR_FALSE;
  for (pvlIdx=0; pvlIdx<ctx->pvlNum; pvlIdx++) {
    if (pvl == ctx->pvl[pvlIdx]) {
      ret = AIR_TRUE;
    }
  }
  return ret;
}

/*
******** gagePerVolumeAttach()
**
** attaches a pervolume to a context, which actually involves 
** very little work
*/
int
gagePerVolumeAttach(gageContext *ctx, gagePerVolume *pvl) {
  char me[]="gagePerVolumeAttach", err[BIFF_STRLEN];
  gageShape *shape;

  if (!( ctx && pvl )) {
    sprintf(err, "%s: got NULL pointer", me);
    biffAdd(GAGE, err); return 1;
  }
  if (gagePerVolumeIsAttached(ctx, pvl)) {
    sprintf(err, "%s: given pervolume already attached", me);
    biffAdd(GAGE, err); return 1;
  }
  if (ctx->pvlNum == GAGE_PERVOLUME_NUM) {
    sprintf(err, "%s: sorry, already have GAGE_PERVOLUME_NUM == %d "
            "pervolumes attached", me, GAGE_PERVOLUME_NUM);
    biffAdd(GAGE, err); return 1;
  }

  if (0 == ctx->pvlNum) {
    /* the volume "shape" is context state that we set now, because unlike 
       everything else (handled by gageUpdate()), it does not effect
       the kind or amount of padding done */
    if (_gageShapeSet(ctx, ctx->shape, pvl->nin, pvl->kind->baseDim)) {
      sprintf(err, "%s: trouble", me); 
      biffAdd(GAGE, err); return 1;
    }
    ctx->flag[gageCtxFlagShape] = AIR_TRUE;
  } else {
    /* have to check to that new pvl matches first one.  Since all
       attached pvls were at some point the "new" one, they all
       should match each other */
    shape = gageShapeNew();
    if (_gageShapeSet(ctx, shape, pvl->nin, pvl->kind->baseDim)) {
      sprintf(err, "%s: trouble", me); 
      biffAdd(GAGE, err); return 1;
    }
    if (!gageShapeEqual(ctx->shape, "existing context", shape, "new volume")) {
      sprintf(err, "%s: trouble", me);
      biffAdd(GAGE, err); gageShapeNix(shape); return 1;
    }
    gageShapeNix(shape); 
  }
  /* here we go */
  ctx->pvl[ctx->pvlNum++] = pvl;
  pvl->verbose = ctx->verbose;

  return 0;
}

/*
******** gagePerVolumeDetach()
**
** detaches a pervolume from a context, but does nothing else
** with the pervolume; caller may want to call gagePerVolumeNix
** if this pervolume will no longer be used
*/
int
gagePerVolumeDetach(gageContext *ctx, gagePerVolume *pvl) {
  char me[]="gagePerVolumeDetach", err[BIFF_STRLEN];
  unsigned int pvlIdx, foundIdx=0;

  if (!( ctx && pvl )) {
    sprintf(err, "%s: got NULL pointer", me);
    biffAdd(GAGE, err); return 1;
  }
  if (!gagePerVolumeIsAttached(ctx, pvl)) {
    sprintf(err, "%s: given pervolume not currently attached", me);
    biffAdd(GAGE, err); return 1;
  }
  for (pvlIdx=0; pvlIdx<ctx->pvlNum; pvlIdx++) {
    if (pvl == ctx->pvl[pvlIdx]) {
      foundIdx = pvlIdx;
    }
  }
  for (pvlIdx=foundIdx+1; pvlIdx<ctx->pvlNum; pvlIdx++) {
    ctx->pvl[pvlIdx-1] = ctx->pvl[pvlIdx];
  }
  ctx->pvl[ctx->pvlNum--] = NULL;
  if (0 == ctx->pvlNum) {
    /* leave things the way that they started */
    gageShapeReset(ctx->shape);
    ctx->flag[gageCtxFlagShape] = AIR_TRUE;
  }
  return 0;
}

/*
** gageIv3Fill()
**
** based on ctx's shape and radius, and the (xi,yi,zi) determined from
** the probe location, fills the iv3 cache in the given pervolume
**
** This function is a bottleneck for so many things, so forcing off
** the verbose comments seems like one way of trying to speed it up.
** However, temporarily if-def'ing out unused branches of the
** "switch(pvl->kind->valLen)", and #define'ing fddd to 64 (for 4x4x4
** neighborhoods) did not noticeably speed anything up.
**
*/
void
gageIv3Fill(gageContext *ctx, gagePerVolume *pvl) {
  char me[]="gageIv3Fill";
  int _xx, _yy, _zz, xx, yy, zz, lx, ly, lz,
    hx, hy, hz, fr, cacheIdx, dataIdx, fddd;
  unsigned int sx, sy, sz;
  char *data, *here;
  unsigned int tup;

  if (0 && ctx->verbose) {
    fprintf(stderr, "%s: hello\n", me);
  }
  sx = ctx->shape->size[0];
  sy = ctx->shape->size[1];
  sz = ctx->shape->size[2];
  fr = ctx->radius;
  lx = ctx->point.xi - (fr - 1);
  ly = ctx->point.yi - (fr - 1);
  lz = ctx->point.zi - (fr - 1);
  hx = lx + 2*fr - 1;
  hy = ly + 2*fr - 1;
  hz = lz + 2*fr - 1;
  fddd = 2*fr*2*fr*2*fr;
  data = (char*)pvl->nin->data;
  if (lx >= 0 && ly >= 0 && lz >= 0 
      && hx < AIR_CAST(int, sx)
      && hy < AIR_CAST(int, sy)
      && hz < AIR_CAST(int, sz)) {
    /* all the samples we need are inside the existing volume */
    dataIdx = lx + sx*(ly + sy*(lz));
    if (0 && ctx->verbose) {
      fprintf(stderr, "%s: hello, valLen = %d, pvl->nin = %p, data = %p\n",
              me, pvl->kind->valLen,
              AIR_CAST(void*, pvl->nin), pvl->nin->data);
    }
    here = data + dataIdx*pvl->kind->valLen*nrrdTypeSize[pvl->nin->type];
    if (0 && ctx->verbose) {
      fprintf(stderr, "%s: size = (%u,%u,%u);\n"
              "  fd = %d; coord = (%d,%d,%d) --> dataIdx = %d\n",
              me, sx, sy, sz, 2*fr,
              ctx->point.xi, ctx->point.yi, ctx->point.zi,
              dataIdx);
      fprintf(stderr, "%s: here = %p; iv3 = %p; off[0,1,2,3,4,5,6,7] = "
              "%d,%d,%d,%d,%d,%d,%d,%d\n",
              me, here, AIR_CAST(void*, pvl->iv3),
              ctx->off[0], ctx->off[1], ctx->off[2], ctx->off[3],
              ctx->off[4], ctx->off[5], ctx->off[6], ctx->off[7]);
    }
    switch(pvl->kind->valLen) {
    case 1:
      for (cacheIdx=0; cacheIdx<fddd; cacheIdx++) {
        pvl->iv3[cacheIdx] = pvl->lup(here, ctx->off[cacheIdx]);
      }
      break;
      /* NOTE: the tuple axis is being shifted from the fastest to
         the slowest axis, to anticipate component-wise filtering
         operations */
    case 3:
      for (cacheIdx=0; cacheIdx<fddd; cacheIdx++) {
        pvl->iv3[cacheIdx + fddd*0] = pvl->lup(here, 0 + 3*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*1] = pvl->lup(here, 1 + 3*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*2] = pvl->lup(here, 2 + 3*ctx->off[cacheIdx]);
      }
      break;
    case 7:
      /* this might come in handy for tenGage ... */
      for (cacheIdx=0; cacheIdx<fddd; cacheIdx++) {
        pvl->iv3[cacheIdx + fddd*0] = pvl->lup(here, 0 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*1] = pvl->lup(here, 1 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*2] = pvl->lup(here, 2 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*3] = pvl->lup(here, 3 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*4] = pvl->lup(here, 4 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*5] = pvl->lup(here, 5 + 7*ctx->off[cacheIdx]);
        pvl->iv3[cacheIdx + fddd*6] = pvl->lup(here, 6 + 7*ctx->off[cacheIdx]);
      }
      break;
    default:
      for (cacheIdx=0; cacheIdx<fddd; cacheIdx++) {
        for (tup=0; tup<pvl->kind->valLen; tup++) {
          pvl->iv3[cacheIdx + fddd*tup] = 
            pvl->lup(here, tup + pvl->kind->valLen*ctx->off[cacheIdx]);
        }
      }
      break;
    }
  } else {
    /* the query requires samples which don't actually lie 
       within the volume- more care has to be taken */
    cacheIdx = 0;
    for (_zz=lz; _zz<=hz; _zz++) {
      zz = AIR_CLAMP(0, _zz, AIR_CAST(int, sz-1));
      for (_yy=ly; _yy<=hy; _yy++) {
        yy = AIR_CLAMP(0, _yy, AIR_CAST(int, sy-1));
        for (_xx=lx; _xx<=hx; _xx++) {
          xx = AIR_CLAMP(0, _xx, AIR_CAST(int, sx-1));
          dataIdx = xx + sx*(yy + sy*zz);
          here = data + dataIdx*pvl->kind->valLen*nrrdTypeSize[pvl->nin->type];
          for (tup=0; tup<pvl->kind->valLen; tup++) {
            pvl->iv3[cacheIdx + fddd*tup] = pvl->lup(here, tup);
          }
          cacheIdx++;
        }
      }
    }
  }
  if (0 && ctx->verbose) {
    fprintf(stderr, "%s: bye\n", me);
  }
  return;
}

/*
******** gageStackProbe()
**
** sets the location in the stack for the next probe gageProbe()
**
** sidx is index-space position in the stack
*/
int
gageStackProbe(gageContext *ctx, double sidx) {
  char me[]="gageStackProbe";

  if (!ctx) {
    return 1;
  }
  if (!ctx->parm.stackUse) {
    sprintf(ctx->errStr, "%s: can't probe stack without parm.stackUse", me);
    ctx->errNum = 1;
    return 1;
  }
  if (!AIR_EXISTS(sidx)) {
    sprintf(ctx->errStr, "%s: stack index %g doesn't exist", me, sidx);
    ctx->errNum = 2;
    return 1;
  }
  
  ctx->stackIdx = sidx;
  return 0;
}

/*
** _gageStackIv3Fill
**
** after the individual iv3's in the stack have been filled, 
** this does the across-stack filtering to fill pvl[0]'s iv3
*/
int
_gageStackIv3Fill(gageContext *ctx) {
  char me[]="_gageStackIv3Fill";
  unsigned int fr, fd, ii, fddd, valLen, pvlIdx, cacheIdx;
  int stackBase, _pvlIdx;
  double *kparm, val, fslw[GAGE_STACK_NUM_MAX], stackFrac, sum;

  fr = AIR_CAST(unsigned int, ctx->radius);
  fd = 2*fr;
  fddd = fd*fd*fd;
  valLen = ctx->pvl[0]->kind->valLen;
  kparm = ctx->stackKsp->parm;

  if (!AIR_EXISTS(ctx->stackIdx)) {
    sprintf(ctx->errStr, "%s: stackIdx %g doesn't exist", me, ctx->stackIdx);
    ctx->errNum = 10;
    return 1;
  }
  if (!( AIR_IN_CL(-0.5, ctx->stackIdx, ctx->pvlNum - 1.5) )) {
    sprintf(ctx->errStr, "%s: stackIdx %g outside [%g,%g]", me, ctx->stackIdx,
            -0.5, ctx->pvlNum - 1.5);
    ctx->errNum = 11;
    return 1;
  }
  /* cell-centered sampling of stack indices from 0 to ctx->pvlNum-2 */
  stackBase = AIR_CAST(int, ctx->stackIdx+1) - 1;
  stackFrac = ctx->stackIdx - stackBase;
  /*
  fprintf(stderr, "!%s: stackIdx = %g -> base,frac = %d, %g\n", me,
          ctx->stackIdx, stackBase, stackFrac);
  */
  for (ii=0; ii<fd; ii++) {
    fslw[ii] = stackFrac - ii + fr-1;
  }  
  ctx->stackKsp->kernel->evalN_d(fslw, fslw, fd, kparm);

  /* renormalize weights */
  sum = 0;
  for (ii=0; ii<fd; ii++) {
    sum += fslw[ii];
  }
  for (ii=0; ii<fd; ii++) {
    fslw[ii] /= sum;
  }

  /* NOTE we are treating the 4D fd*fd*fd*valLen iv3 as a big 1-D array */
  for (cacheIdx=0; cacheIdx<fddd*valLen; cacheIdx++) {
    val = 0;
    for (ii=0; ii<fd; ii++) {
      _pvlIdx = stackBase + ii - (fr-1);
      pvlIdx = 1 + AIR_CLAMP(0, _pvlIdx, AIR_CAST(int, ctx->pvlNum-2));
      val += fslw[ii]*ctx->pvl[pvlIdx]->iv3[cacheIdx];
      /*
      if (!cacheIdx) {
        fprintf(stderr, "!%s: ii=%d, _pvlIdx=%d, pvlIdx=%u, iv3=%g, "
                "f=%g, val=%g\n", me,
                ii, _pvlIdx, pvlIdx, ctx->pvl[pvlIdx]->iv3[cacheIdx],
                fslw[ii], val);
      }
      */
    }
    ctx->pvl[0]->iv3[cacheIdx] = val;
  }
  return 0;
}

/*
******** gageProbe()
**
** how to do probing.  (x,y,z) position is *index space* position
**
** doesn't actually do much more than call callbacks in the gageKind
** structs of the attached pervolumes
*/
int
gageProbe(gageContext *ctx, double x, double y, double z) {
  char me[]="gageProbe";
  int xi, yi, zi;
  unsigned int pvlIdx;
  
  if (!ctx) {
    return 1;
  }
  xi = ctx->point.xi;
  yi = ctx->point.yi;
  zi = ctx->point.zi;
  if (_gageLocationSet(ctx, x, y, z)) {
    /* we're outside the volume; leave gageErrStr and gageErrNum set
       (as they should be already) */
    return 1;
  }
  
  /* fprintf(stderr, "##%s: bingo 1\n", me); */
  /* if necessary, refill the iv3 cache */
  if (!( xi == ctx->point.xi &&
         yi == ctx->point.yi &&
         zi == ctx->point.zi )) {
    for (pvlIdx=(ctx->parm.stackUse ? 1 : 0); pvlIdx<ctx->pvlNum; pvlIdx++) {
      gageIv3Fill(ctx, ctx->pvl[pvlIdx]);
    }
  }
  if (ctx->parm.stackUse) {
    if (_gageStackIv3Fill(ctx)) {
      /* some problem; leave gageErrStr and gageErrNum set */
      return 1;
    }
    if (ctx->verbose > 1) {
      fprintf(stderr, "%s: stack's pvl's value cache at "
              "coords = %d,%d,%d:\n", me,
              ctx->point.xi, ctx->point.yi, ctx->point.zi);
      ctx->pvl[0]->kind->iv3Print(stderr, ctx, ctx->pvl[0]);
    }
    ctx->pvl[0]->kind->filter(ctx, ctx->pvl[0]);
    ctx->pvl[0]->kind->answer(ctx, ctx->pvl[0]);
  } else {
    /* fprintf(stderr, "##%s: bingo 2\n", me); */
    for (pvlIdx=0; pvlIdx<ctx->pvlNum; pvlIdx++) {
      if (ctx->verbose > 1) {
        fprintf(stderr, "%s: pvl[%u]'s value cache at "
                "coords = %d,%d,%d:\n", me, pvlIdx,
                ctx->point.xi, ctx->point.yi, ctx->point.zi);
        ctx->pvl[pvlIdx]->kind->iv3Print(stderr, ctx, ctx->pvl[pvlIdx]);
      }
      ctx->pvl[pvlIdx]->kind->filter(ctx, ctx->pvl[pvlIdx]);
      ctx->pvl[pvlIdx]->kind->answer(ctx, ctx->pvl[pvlIdx]);
    }
  }
  
  /* fprintf(stderr, "##%s: bingo 5\n", me); */
  return 0;
}

int
gageProbeSpace(gageContext *ctx, double x, double y, double z,
               int indexSpace, int clamp) {
  int ret;
  unsigned int *size;
  double xi, yi, zi;

  size = ctx->shape->size;
  if (indexSpace) {
    xi = x;
    yi = y;
    zi = z;
  } else {
    /* have to convert from world to index */
    /* HEY: this has to be tested/debugged */
    double icoord[4]; double wcoord[4];
    ELL_4V_SET(wcoord, x, y, z, 1);
    ELL_4MV_MUL(icoord, ctx->shape->WtoI, wcoord);
    ELL_4V_HOMOG(icoord, icoord);
    xi = icoord[0];
    yi = icoord[1];
    zi = icoord[2];
  }
  if (clamp) {
    if (nrrdCenterNode == ctx->shape->center) {
      xi = AIR_CLAMP(0, xi, size[0]-1);
      yi = AIR_CLAMP(0, yi, size[1]-1);
      zi = AIR_CLAMP(0, zi, size[2]-1);
    } else {
      xi = AIR_CLAMP(-0.5, xi, size[0]-0.5);
      yi = AIR_CLAMP(-0.5, yi, size[1]-0.5);
      zi = AIR_CLAMP(-0.5, zi, size[2]-0.5);
    }
  }
  ret = gageProbe(ctx, xi, yi, zi);
  return ret;
}
