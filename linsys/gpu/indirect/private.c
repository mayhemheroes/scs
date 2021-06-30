#include "private.h"

#define CG_BEST_TOL 1e-9
#define CG_MIN_TOL 1e-1

/* do not use within pcg, reuses memory */
void SCS(accum_by_atrans)(const ScsMatrix *A, ScsLinSysWork *p,
                          const scs_float *x, scs_float *y) {
  scs_float *v_m = p->tmp_m;
  scs_float *v_n = p->r;
  cudaMemcpy(v_m, x, A->m * sizeof(scs_float), cudaMemcpyHostToDevice);
  cudaMemcpy(v_n, y, A->n * sizeof(scs_float), cudaMemcpyHostToDevice);

  cusparseDnVecSetValues(p->dn_vec_m, (void *) v_m);
  cusparseDnVecSetValues(p->dn_vec_n, (void *) v_n);
  SCS(_accum_by_atrans_gpu)(
    p->Ag, p->dn_vec_m, p->dn_vec_n, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );

  cudaMemcpy(y, v_n, A->n * sizeof(scs_float), cudaMemcpyDeviceToHost);
}

/* do not use within pcg, reuses memory */
void SCS(accum_by_a)(const ScsMatrix *A, ScsLinSysWork *p, const scs_float *x,
                     scs_float *y) {
  scs_float *v_m = p->tmp_m;
  scs_float *v_n = p->r;
  cudaMemcpy(v_n, x, A->n * sizeof(scs_float), cudaMemcpyHostToDevice);
  cudaMemcpy(v_m, y, A->m * sizeof(scs_float), cudaMemcpyHostToDevice);

  cusparseDnVecSetValues(p->dn_vec_m, (void *) v_m);
  cusparseDnVecSetValues(p->dn_vec_n, (void *) v_n);
#if GPU_TRANSPOSE_MAT > 0
  SCS(_accum_by_atrans_gpu)(
    p->Agt, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#else
  SCS(_accum_by_a_gpu)(
    p->Ag, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#endif

  cudaMemcpy(y, v_m, A->m * sizeof(scs_float), cudaMemcpyDeviceToHost);
}

char *SCS(get_lin_sys_method)(const ScsMatrix *A, const ScsSettings *stgs) {
  char *str = (char *)scs_malloc(sizeof(char) * 128);
  sprintf(str, "sparse-indirect GPU, nnz in A = %li, CG tol ~ 1/iter^(%2.2f)",
          (long)A->p[A->n], stgs->cg_rate);
  return str;
}

char *SCS(get_lin_sys_summary)(ScsLinSysWork *p, const ScsInfo *info) {
  char *str = (char *)scs_malloc(sizeof(char) * 128);
  sprintf(str,
          "\tLin-sys: avg # CG iterations: %2.2f, avg solve time: %1.2es\n",
          (scs_float)p->tot_cg_its / (info->iter + 1),
          p->total_solve_time / (info->iter + 1) / 1e3);
  p->tot_cg_its = 0;
  p->total_solve_time = 0;
  return str;
}

void SCS(free_lin_sys_work)(ScsLinSysWork *p) {
  if (p) {
    cudaFree(p->p);
    cudaFree(p->r);
    cudaFree(p->Gp);
    cudaFree(p->bg);
    cudaFree(p->tmp_m);
    cudaFree(p->z);
    cudaFree(p->M);
    if (p->Ag) {
      SCS(free_gpu_matrix)(p->Ag);
      scs_free(p->Ag);
    }
    if (p->Agt) {
      SCS(free_gpu_matrix)(p->Agt);
      scs_free(p->Agt);
    }
    if (p->buffer != SCS_NULL) {
      cudaFree(p->buffer);
    }
    cusparseDestroyDnVec(p->dn_vec_m);
    cusparseDestroyDnVec(p->dn_vec_n);
    cusparseDestroy(p->cusparse_handle);
    cublasDestroy(p->cublas_handle);
    /* Don't reset because it interferes with other GPU programs. */
    /* cudaDeviceReset(); */
    scs_free(p);
  }
}

static void scale_by_diag_r(scs_float *vec, scs_int m, ScsLinSysWork *p) {
  scs_float onef = 1.0;
  scs_float zerof = 0.0;
  CUBLAS(sbmv)
    (p->cublas_handle, CUBLAS_FILL_MODE_LOWER, m, 0, &onef, p->rho_y_vec_g, 1, vec, 1, &zerof, SCS_NULL, 0);
}

/* y = (rho_x * I + P + A' R A) x */
static void mat_vec(const ScsGpuMatrix *A, const ScsSettings *s,
                    ScsLinSysWork *p, const scs_float *x, scs_float *y) {
  /* x and y MUST already be loaded to GPU */
  scs_float *z= p->tmp_m; /* temp memory */
  cudaMemset(z, 0, A->m * sizeof(scs_float));
  cudaMemset(y, 0, A->n * sizeof(scs_float));
  if (P) {
    SCS(accum_by_p)(P, p, x, y); /* y = Px */
  }
  cusparseDnVecSetValues(p->dn_vec_m, (void *) z);
  cusparseDnVecSetValues(p->dn_vec_n, (void *) x);
  /* z = Ax */
#if GPU_TRANSPOSE_MAT > 0
  SCS(_accum_by_atrans_gpu)(
    p->Agt, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#else
  SCS(_accum_by_a_gpu)(
    A, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#endif

  /* z = R A x */
  scale_by_diag_r(z, A->m, p);

  cusparseDnVecSetValues(p->dn_vec_m, (void *) z);
  cusparseDnVecSetValues(p->dn_vec_n, (void *) y);
  /* y += A'z, y = Px + A' R Ax */
  SCS(_accum_by_atrans_gpu)(
    A, p->dn_vec_m, p->dn_vec_n, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
  /* y += rho_x * x = rho_x * x + Px + A' R A x */
  CUBLAS(axpy)(p->cublas_handle, A->n, &(s->rho_x), x, 1, y, 1);
}

/* M = inv ( diag ( RHO_X * I + A'A ) ) */
static void get_preconditioner(const ScsMatrix *A, const ScsSettings *stgs,
                               ScsLinSysWork *p) {
  scs_int i;
  scs_float *M = (scs_float *)scs_malloc(A->n * sizeof(scs_float));

#if VERBOSITY > 0
  scs_printf("getting pre-conditioner\n");
#endif

  for (i = 0; i < A->n; ++i) {
    M[i] = 1 / (stgs->rho_x +
                SCS(norm_sq)(&(A->x[A->p[i]]), A->p[i + 1] - A->p[i]));
    /* M[i] = 1; */
  }
  cudaMemcpy(p->M, M, A->n * sizeof(scs_float), cudaMemcpyHostToDevice);
  scs_free(M);

#if VERBOSITY > 0
  scs_printf("finished getting pre-conditioner\n");
#endif
}

/* P comes in upper triangular, expand to full */
static csc *fill_p_matrix(const csc *P) {
  Pnzmax = 2 * P->p[n]; /* upper bound */
  P_T = cs_spalloc(n, n, Pnzmax, 1, 1);
  kk = 0;
  for (j = 0; j < P->n; j++) { /* cols */
    for (k = P->p[j]; k < P->p[j + 1]; k++) {
      i = P->i[k]; /* row */
      if (i > j) { /* only upper triangular needed */
        break;
      }
      P_T->i[kk] = i;
      P_T->p[kk] = j;
      P_T->x[kk] = P->x[k];
      if (i != j) { /* not diagonal */
        P_T->i[kk+1] = j;
        P_T->p[kk+1] = i;
        P_T->x[kk+1] = P->x[k];
      }
      kk += 2;
    }
  }
  return cs_compress(P, SCS_NULL);
}


ScsLinSysWork *SCS(init_lin_sys_work)(const ScsMatrix *A, const ScsMatrix *P,
                                      const ScsSettings *stgs,
                                      scs_float *rho_y_vec) {
  cudaError_t err;
  ScsLinSysWork *p = (ScsLinSysWork *)scs_calloc(1, sizeof(ScsLinSysWork));
  ScsGpuMatrix *Ag = (ScsGpuMatrix *)scs_malloc(sizeof(ScsGpuMatrix));
  ScsGpuMatrix *Pg = (ScsGpuMatrix *)scs_malloc(sizeof(ScsGpuMatrix));

  /* Used for initializing dense vectors */
  scs_float *tmp_null_n = SCS_NULL;
  scs_float *tmp_null_m = SCS_NULL;

#if GPU_TRANSPOSE_MAT > 0
  size_t new_buffer_size = 0;
#endif

  P_full = fill_p_matrix();

  p->cublas_handle = 0;
  p->cusparse_handle = 0;

  p->total_solve_time = 0;
  p->tot_cg_its = 0;

  p->buffer_size = 0;
  p->buffer = SCS_NULL;

  /* Get handle to the CUBLAS context */
  cublasCreate(&p->cublas_handle);

  /* Get handle to the CUSPARSE context */
  cusparseCreate(&p->cusparse_handle);

  Ag->n = A->n;
  Ag->m = A->m;
  Ag->Annz = A->p[A->n];
  Ag->descr = 0;
  /* Matrix description */

  p->Ag = Ag;
  p->Agt = SCS_NULL;

  cudaMalloc((void **)&Ag->i, (A->p[A->n]) * sizeof(scs_int));
  cudaMalloc((void **)&Ag->p, (A->n + 1) * sizeof(scs_int));
  cudaMalloc((void **)&Ag->x, (A->p[A->n]) * sizeof(scs_float));

  cudaMalloc((void **)&p->p, A->n * sizeof(scs_float));
  cudaMalloc((void **)&p->r, A->n * sizeof(scs_float));
  cudaMalloc((void **)&p->Gp, A->n * sizeof(scs_float));
  cudaMalloc((void **)&p->bg, (A->n + A->m) * sizeof(scs_float));
  cudaMalloc((void **)&p->tmp_m,
             A->m * sizeof(scs_float)); /* intermediate result */
  cudaMalloc((void **)&p->z, A->n * sizeof(scs_float));
  cudaMalloc((void **)&p->M, A->n * sizeof(scs_float));

  cudaMemcpy(Ag->i, A->i, (A->p[A->n]) * sizeof(scs_int),
             cudaMemcpyHostToDevice);
  cudaMemcpy(Ag->p, A->p, (A->n + 1) * sizeof(scs_int), cudaMemcpyHostToDevice);
  cudaMemcpy(Ag->x, A->x, (A->p[A->n]) * sizeof(scs_float),
             cudaMemcpyHostToDevice);

  cusparseCreateCsr
  (&Ag->descr, Ag->n, Ag->m, Ag->Annz, Ag->p, Ag->i, Ag->x,
    SCS_CUSPARSE_INDEX, SCS_CUSPARSE_INDEX,
    CUSPARSE_INDEX_BASE_ZERO, SCS_CUDA_FLOAT);

  cudaMalloc((void **)&tmp_null_n, A->n * sizeof(scs_float));
  cudaMalloc((void **)&tmp_null_m, A->m * sizeof(scs_float));
  cusparseCreateDnVec(&p->dn_vec_n, Ag->n, tmp_null_n, SCS_CUDA_FLOAT);
  cusparseCreateDnVec(&p->dn_vec_m, Ag->m, tmp_null_m, SCS_CUDA_FLOAT);
  cudaFree(tmp_null_n);
  cudaFree(tmp_null_m);

  get_preconditioner(A, stgs, p);

#if GPU_TRANSPOSE_MAT > 0
  p->Agt = (ScsGpuMatrix *)scs_malloc(sizeof(ScsGpuMatrix));
  p->Agt->n = A->m;
  p->Agt->m = A->n;
  p->Agt->Annz = A->p[A->n];
  p->Agt->descr = 0;
  /* Matrix description */

  cudaMalloc((void **)&p->Agt->i, (A->p[A->n]) * sizeof(scs_int));
  cudaMalloc((void **)&p->Agt->p, (A->m + 1) * sizeof(scs_int));
  cudaMalloc((void **)&p->Agt->x, (A->p[A->n]) * sizeof(scs_float));
  /* transpose Ag into Agt for faster multiplies */
  /* TODO: memory intensive, could perform transpose in CPU and copy to GPU */
  cusparseCsr2cscEx2_bufferSize
  (p->cusparse_handle, A->n, A->m, A->p[A->n],
    Ag->x, Ag->p, Ag->i,
    p->Agt->x, p->Agt->p, p->Agt->i,
    SCS_CUDA_FLOAT, CUSPARSE_ACTION_NUMERIC,
    CUSPARSE_INDEX_BASE_ZERO, SCS_CSR2CSC_ALG,
    &new_buffer_size);

  if (new_buffer_size > p->buffer_size) {
    if (p->buffer != SCS_NULL) {
      cudaFree(p->buffer);
    }
    cudaMalloc(&p->buffer, new_buffer_size);
    p->buffer_size = new_buffer_size;
  }

  cusparseCsr2cscEx2
  (p->cusparse_handle, A->n, A->m, A->p[A->n],
    Ag->x, Ag->p, Ag->i,
    p->Agt->x, p->Agt->p, p->Agt->i,
    SCS_CUDA_FLOAT, CUSPARSE_ACTION_NUMERIC,
    CUSPARSE_INDEX_BASE_ZERO, SCS_CSR2CSC_ALG,
    p->buffer);

  cusparseCreateCsr
  (&p->Agt->descr, p->Agt->n, p->Agt->m, p->Agt->Annz,
    p->Agt->p, p->Agt->i, p->Agt->x,
    SCS_CUSPARSE_INDEX, SCS_CUSPARSE_INDEX,
    CUSPARSE_INDEX_BASE_ZERO, SCS_CUDA_FLOAT);
#endif

  err = cudaGetLastError();
  if (err != cudaSuccess) {
    printf("%s:%d:%s\nERROR_CUDA: %s\n", __FILE__, __LINE__, __func__,
           cudaGetErrorString(err));
    SCS(free_lin_sys_work)(p);
    return SCS_NULL;
  }
  return p;
}

static void apply_pre_conditioner(cublasHandle_t cublas_handle, scs_float *M,
                                  scs_float *z, scs_float *r, scs_int n) {
  cudaMemcpy(z, r, n * sizeof(scs_float), cudaMemcpyDeviceToDevice);
  CUBLAS(tbmv)
  (cublas_handle, CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_N, CUBLAS_DIAG_NON_UNIT, n,
   0, M, 1, z, 1);
}

/* solves (I+A'A)x = b, s warm start, solution stored in bg (on GPU) */
static scs_int pcg(const ScsGpuMatrix *A, const ScsSettings *stgs,
                   ScsLinSysWork *pr, const scs_float *s, scs_float *bg,
                   scs_int max_its, scs_float tol) {
  scs_int i, n = A->n;
  scs_float alpha, nrm_r, p_gp, neg_alpha, beta, ipzr, ipzr_old;
  scs_float onef = 1.0, neg_onef = -1.0;
  scs_float *p = pr->p;   /* cg direction */
  scs_float *Gp = pr->Gp; /* updated CG direction */
  scs_float *r = pr->r;   /* cg residual */
  scs_float *z = pr->z;   /* preconditioned */
  scs_float *M = pr->M;   /* preconditioner */
  cublasHandle_t cublas_handle = pr->cublas_handle;

  if (s == SCS_NULL) {
    cudaMemcpy(r, bg, n * sizeof(scs_float), cudaMemcpyDeviceToDevice);
    cudaMemset(bg, 0, n * sizeof(scs_float));
  } else {
    /* p contains bg temporarily */
    cudaMemcpy(p, bg, n * sizeof(scs_float), cudaMemcpyDeviceToDevice);
    /* bg contains s */
    cudaMemcpy(bg, s, n * sizeof(scs_float), cudaMemcpyHostToDevice);
    mat_vec(A, stgs, pr, bg, r);
    CUBLAS(axpy)(cublas_handle, n, &neg_onef, p, 1, r, 1);
    CUBLAS(scal)(cublas_handle, n, &neg_onef, r, 1);
  }

  /* for some reason nrm2 is VERY slow */
  /* CUBLAS(nrm2)(cublas_handle, n, r, 1, &nrm_r); */
  CUBLAS(dot)(cublas_handle, n, r, 1, r, 1, &nrm_r);
  nrm_r = SQRTF(nrm_r);
  /* check to see if we need to run CG at all */
  if (nrm_r < MIN(tol, 1e-18)) {
    return 0;
  }

  apply_pre_conditioner(cublas_handle, M, z, r, n);
  CUBLAS(dot)(cublas_handle, n, r, 1, z, 1, &ipzr);
  /* put z in p, replacing temp mem */
  cudaMemcpy(p, z, n * sizeof(scs_float), cudaMemcpyDeviceToDevice);

  for (i = 0; i < max_its; ++i) {
    mat_vec(A, stgs, pr, p, Gp);

    CUBLAS(dot)(cublas_handle, n, p, 1, Gp, 1, &p_gp);

    alpha = ipzr / p_gp;
    neg_alpha = -alpha;

    CUBLAS(axpy)(cublas_handle, n, &alpha, p, 1, bg, 1);
    CUBLAS(axpy)(cublas_handle, n, &neg_alpha, Gp, 1, r, 1);

    /* for some reason nrm2 is VERY slow */
    /* CUBLAS(nrm2)(cublas_handle, n, r, 1, &nrm_r); */
    CUBLAS(dot)(cublas_handle, n, r, 1, r, 1, &nrm_r);
    nrm_r = SQRTF(nrm_r);
    if (nrm_r < tol) {
      i++;
      break;
    }
    ipzr_old = ipzr;
    apply_pre_conditioner(cublas_handle, M, z, r, n);
    CUBLAS(dot)(cublas_handle, n, r, 1, z, 1, &ipzr);

    beta = ipzr / ipzr_old;
    CUBLAS(scal)(cublas_handle, n, &beta, p, 1);
    CUBLAS(axpy)(cublas_handle, n, &onef, z, 1, p, 1);
  }
#if VERBOSITY > 0
  scs_printf("tol: %.4e, resid: %.4e, iters: %li\n", tol, nrm_r, (long)i + 1);
#endif
  return i;
}

/* solves Mx = b, for x but stores result in b */
/* s contains warm-start (if available) */
/*
 * [x] = [rho_x I + P     A' ]^{-1} [rx]
 * [y]   [     A        -R^-1]      [ry]
 *
 * R = diag(rho_y_vec)
 *
 * becomes:
 *
 * x = (rho_x I + P + A' R A)^{-1} (rx + A' R ry)
 * y = R (Ax - ry)
 *
 */
scs_int SCS(solve_lin_sys)(const ScsMatrix *A, const ScsMatrix *P,
                           const ScsSettings *stgs,i ScsLinSysWork *p,
                           scs_float *b, const scs_float *s, scs_int iter) {
  scs_int cg_its, max_iters = INT_MAX;
  scs_float cg_tol = CG_BEST_TOL;
  scs_float neg_onef = -1.0;
  /* these are on GPU */
  scs_float *bg = p->bg;
  scs_float *R = p->R;
  scs_float *tmp_m = p->tmp_m
  ScsGpuMatrix *Ag = p->Ag;
  ScsGpuMatrix *Pg = p->Pg;

  if (CG_NORM(b, A->n + A->m) <= 1e-18) {
    memset(b, 0, (A->n + A->m) * sizeof(scs_float));
    return 0;
  }

  /* bg = b = [rx; ry] */
  cudaMemcpy(bg, b, (Ag->n + Ag->m) * sizeof(scs_float), cudaMemcpyHostToDevice);
  /* tmp = ry */
  cudaMemcpy(tmp_m, &(b[Ag->n]), Ag/* tmp = R * ry */->m * sizeof(scs_float), cudaMemcpyDeviceToDevice);
  /* tmp = R * ry */
  scale_by_diag_r(tmp_m, A->m, p);

  cusparseDnVecSetValues(p->dn_vec_m, (void *) tmp_m); /* R * ry */
  cusparseDnVecSetValues(p->dn_vec_n, (void *) bg); /* rx */
  /* bg[:n] = rx + A' R ry */
  SCS(_accum_by_atrans_gpu)(
    Ag, p->dn_vec_m, p->dn_vec_n, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );

  if (iter >= 0) {
    cg_tol = MAX(CG_BEST_TOL, CG_NORM(b, Ag->n) * CG_BASE_TOL /
                 POWF((scs_float)iter + 1, stgs->cg_rate));
    /* set max_iters to 100 * n (though in theory n is enough for any tol) */
    max_iters = 100 * Ag->n;
  }

  /* solves (rho_x I + P + A' R A)x = bg, s warm start, solution stored in bg */
  cg_its = pcg(Ag, Pg, stgs, p, s, bg, max_iters, cg_tol); /* bg[:n] = x */

  /* bg[n:] = -ry */
  CUBLAS(scal)(p->cublas_handle, Ag->m, &neg_onef, &(bg[Ag->n]), 1);
  cusparseDnVecSetValues(p->dn_vec_m, (void *) &(bg[Ag->n])); /* -ry */
  cusparseDnVecSetValues(p->dn_vec_n, (void *) bg); /* x */

  /* b[n:] = Ax - ry */
#if GPU_TRANSPOSE_MAT > 0
  SCS(_accum_by_atrans_gpu)(
    p->Agt, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#else
  SCS(_accum_by_a_gpu)(
    Ag, p->dn_vec_n, p->dn_vec_m, p->cusparse_handle,
    &p->buffer_size, &p->buffer
  );
#endif

  /* bg[n:] = R (Ax - ry) = y */
  scale_by_diag_r(&(bg[A->n]), A->m, p);

  /* copy bg = [x; y] back to b */
  cudaMemcpy(b, bg, (Ag->n + Ag->m) * sizeof(scs_float), cudaMemcpyDeviceToHost);

  if (iter >= 0) {
    p->tot_cg_its += cg_its;
  }

  return 0;
}
