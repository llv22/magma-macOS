/*
    -- MAGMA (version 1.1) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2011

       @precisions normal z -> s d c

*/
#include "common_magma.h"

extern "C" magma_int_t
magma_zgeqrf(magma_int_t m, magma_int_t n, 
             cuDoubleComplex *A,    magma_int_t lda, cuDoubleComplex *tau, 
             cuDoubleComplex *work, magma_int_t lwork,
             magma_int_t *info )
{
/*  -- MAGMA (version 1.1) --
       Univ. of Tennessee, Knoxville
       Univ. of California, Berkeley
       Univ. of Colorado, Denver
       November 2011

    Purpose
    =======
    ZGEQRF computes a QR factorization of a COMPLEX_16 M-by-N matrix A:
    A = Q * R. This version does not require work space on the GPU
    passed as input. GPU memory is allocated in the routine.

    Arguments
    =========
    M       (input) INTEGER
            The number of rows of the matrix A.  M >= 0.

    N       (input) INTEGER
            The number of columns of the matrix A.  N >= 0.

    A       (input/output) COMPLEX_16 array, dimension (LDA,N)
            On entry, the M-by-N matrix A.
            On exit, the elements on and above the diagonal of the array
            contain the min(M,N)-by-N upper trapezoidal matrix R (R is
            upper triangular if m >= n); the elements below the diagonal,
            with the array TAU, represent the orthogonal matrix Q as a
            product of min(m,n) elementary reflectors (see Further
            Details).

            Higher performance is achieved if A is in pinned memory, e.g.
            allocated using magma_malloc_pinned.

    LDA     (input) INTEGER
            The leading dimension of the array A.  LDA >= max(1,M).

    TAU     (output) COMPLEX_16 array, dimension (min(M,N))
            The scalar factors of the elementary reflectors (see Further
            Details).

    WORK    (workspace/output) COMPLEX_16 array, dimension (MAX(1,LWORK))
            On exit, if INFO = 0, WORK(1) returns the optimal LWORK.

            Higher performance is achieved if WORK is in pinned memory, e.g.
            allocated using magma_malloc_pinned.

    LWORK   (input) INTEGER
            The dimension of the array WORK.  LWORK >= max( N*NB, 2*NB*NB ),
            where NB can be obtained through magma_get_zgeqrf_nb(M).

            If LWORK = -1, then a workspace query is assumed; the routine
            only calculates the optimal size of the WORK array, returns
            this value as the first entry of the WORK array, and no error
            message related to LWORK is issued.

    INFO    (output) INTEGER
            = 0:  successful exit
            < 0:  if INFO = -i, the i-th argument had an illegal value
                  or another error occured, such as memory allocation failed.

    Further Details
    ===============
    The matrix Q is represented as a product of elementary reflectors

       Q = H(1) H(2) . . . H(k), where k = min(m,n).

    Each H(i) has the form

       H(i) = I - tau * v * v'

    where tau is a complex scalar, and v is a complex vector with
    v(1:i-1) = 0 and v(i) = 1; v(i+1:m) is stored on exit in A(i+1:m,i),
    and tau in TAU(i).
    =====================================================================    */

    #define  A(i,j) ( A + (i) + (j)*lda )
    #define dA(i,j) (dA + (i) + (j)*ldda)

    cuDoubleComplex *dA, *dwork, *dT;
    cuDoubleComplex c_one = MAGMA_Z_ONE;

    magma_int_t i, k, lddwork, old_i, old_ib;
    magma_int_t ib, ldda;

    /* Function Body */
    *info = 0;
    magma_int_t nb = magma_get_zgeqrf_nb(min(m, n));

    // need 2*nb*nb to store T and upper triangle of V simultaneously
    magma_int_t lwkopt = max(n*nb, 2*nb*nb);
    work[0] = MAGMA_Z_MAKE( (double)lwkopt, 0 );
    int lquery = (lwork == -1);
    if (m < 0) {
        *info = -1;
    } else if (n < 0) {
        *info = -2;
    } else if (lda < max(1,m)) {
        *info = -4;
    } else if (lwork < max(1, lwkopt) && ! lquery) {
        *info = -7;
    }
    if (*info != 0) {
        magma_xerbla( __func__, -(*info) );
        return *info;
    }
    else if (lquery)
        return *info;

    k = min(m,n);
    if (k == 0) {
        work[0] = c_one;
        return *info;
    }

    // largest N for larfb is n-nb (trailing matrix lacks 1st panel)
    lddwork = ((n+31)/32)*32 - nb;
    ldda    = ((m+31)/32)*32;

    magma_int_t num_gpus = magma_num_gpus();
    if( num_gpus > 1 ) {
        /* call multiple-GPU interface  */
        return magma_zgeqrf4(num_gpus, m, n, A, lda, tau, work, lwork, info);
    }

    // allocate space for dA, dwork, and dT
    if (MAGMA_SUCCESS != magma_zmalloc( &dA, n*ldda + nb*lddwork + nb*nb )) {
        /* Switch to the "out-of-core" (out of GPU-memory) version */
        return magma_zgeqrf_ooc(m, n, A, lda, tau, work, lwork, info);
    }

    cudaStream_t stream[2];
    magma_queue_create( &stream[0] );
    magma_queue_create( &stream[1] );

    dwork = dA + n*ldda;
    dT    = dA + n*ldda + nb*lddwork;

    if ( (nb > 1) && (nb < k) ) {
        /* Use blocked code initially */
        magma_zsetmatrix_async( m, n-nb,
                                A(0,nb),  lda,
                                dA(0,nb), ldda, stream[0] );

        old_i = 0;
        old_ib = nb;
        for (i = 0; i < k-nb; i += nb) {
            ib = min(k-i, nb);
            if (i>0) {
                magma_zgetmatrix_async( m-i, ib,
                                        dA(i,i), ldda,
                                        A(i,i),  lda, stream[1] );

                magma_zgetmatrix_async( i, ib,
                                        dA(0,i), ldda,
                                        A(0,i),  lda, stream[0] );

                /* Apply H' to A(i:m,i+2*ib:n) from the left */
                //printf( "m %4d, n %4d, nb %4d, i %4d, larfb m %4d, n %4d, k %4d\n",
                //        m, n, nb, i, m-old_i, n-old_i-2*old_ib, old_ib );
                magma_zlarfb_gpu( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise, 
                                  m-old_i, n-old_i-2*old_ib, old_ib,
                                  dA(old_i, old_i),          ldda, dT,    nb,
                                  dA(old_i, old_i+2*old_ib), ldda, dwork, lddwork);
            }

            magma_queue_sync( stream[1] );
            magma_int_t rows = m-i;
            lapackf77_zgeqrf(&rows, &ib, A(i,i), &lda, tau+i, work, &lwork, info);
            /* Form the triangular factor of the block reflector
               H = H(i) H(i+1) . . . H(i+ib-1) */
            lapackf77_zlarft( MagmaForwardStr, MagmaColumnwiseStr, 
                              &rows, &ib, A(i,i), &lda, tau+i, work, &ib);
            zpanel_to_q(MagmaUpper, ib, A(i,i), lda, work+ib*ib);
            magma_zsetmatrix( rows, ib, A(i,i), lda, dA(i,i), ldda );
            zq_to_panel(MagmaUpper, ib, A(i,i), lda, work+ib*ib);

            if (i + ib < n) {
                magma_zsetmatrix( ib, ib, work, ib, dT, nb );

                if (i+ib < k-nb) {
                    /* Apply H' to A(i:m,i+ib:i+2*ib) from the left (look-ahead) */
                    //printf( "m %4d, n %4d, nb %4d, i %4d, larfb m %4d, n %4d, k %4d, lddwork %4d (lookahead 1)\n",
                    //        m, n, nb, i, rows, ib, ib, lddwork );
                    magma_zlarfb_gpu( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise, 
                                      rows, ib, ib, 
                                      dA(i, i   ), ldda, dT,    nb, 
                                      dA(i, i+ib), ldda, dwork, lddwork);
                }
                else {
                    /* After last panel, update whole trailing matrix. */
                    /* Apply H' to A(i:m,i+ib:n) from the left */
                    //printf( "m %4d, n %4d, nb %4d, i %4d, larfb m %4d, n %4d, k %4d, lddwork %4d (last)\n",
                    //        m, n, nb, i, rows, n-i-ib, ib, lddwork );
                    magma_zlarfb_gpu( MagmaLeft, MagmaConjTrans, MagmaForward, MagmaColumnwise, 
                                      rows, n-i-ib, ib, 
                                      dA(i, i   ), ldda, dT,    nb, 
                                      dA(i, i+ib), ldda, dwork, lddwork);
                }

                old_i  = i;
                old_ib = ib;
            }
        }
    } else {
        i = 0;
    }
    
    /* Use unblocked code to factor the last or only block. */
    if (i < k) {
        ib = n-i;
        if (i != 0) {
            magma_zgetmatrix( m, ib, dA(0,i), ldda, A(0,i), lda );
        }
        magma_int_t rows = m-i;
        lapackf77_zgeqrf(&rows, &ib, A(i,i), &lda, tau+i, work, &lwork, info);
    }

    magma_queue_destroy( stream[0] );
    magma_queue_destroy( stream[1] );
    magma_free( dA );
    
    return *info;
} /* magma_zgeqrf */

