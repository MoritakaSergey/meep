/* Copyright (C) 2005-2019 Massachusetts Institute of Technology.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* this file implements multilevel atomic materials for Meep */

#include <stdlib.h>
#include <string.h>
#include "meep.hpp"
#include "meep_internals.hpp"
#include "config.h"

namespace meep {

multilevel_susceptibility::multilevel_susceptibility(int theL, int theT, const realnum *theGamma,
                                                     const realnum *theN0, const realnum *thealpha,
                                                     const realnum *theomega,
                                                     const realnum *thegamma,
                                                     const realnum *thesigmat) {
  L = theL;
  T = theT;
  Gamma = new realnum[L * L];
  memcpy(Gamma, theGamma, sizeof(realnum) * L * L);
  N0 = new realnum[L];
  memcpy(N0, theN0, sizeof(realnum) * L);
  alpha = new realnum[L * T];
  memcpy(alpha, thealpha, sizeof(realnum) * L * T);
  omega = new realnum[T];
  memcpy(omega, theomega, sizeof(realnum) * T);
  gamma = new realnum[T];
  memcpy(gamma, thegamma, sizeof(realnum) * T);
  sigmat = new realnum[T * 5];
  memcpy(sigmat, thesigmat, sizeof(realnum) * T * 5);
}

multilevel_susceptibility::multilevel_susceptibility(const multilevel_susceptibility &from)
    : susceptibility(from) {
  L = from.L;
  T = from.T;
  Gamma = new realnum[L * L];
  memcpy(Gamma, from.Gamma, sizeof(realnum) * L * L);
  N0 = new realnum[L];
  memcpy(N0, from.N0, sizeof(realnum) * L);
  alpha = new realnum[L * T];
  memcpy(alpha, from.alpha, sizeof(realnum) * L * T);
  omega = new realnum[T];
  memcpy(omega, from.omega, sizeof(realnum) * T);
  gamma = new realnum[T];
  memcpy(gamma, from.gamma, sizeof(realnum) * T);
  sigmat = new realnum[T * 5];
  memcpy(sigmat, from.sigmat, sizeof(realnum) * T * 5);
}

multilevel_susceptibility::~multilevel_susceptibility() {
  delete[] Gamma;
  delete[] N0;
  delete[] alpha;
  delete[] omega;
  delete[] gamma;
  delete[] sigmat;
}

#if MEEP_SINGLE
#define DGETRF F77_FUNC(sgetrf, SGETRF)
#define DGETRI F77_FUNC(sgetri, SGETRI)
#else
#define DGETRF F77_FUNC(dgetrf, DGETRF)
#define DGETRI F77_FUNC(dgetri, DGETRI)
#endif
extern "C" void DGETRF(const int *m, const int *n, realnum *A, const int *lda, int *ipiv,
                       int *info);
extern "C" void DGETRI(const int *n, realnum *A, const int *lda, int *ipiv, realnum *work,
                       int *lwork, int *info);

/* S -> inv(S), where S is a p x p matrix in row-major order */
static bool invert(realnum *S, int p) {
#ifdef HAVE_LAPACK
  int info;
  int *ipiv = new int[p];
  DGETRF(&p, &p, S, &p, ipiv, &info);
  if (info < 0) abort("invalid argument %d in DGETRF", -info);
  if (info > 0) {
    delete[] ipiv;
    return false;
  } // singular

  int lwork = -1;
  realnum work1;
  DGETRI(&p, S, &p, ipiv, &work1, &lwork, &info);
  if (info != 0) abort("error %d in DGETRI workspace query", info);
  lwork = int(work1);
  realnum *work = new realnum[lwork];
  DGETRI(&p, S, &p, ipiv, work, &lwork, &info);
  if (info < 0) abort("invalid argument %d in DGETRI", -info);

  delete[] work;
  delete[] ipiv;
  return info == 0;
#else /* !HAVE_LAPACK */
  abort("LAPACK is needed for multilevel-atom support");
  return false;
#endif
}

typedef realnum *realnumP;
typedef struct {
  size_t sz_data;
  size_t ntot;
  realnum *GammaInv;                    // inv(1 + Gamma * dt / 2)
  realnumP *P[NUM_FIELD_COMPONENTS][2]; // P[c][cmp][transition][i]
  realnumP *P_prev[NUM_FIELD_COMPONENTS][2];
  realnum *N;    // ntot x L array of centered grid populations N[i*L + level]
  realnum *Ntmp; // temporary length L array of levels, used in updating
  realnum data[1];
} multilevel_data;

void *multilevel_susceptibility::new_internal_data(realnum *W[NUM_FIELD_COMPONENTS][2],
                                                   const grid_volume &gv) const {
  size_t num = 0; // number of P components
  FOR_COMPONENTS(c) DOCMP2 {
    if (needs_P(c, cmp, W)) num += 2 * gv.ntot();
  }
  size_t sz = sizeof(multilevel_data) + sizeof(realnum) * (L * L + L + gv.ntot() * L + num * T - 1);
  multilevel_data *d = (multilevel_data *)malloc(sz);
  memset(d, 0, sz);
  d->sz_data = sz;
  return (void *)d;
}

void multilevel_susceptibility::init_internal_data(realnum *W[NUM_FIELD_COMPONENTS][2], double dt,
                                                   const grid_volume &gv, void *data) const {
  multilevel_data *d = (multilevel_data *)data;
  size_t sz_data = d->sz_data;
  memset(d, 0, sz_data);
  d->sz_data = sz_data;
  size_t ntot = d->ntot = gv.ntot();

  /* d->data points to a big block of data that holds GammaInv, P,
     P_prev, Ntmp, and N.  We also initialize a bunch of convenience
     pointer in d to point to the corresponding data in d->data, so
     that we don't have to remember in other functions how d->data is
     laid out. */

  d->GammaInv = d->data;
  for (int i = 0; i < L; ++i)
    for (int j = 0; j < L; ++j)
      d->GammaInv[i * L + j] = (i == j) + Gamma[i * L + j] * dt / 2;
  if (!invert(d->GammaInv, L)) abort("multilevel_susceptibility: I + Gamma*dt/2 matrix singular");

  realnum *P = d->data + L * L;
  realnum *P_prev = P + ntot;
  FOR_COMPONENTS(c) DOCMP2 {
    if (needs_P(c, cmp, W)) {
      d->P[c][cmp] = new realnumP[T];
      d->P_prev[c][cmp] = new realnumP[T];
      for (int t = 0; t < T; ++t) {
        d->P[c][cmp][t] = P;
        d->P_prev[c][cmp][t] = P_prev;
        P += 2 * ntot;
        P_prev += 2 * ntot;
      }
    }
  }

  d->Ntmp = P;
  d->N = P + L; // the last L*ntot block of the data

  // initial populations
  for (size_t i = 0; i < ntot; ++i)
    for (int l = 0; l < L; ++l)
      d->N[i * L + l] = N0[l];
}

void multilevel_susceptibility::delete_internal_data(void *data) const {
  if (data) {
    multilevel_data *d = (multilevel_data *)data;
    FOR_COMPONENTS(c) DOCMP2 {
      delete[] d->P[c][cmp];
      delete[] d->P_prev[c][cmp];
    }
    free(data);
  }
}

void *multilevel_susceptibility::copy_internal_data(void *data) const {
  multilevel_data *d = (multilevel_data *)data;
  if (!d) return 0;
  multilevel_data *dnew = (multilevel_data *)malloc(d->sz_data);
  memcpy(dnew, d, d->sz_data);
  size_t ntot = d->ntot;
  dnew->GammaInv = dnew->data;
  realnum *P = dnew->data + L * L;
  realnum *P_prev = P + ntot;
  FOR_COMPONENTS(c) DOCMP2 {
    if (d->P[c][cmp]) {
      dnew->P[c][cmp] = new realnumP[T];
      dnew->P_prev[c][cmp] = new realnumP[T];
      for (int t = 0; t < T; ++t) {
        dnew->P[c][cmp][t] = P;
        dnew->P_prev[c][cmp][t] = P_prev;
        P += 2 * ntot;
        P_prev += 2 * ntot;
      }
    }
  }
  dnew->Ntmp = P;
  dnew->N = P + L;
  return (void *)dnew;
}

int multilevel_susceptibility::num_cinternal_notowned_needed(component c,
                                                             void *P_internal_data) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  return d->P[c][0] ? T : 0;
}

realnum *multilevel_susceptibility::cinternal_notowned_ptr(int inotowned, component c, int cmp,
                                                           int n, void *P_internal_data) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  if (!d || !d->P[c][cmp] || inotowned < 0 || inotowned >= T) // never true
    return NULL;
  return d->P[c][cmp][inotowned] + n;
}

void multilevel_susceptibility::update_P(realnum *W[NUM_FIELD_COMPONENTS][2],
                                         realnum *W_prev[NUM_FIELD_COMPONENTS][2], double dt,
                                         const grid_volume &gv, void *P_internal_data) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  double dt2 = 0.5 * dt;

  // field directions and offsets for E * dP dot product.
  component cdot[3] = {Dielectric, Dielectric, Dielectric};
  ptrdiff_t o1[3], o2[3];
  int idot = 0;
  FOR_COMPONENTS(c) {
    if (d->P[c][0]) {
      if (idot == 3) abort("bug in meep: too many polarization components");
      gv.yee2cent_offsets(c, o1[idot], o2[idot]);
      cdot[idot++] = c;
    }
  }

  // update N from W and P
  realnum *GammaInv = d->GammaInv;
  realnum *Ntmp = d->Ntmp;
  LOOP_OVER_VOL_OWNED(gv, Centered, i) {
    realnum *N = d->N + i * L; // N at current point, to update

    // Ntmp = (I - Gamma * dt/2) * N
    for (int l1 = 0; l1 < L; ++l1) {
      Ntmp[l1] = 0;
      for (int l2 = 0; l2 < L; ++l2) {
        Ntmp[l1] += ((l1 == l2) - Gamma[l1 * L + l2] * dt2) * N[l2];
      }
    }

    // compute E*8 at point i
    double E8[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    for (idot = 0; idot < 3 && cdot[idot] != Dielectric; ++idot) {
      realnum *w = W[cdot[idot]][0], *wp = W_prev[cdot[idot]][0];
      E8[idot][0] = w[i] + w[i + o1[idot]] + w[i + o2[idot]] + w[i + o1[idot] + o2[idot]] + wp[i] +
                    wp[i + o1[idot]] + wp[i + o2[idot]] + wp[i + o1[idot] + o2[idot]];
      if (W[cdot[idot]][1]) {
        w = W[cdot[idot]][1];
        wp = W_prev[cdot[idot]][1];
        E8[idot][1] = w[i] + w[i + o1[idot]] + w[i + o2[idot]] + w[i + o1[idot] + o2[idot]] +
                      wp[i] + wp[i + o1[idot]] + wp[i + o2[idot]] + wp[i + o1[idot] + o2[idot]];
      }
      else
        E8[idot][1] = 0;
    }

    // Ntmp = Ntmp + alpha * E * dP
    for (int t = 0; t < T; ++t) {
      // compute 32 * E * dP and 64 * E * P at point i
      double EdP32 = 0;
      double EPave64 = 0;
      double gperpdt = gamma[t] * pi * dt;
      for (idot = 0; idot < 3 && cdot[idot] != Dielectric; ++idot) {
        realnum *p = d->P[cdot[idot]][0][t], *pp = d->P_prev[cdot[idot]][0][t];
        realnum dP = p[i] + p[i + o1[idot]] + p[i + o2[idot]] + p[i + o1[idot] + o2[idot]] -
                     (pp[i] + pp[i + o1[idot]] + pp[i + o2[idot]] + pp[i + o1[idot] + o2[idot]]);
        realnum Pave2 = p[i] + p[i + o1[idot]] + p[i + o2[idot]] + p[i + o1[idot] + o2[idot]] +
                        (pp[i] + pp[i + o1[idot]] + pp[i + o2[idot]] + pp[i + o1[idot] + o2[idot]]);
        EdP32 += dP * E8[idot][0];
        EPave64 += Pave2 * E8[idot][0];
        if (d->P[cdot[idot]][1]) {
          p = d->P[cdot[idot]][1][t];
          pp = d->P_prev[cdot[idot]][1][t];
          dP = p[i] + p[i + o1[idot]] + p[i + o2[idot]] + p[i + o1[idot] + o2[idot]] -
               (pp[i] + pp[i + o1[idot]] + pp[i + o2[idot]] + pp[i + o1[idot] + o2[idot]]);
          Pave2 = p[i] + p[i + o1[idot]] + p[i + o2[idot]] + p[i + o1[idot] + o2[idot]] +
                  (pp[i] + pp[i + o1[idot]] + pp[i + o2[idot]] + pp[i + o1[idot] + o2[idot]]);
          EdP32 += dP * E8[idot][1];
          EPave64 += Pave2 * E8[idot][1];
        }
      }
      EdP32 *= 0.03125;    /* divide by 32 */
      EPave64 *= 0.015625; /* divide by 64 (extra factor of 1/2 is from P_current + P_previous) */
      for (int l = 0; l < L; ++l)
        Ntmp[l] += alpha[l * T + t] * EdP32 + alpha[l * T + t] * gperpdt * EPave64;
    }

    // N = GammaInv * Ntmp
    for (int l1 = 0; l1 < L; ++l1) {
      N[l1] = 0;
      for (int l2 = 0; l2 < L; ++l2)
        N[l1] += GammaInv[l1 * L + l2] * Ntmp[l2];
    }
  }

  // each P is updated as a damped harmonic oscillator
  for (int t = 0; t < T; ++t) {
    const double omega2pi = 2 * pi * omega[t], g2pi = gamma[t] * 2 * pi, gperp = gamma[t] * pi;
    const double omega0dtsqrCorrected = omega2pi * omega2pi * dt * dt + gperp * gperp * dt * dt;
    const double gamma1inv = 1 / (1 + g2pi * dt2), gamma1 = (1 - g2pi * dt2);
    const double dtsqr = dt * dt;
    // note that gamma[t]*2*pi = 2*gamma_perp as one would usually write it in SALT. -- AWC

    // figure out which levels this transition couples
    int lp = -1, lm = -1;
    for (int l = 0; l < L; ++l) {
      if (alpha[l * T + t] > 0) lp = l;
      if (alpha[l * T + t] < 0) lm = l;
    }
    if (lp < 0 || lm < 0) abort("invalid alpha array for transition %d", t);

    FOR_COMPONENTS(c) DOCMP2 {
      if (d->P[c][cmp]) {
        const realnum *w = W[c][cmp], *s = sigma[c][component_direction(c)];
        const double st = sigmat[5 * t + component_direction(c)];
        if (w && s) {
          realnum *p = d->P[c][cmp][t], *pp = d->P_prev[c][cmp][t];

          ptrdiff_t o1, o2;
          gv.cent2yee_offsets(c, o1, o2);
          o1 *= L;
          o2 *= L;
          const realnum *N = d->N;

          // directions/strides for offdiagonal terms, similar to update_eh
          const direction d = component_direction(c);
          direction d1 = cycle_direction(gv.dim, d, 1);
          component c1 = direction_component(c, d1);
          const realnum *w1 = W[c1][cmp];
          const realnum *s1 = w1 ? sigma[c][d1] : NULL;
          direction d2 = cycle_direction(gv.dim, d, 2);
          component c2 = direction_component(c, d2);
          const realnum *w2 = W[c2][cmp];
          const realnum *s2 = w2 ? sigma[c][d2] : NULL;

          if (s1 || s2) { abort("nondiagonal saturable gain is not yet supported"); }
          else { // isotropic
            LOOP_OVER_VOL_OWNED(gv, c, i) {
              realnum pcur = p[i];
              const realnum *Ni = N + i * L;
              // dNi is population inversion for this transition
              double dNi = 0.25 * (Ni[lp] + Ni[lp + o1] + Ni[lp + o2] + Ni[lp + o1 + o2] - Ni[lm] -
                                   Ni[lm + o1] - Ni[lm + o2] - Ni[lm + o1 + o2]);
              p[i] = gamma1inv * (pcur * (2 - omega0dtsqrCorrected) - gamma1 * pp[i] -
                                  dtsqr * (st * s[i] * w[i]) * dNi);
              pp[i] = pcur;
            }
          }
        }
      }
    }
  }
}

void multilevel_susceptibility::subtract_P(field_type ft,
                                           realnum *f_minus_p[NUM_FIELD_COMPONENTS][2],
                                           void *P_internal_data) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  field_type ft2 = ft == E_stuff ? D_stuff : B_stuff; // for sources etc.
  size_t ntot = d->ntot;
  for (int t = 0; t < T; ++t) {
    FOR_FT_COMPONENTS(ft, ec) DOCMP2 {
      if (d->P[ec][cmp]) {
        component dc = field_type_component(ft2, ec);
        if (f_minus_p[dc][cmp]) {
          realnum *p = d->P[ec][cmp][t];
          realnum *fmp = f_minus_p[dc][cmp];
          for (size_t i = 0; i < ntot; ++i)
            fmp[i] -= p[i];
        }
      }
    }
  }
}

typedef struct {
  size_t sz_data;
  size_t ntot;
  realnum *GammaInv; // inv(1 + Gamma * dt / 2)

  realnumP *P[NUM_FIELD_COMPONENTS][2]; // P[c][cmp][transition][i]
  realnumP *P_prev[NUM_FIELD_COMPONENTS][2];

  realnumP *V[2]; // V[cmp][crossection][i]
  realnumP *V_prev[2];

  realnum *N;    // ntot x L array of centered grid populations N[i*L + level]
  realnum *Ntmp; // temporary length L array of levels, used in updating
  realnum data[1];
} multilevel_extended_data;

void *multilevel_nonlinear_susceptibility::new_internal_data(realnum *W[NUM_FIELD_COMPONENTS][2],
                                                             const grid_volume &gv) const {
  size_t ntot = gv.ntot();
  size_t npol = 0; // number of polarized grid components
  FOR_COMPONENTS(c) DOCMP2 {
    if (needs_P(c, cmp, W)) npol += ntot;
  }

  size_t P_size =
      2 * npol * T; // number of radiative vector elements (2 because stores current and previous)
  size_t V_size =
      2 * ntot *
      C; // number of nonradiative scalar elements (2 because stores current and previous)
  size_t N_size =
      (ntot + 1) *
      L; // number of population elements ( + 1*L because stores Ntmp for further calculations)
  size_t G_size = L * L; // memory for GammaInv

  size_t sz = sizeof(multilevel_data) + sizeof(realnum) * (G_size + N_size + P_size + V_size - 1);
  multilevel_extended_data *d = (multilevel_extended_data *)malloc(sz);
  memset(d, 0, sz);
  d->sz_data = sz;
  return (void *)d;
}

void multilevel_nonlinear_susceptibility::init_internal_data(realnum *W[NUM_FIELD_COMPONENTS][2],
                                                             double dt, const grid_volume &gv,
                                                             void *data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)data;
  size_t sz_data = d->sz_data;
  memset(d, 0, sz_data);
  d->sz_data = sz_data;
  size_t ntot = d->ntot = gv.ntot();

  /* d->data points to a big block of data that holds GammaInv, P,
     P_prev, Ntmp, and N.  We also initialize a bunch of convenience
     pointer in d to point to the corresponding data in d->data, so
     that we don't have to remember in other functions how d->data is
     laid out. */

  // First L*L data block
  d->GammaInv = d->data;
  for (int i = 0; i < L; ++i)
    for (int j = 0; j < L; ++j)
      d->GammaInv[i * L + j] = (i == j) + Gamma[i * L + j] * dt / 2;
  if (!invert(d->GammaInv, L)) abort("multilevel_susceptibility: I + Gamma*dt/2 matrix singular");
  size_t G_size = L * L;

  // Second data block
  realnum *P = d->data + G_size;
  realnum *P_prev = P + ntot;
  size_t P_size = 0;
  FOR_COMPONENTS(c) DOCMP2 {
    if (needs_P(c, cmp, W)) {
      d->P[c][cmp] = new realnumP[T];
      d->P_prev[c][cmp] = new realnumP[T];
      for (int t = 0; t < T; ++t) {
        d->P[c][cmp][t] = P;
        d->P_prev[c][cmp][t] = P_prev;
        P += 2 * ntot;
        P_prev += 2 * ntot;
      }
      P_size += 2 * ntot * T;
    }
  }

  // Third data block
  d->Ntmp = d->data + G_size + P_size;
  d->N = d->Ntmp + L;
  // initial populations
  for (size_t i = 0; i < ntot; ++i)
    for (int l = 0; l < L; ++l)
      d->N[i * L + l] = N0[l];
  size_t N_size = L * (ntot + 1);

  // Last data block
  realnum *V = d->data + G_size + P_size + N_size;
  realnum *V_prev = V + ntot;
  DOCMP2 {
    d->V[cmp] = new realnumP[C];
    d->V_prev[cmp] = new realnumP[C];
    for (int cr = 0; cr < C; ++cr) {
      d->V[cmp][cr] = V;
      d->V_prev[cmp][cr] = V_prev;
      V += 2 * ntot;
      V_prev += 2 * ntot;
    }
  }
}

void *multilevel_nonlinear_susceptibility::copy_internal_data(void *data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)data;
  if (!d) return 0;
  multilevel_extended_data *dnew = (multilevel_extended_data *)malloc(d->sz_data);
  memcpy(dnew, d, d->sz_data);

  // reassign internal pointers to actual structure
  size_t ntot = d->ntot;

  dnew->GammaInv = dnew->data;
  size_t G_size = L * L;

  realnum *P = dnew->data + G_size;
  realnum *P_prev = P + ntot;
  size_t P_size = 0;
  FOR_COMPONENTS(c) DOCMP2 {
    if (d->P[c][cmp]) {
      dnew->P[c][cmp] = new realnumP[T];
      dnew->P_prev[c][cmp] = new realnumP[T];
      for (int t = 0; t < T; ++t) {
        dnew->P[c][cmp][t] = P;
        dnew->P_prev[c][cmp][t] = P_prev;
        P += 2 * ntot;
        P_prev += 2 * ntot;
      }
      P_size += 2 * ntot * T;
    }
  }

  dnew->Ntmp = dnew->data + G_size + P_size;
  dnew->N = dnew->Ntmp + L;
  size_t N_size = L * (ntot + 1);

  realnum *V = dnew->data + G_size + P_size + N_size;
  realnum *V_prev = V + ntot;
  DOCMP2 {
    dnew->V[cmp] = new realnumP[C];
    dnew->V_prev[cmp] = new realnumP[C];
    for (int cr = 0; cr < C; ++cr) {
      dnew->V[cmp][cr] = V;
      dnew->V_prev[cmp][cr] = V_prev;
      V += 2 * ntot;
      V_prev += 2 * ntot;
    }
  }

  return (void *)dnew;
}

void multilevel_nonlinear_susceptibility::delete_internal_data(void *data) const {
  if (data) {
    multilevel_extended_data *d = (multilevel_extended_data *)data;
    FOR_COMPONENTS(c) DOCMP2 {
      delete[] d->P[c][cmp];
      delete[] d->P_prev[c][cmp];
    }
    DOCMP2 {
      delete[] d->V[cmp];
      delete[] d->V_prev[cmp];
    }
    free(data);
  }
}

int multilevel_nonlinear_susceptibility::num_cinternal_notowned_needed(
    component c, void *P_internal_data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)P_internal_data;
  return d->P[c][0] ? T : 0;
}

realnum *multilevel_nonlinear_susceptibility::cinternal_notowned_ptr(int inotowned, component c,
                                                                     int cmp, int n,
                                                                     void *P_internal_data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)P_internal_data;
  if (!d || !d->P[c][cmp] || inotowned < 0 || inotowned >= T) // never true
    return NULL;
  return d->P[c][cmp][inotowned] + n;
}

void multilevel_nonlinear_susceptibility::update_P(realnum *W[NUM_FIELD_COMPONENTS][2],
                                                   realnum *W_prev[NUM_FIELD_COMPONENTS][2],
                                                   double dt, const grid_volume &gv,
                                                   void *P_internal_data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)P_internal_data;
  double dt2 = 0.5 * dt;

  // field directions and offsets for E * dP dot product.
  directions dirs = pick_field_directions(d, gv);
  offsets offs = pick_field_offsets(d, gv);

  // update N from W and P
  realnum *GammaInv = d->GammaInv;
  realnum *Ntmp = d->Ntmp;
  LOOP_OVER_VOL_OWNED(gv, Centered, i) {
    realnum *N = d->N + i * L; // N at current point, to update

    // Ntmp = (I - Gamma * dt/2) * N
    for (int l1 = 0; l1 < L; ++l1) {
      Ntmp[l1] = 0;
      for (int l2 = 0; l2 < L; ++l2) {
        Ntmp[l1] += ((l1 == l2) - Gamma[l1 * L + l2] * dt2) * N[l2];
      }
    }

    // compute E*8 at point i
    double E8[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}};
    component *cdot = dirs.cdot;
    for (int idot = 0; idot < 3 && cdot[idot] != Dielectric; ++idot) {
      realnum *w = W[cdot[idot]][0];
      realnum *wp = W_prev[cdot[idot]][0];
      E8[idot][0] = sum(i, idot, w, wp, offs);

      bool is_complex = W[cdot[idot]][1] != 0;
      if (is_complex) {
        w = W[cdot[idot]][1];
        wp = W_prev[cdot[idot]][1];
        E8[idot][1] = sum(i, idot, w, wp, offs);
      }
    }

    // Ntmp = Ntmp + alpha * E * dP
    for (int t = 0; t < T; ++t) {
      // compute 32 * E * dP and 64 * E * P at point i
      double EdP32 = 0;
      double EPave64 = 0;
      double gperpdt = gamma[t] * pi * dt;
      for (int idot = 0; idot < 3 && cdot[idot] != Dielectric; ++idot) {
        realnum *p = d->P[cdot[idot]][0][t];
        realnum *pp = d->P_prev[cdot[idot]][0][t];
        realnum dP = dif(i, idot, p, pp, offs);
        realnum Pave2 = sum(i, idot, p, pp, offs);

        EdP32 += dP * E8[idot][0];
        EPave64 += Pave2 * E8[idot][0];

        bool is_complex = d->P[cdot[idot]][1] != 0;
        if (is_complex) {
          p = d->P[cdot[idot]][1][t];
          pp = d->P_prev[cdot[idot]][1][t];
          dP = dif(i, idot, p, pp, offs);
          Pave2 = sum(i, idot, p, pp, offs);
          EdP32 += dP * E8[idot][1];
          EPave64 += Pave2 * E8[idot][1];
        }
      }
      EdP32 *= 0.03125;    /* divide by 32 */
      EPave64 *= 0.015625; /* divide by 64 (extra factor of 1/2 is from P_current + P_previous) */
      for (int l = 0; l < L; ++l)
        Ntmp[l] += alpha[l * T + t] * EdP32 + alpha[l * T + t] * gperpdt * EPave64;
    }

    // N = GammaInv * Ntmp
    for (int l1 = 0; l1 < L; ++l1) {
      N[l1] = 0;
      for (int l2 = 0; l2 < L; ++l2)
        N[l1] += GammaInv[l1 * L + l2] * Ntmp[l2];
    }
  }

  // each V is updated through Lioville equations
  for (int crossection = 0; crossection < C; ++crossection) {
    // figure out which levels this nonradiative transition couples
    int lp = -1, lm = -1;
    for (int l = 0; l < L; ++l) {
      if (beta[l * C + crossection] > 0) lp = 1;
      if (beta[l * C + crossection] < 0) lm = 1;
    }
    if (lp < 0 || lm < 0) abort("invalid beta array for nonradiative transition %d", crossection);

    LOOP_OVER_VOL_OWNED(gv, Centered, i) {
      realnum drho[2] = {0.0, 0.0};

      FOR_COMPONENTS(c) DOCMP2 {
        const realnum *w = W[c][cmp];
        const realnum *s = sigma[c][component_direction(c)];

        // autoevolution
        const realnum *v = d->V[cmp][crossection];
        drho[cmp] = - sign(cmp, 0) * gamma_decoherence[crossection] * v[cmp][i];
        drho[conjugate(cmp)] = - sign(cmp, 1) * omega_nonradiative[crossection] * v[cmp][i];

        for (int lk = 0; lk < L; ++lk) {
          // determine dipole transitions corresponding to pk and km
          for (int transition = 0; transition < T; ++transition) {
            const bool is_coupled_p = alpha[lp * T + transition] != 0;
            const bool is_coupled_k = alpha[lk * T + transition] != 0;
            const bool is_coupled_m = alpha[lm * T + transition] != 0;

            //former term in a commutator
            if (is_coupled_p && is_coupled_k) { 
              const realnum st = sigmat[5 * transition + component_direction(c)];
              const realnum interaction_factor = st * sigma[i] * w[i];
              bool is_index_found = false;

              const int nonradiative = correspond_nonradiative_transition(lk, lm, is_index_found);
              if (is_index_found) {
                const realnum *v_current = d->V[cmp][nonradiative];
                const realnum *v_previous = d->V_prev[cmp][nonradiative];
                drho[conjugate(cmp)] += sign(cmp, 1) * interaction_factor * (v_current[i] + v_previous[i]) * dt2;
              }
              else {
                const int radiative = correspond_radiative_transition(lk, lm, is_index_found);
                if (is_index_found) {
                  const realnum *u_current = d->P[c][cmp][radiative];
                  const realnum *u_previous = d->P_prev[c][cmp][radiative];
                  drho[conjugate(cmp)] += sign(cmp, 1) * interaction_factor * (u_current[i] + u_previous[i]) * dt2;
                }
                else 
                  abort("failed to correspond transition index to level indexes");
              }
            }

            //latter term in a commutator
            if (is_coupled_k && is_coupled_m) {
              const realnum st = sigmat[5 * transition + component_direction(c)];
              const realnum interaction_factor = st * sigma[i] * w[i];
              bool is_index_found = false;

              const int nonradiative = correspond_nonradiative_transition(lp, lk, is_index_found);
              if (is_index_found) {
                const realnum *v_current = d->V[cmp][nonradiative];
                const realnum *v_previous = d->V_prev[cmp][nonradiative];
                drho[conjugate(cmp)] -= sign(cmp, 1) * interaction_factor * (v_current[i] + v_previous[i]) * dt2;
              }
              else {
                const int radiative = correspond_radiative_transition(lp, lk, is_index_found);
                if (is_index_found) {
                  const realnum *u_current = d->P[c][cmp][radiative];
                  const realnum *u_previous = d->P_prev[c][cmp][radiative];
                  drho[conjugate(cmp)] -= sign(cmp, 1) * interaction_factor * (u_current[i] + u_previous[i]) * dt2;
                }
                else 
                  abort("failed to correspond transition index to level indexes");
              }
            }
          }
        }
      }

      d->V_prev[0][crossection][i] = d->V[0][crossection][i];
      d->V_prev[1][crossection][i] = d->V[1][crossection][i];

      d->V[0][crossection][i] += drho[0];
      d->V[1][crossection][i] += drho[1];
    }
  }

  // each P is updated through Liouville equations
  for (int t = 0; t < T; ++t) {
    const double omega2pi = 2 * pi * omega[t];
    const double g2pi = gamma[t] * 2 * pi;
    const double gperp = gamma[t] * pi;
    const double omega0dtsqrCorrected = omega2pi * omega2pi * dt * dt + gperp * gperp * dt * dt;
    const double gamma1inv = 1 / (1 + g2pi * dt2);
    const double gamma1 = (1 - g2pi * dt2);
    const double dtsqr = dt * dt;
    // note that gamma[t]*2*pi = 2*gamma_perp as one would usually write it in SALT. -- AWC

    // figure out which levels this transition couples
    int lp = -1, lm = -1;
    for (int l = 0; l < L; ++l) {
      if (alpha[l * T + t] > 0) lp = l;
      if (alpha[l * T + t] < 0) lm = l;
    }
    if (lp < 0 || lm < 0) abort("invalid alpha array for transition %d", t);

    FOR_COMPONENTS(c) DOCMP2 {
      if (d->P[c][cmp]) {
        const realnum *w = W[c][cmp];
        const realnum *s = sigma[c][component_direction(c)];
        const double st = sigmat[5 * t + component_direction(c)];
        if (w && s) {
          realnum *p = d->P[c][cmp][t];
          realnum *pp = d->P_prev[c][cmp][t];
          ptrdiff_t o1, o2;
          gv.cent2yee_offsets(c, o1, o2);
          o1 *= L;
          o2 *= L;
          const realnum *N = d->N;

          // directions/strides for offdiagonal terms, similar to update_eh
          const direction d = component_direction(c);
          direction d1 = cycle_direction(gv.dim, d, 1);
          component c1 = direction_component(c, d1);
          const realnum *w1 = W[c1][cmp];
          const realnum *s1 = w1 ? sigma[c][d1] : NULL;
          direction d2 = cycle_direction(gv.dim, d, 2);
          component c2 = direction_component(c, d2);
          const realnum *w2 = W[c2][cmp];
          const realnum *s2 = w2 ? sigma[c][d2] : NULL;

          if (s1 || s2) { abort("nondiagonal saturable gain is not yet supported"); }
          else { // isotropic
            LOOP_OVER_VOL_OWNED(gv, c, i) {
              realnum pcur = p[i];
              const realnum *Ni = N + i * L;
              // dNi is population inversion for this transition
              double dNi = 0.25 * (Ni[lp] + Ni[lp + o1] + Ni[lp + o2] + Ni[lp + o1 + o2] - Ni[lm] -
                                   Ni[lm + o1] - Ni[lm + o2] - Ni[lm + o1 + o2]);
              p[i] = gamma1inv * (pcur * (2 - omega0dtsqrCorrected) - gamma1 * pp[i] -
                                  dtsqr * (st * s[i] * w[i]) * dNi);
              pp[i] = pcur;
            }
          }
        }
      }
    }
  }
}

void multilevel_nonlinear_susceptibility::subtract_P(field_type ft,
                                           realnum *f_minus_p[NUM_FIELD_COMPONENTS][2],
                                           void *P_internal_data) const {
  multilevel_extended_data *d = (multilevel_extended_data *)P_internal_data;
  field_type ft2 = ft == E_stuff ? D_stuff : B_stuff; // for sources etc.
  size_t ntot = d->ntot;
  for (int t = 0; t < T; ++t) {
    FOR_FT_COMPONENTS(ft, ec) DOCMP2 {
      if (d->P[ec][cmp]) {
        component dc = field_type_component(ft2, ec);
        if (f_minus_p[dc][cmp]) {
          realnum *p = d->P[ec][cmp][t];
          realnum *fmp = f_minus_p[dc][cmp];
          for (size_t i = 0; i < ntot; ++i)
            fmp[i] -= p[i];
        }
      }
    }
  }
}

multilevel_nonlinear_susceptibility::directions
multilevel_nonlinear_susceptibility::pick_field_directions(const void *P_internal_data,
                                                           const grid_volume &gv) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  directions dirs;
  int idot = 0;
  FOR_COMPONENTS(c) {
    if (d->P[c][0]) {
      if (idot == 3) abort("bug in meep: too many polarization components");
      dirs.cdot[idot++] = c;
    }
  }
  return dirs;
}

multilevel_nonlinear_susceptibility::offsets
multilevel_nonlinear_susceptibility::pick_field_offsets(const void *P_internal_data,
                                                        const grid_volume &gv) const {
  multilevel_data *d = (multilevel_data *)P_internal_data;
  offsets o;
  int idot = 0;
  FOR_COMPONENTS(c) {
    if (d->P[c][0]) {
      if (idot == 3) abort("bug in meep: too many polarization components");
      gv.yee2cent_offsets(c, o.o1[idot], o.o2[idot]);
    }
  }
  return o;
}

double multilevel_nonlinear_susceptibility::sum(int i, int idot, realnum *curr, realnum *prev,
                                                offsets offs) const {
  double sumval = sum(i, idot, curr, offs) + sum(i, idot, prev, offs);
  return sumval;
}

double multilevel_nonlinear_susceptibility::dif(int i, int idot, realnum *curr, realnum *prev,
                                                offsets offs) const {
  double difval = sum(i, idot, curr, offs) - sum(i, idot, prev, offs);
  return difval;
}

double multilevel_nonlinear_susceptibility::sum(int i, int idot, realnum *vals,
                                                offsets offs) const {
  ptrdiff_t *o1 = offs.o1;
  ptrdiff_t *o2 = offs.o2;
  double sumval = vals[i] + vals[i + o1[idot]] + vals[i + o2[idot]] + vals[i + o1[idot] + o2[idot]];
  return sumval;
}

int multilevel_nonlinear_susceptibility::correspond_radiative_transition(int l1, int l2, bool &is_successfull) const {
  for (int radiative = 0; radiative < T; ++radiative) { //evolution due to radiative oscillations
    const bool is_corresponded = (alpha[l1 * T + radiative] != 0 && alpha[l2 * T + radiative] != 0);
    if (is_corresponded) {
      is_successfull = true;
      return radiative;
    }
  }
  return -1;
}

int multilevel_nonlinear_susceptibility::correspond_nonradiative_transition(int l1, int l2, bool &is_successfull) const {
  for (int nonradiative = 0; nonradiative < C; ++nonradiative) { // evolution due to nonradiative oscillations
    const bool is_corresponded = (beta[l1 * C + nonradiative] != 0 && beta[l2 * C + nonradiative] != 0);
    if (is_corresponded) {
      is_successfull = true;
      return nonradiative;
    }
  }
  return -1;
}

int multilevel_nonlinear_susceptibility::conjugate(int cmp) const {
  switch (cmp)
  {
  case 0:
    return 1;
  case 1:
    return 0;
  default:
    abort("Invalid index for a real/imaginary part of a complex number")
    break;
  }
}

} // namespace meep
