/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *
 *  (C) 2001 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */
#ifndef MPITEST_H_INCLUDED
#define MPITEST_H_INCLUDED

#include <string.h>
#include "mpitestconf.h"

/*
 * Init and finalize test
 */
void MTest_Init(int *, char ***);
void MTest_Init_thread(int *, char ***, int, int *);
void MTest_Finalize(int);
void MTestPrintError(int);
void MTestPrintErrorMsg(const char[], int);
void MTestPrintfMsg(int, const char[], ...);
void MTestError(const char[]);
int MTestReturnValue(int);

/*
 * Utilities
 */
void MTestSleep(int);
void MTestGetDbgInfo(int *dbgflag, int *verbose);

/*
 * This structure contains the information used to test datatypes
 * buf is set to null when an MTestDatatype is created; the
 * InitBuf routine will allocate (if necessary) and initialize
 * the data.  InitBuf may be called multiple times (this is particularly
 * important for recv bufs), in which case the buffer will only
 * be allocated if it has not already been created.
 */
typedef struct _MTestDatatype {
    MPI_Datatype datatype;
    void *buf;                  /* buffer to use in communication */
    MPI_Aint count;             /* count to use for this datatype */
    int isBasic;                /* true if the type is predefined */
    int printErrors;            /* true if errors should be printed
                                 * (used by the CheckBuf routines) */
    /* The following is optional data that is used by some of
     * the derived datatypes */
    int nblock, *index;
    /* stride, and blksize are in bytes */
    MPI_Aint stride, blksize, *displ_in_bytes;
    int *displs, basesize;
    MPI_Datatype *old_datatypes;
    /* used in subarray */
    int arr_sizes[2], arr_subsizes[2], arr_starts[2], order;

    void *(*InitBuf) (struct _MTestDatatype *);
    void *(*FreeBuf) (struct _MTestDatatype *);
    int (*CheckBuf) (struct _MTestDatatype *);
} MTestDatatype;

/* The max value of count must be very large to ensure that we
 *  reach the long message algorithms. (The maximal count or block length
 *  can be generated by 256K count is 4K or 32Kbytes respectively) */
#define MTEST_DATATYPE_FOR_EACH_COUNT(count) \
        for (count = 1; count <= 262144; count *= 128)

/* Setup the full version of datatype tests.
 * It generate tests for all basic datatypes and all derived datatypes except darray. */
void MTestInitFullDatatypes(void);

/* Setup the minimum version of datatype tests.
 * It generate tests for all basic datatypes, vector and indexed. */
void MTestInitMinDatatypes(void);

/* Setup the basic version of datatype tests.
 * It generate tests for all basic datatypes. */
void MTestInitBasicDatatypes(void);

int MTestCheckRecv(MPI_Status *, MTestDatatype *);
int MTestGetDatatypes(MTestDatatype *, MTestDatatype *, MPI_Aint);
void MTestResetDatatypes(void);
void MTestFreeDatatype(MTestDatatype *);
const char *MTestGetDatatypeName(MTestDatatype *);
int MTestGetDatatypeIndex(void);

int MTestGetIntracomm(MPI_Comm *, int);
int MTestGetIntracommGeneral(MPI_Comm *, int, int);
int MTestGetIntercomm(MPI_Comm *, int *, int);
int MTestGetComm(MPI_Comm *, int);
int MTestTestIntercomm(MPI_Comm intercomm);
int MTestTestIntracomm(MPI_Comm intracomm);
int MTestTestComm(MPI_Comm comm);
const char *MTestGetIntracommName(void);
const char *MTestGetIntercommName(void);
void MTestFreeComm(MPI_Comm *);

int MTestSpawnPossible(int *);

#ifdef HAVE_MPI_WIN_CREATE
int MTestGetWin(MPI_Win *, int);
const char *MTestGetWinName(void);
void MTestFreeWin(MPI_Win *);
#endif

/* These macros permit overrides via:
 *     make CPPFLAGS='-DMTEST_MPI_VERSION=X -DMTEST_MPI_SUBVERSION=Y'
 * where X and Y are the major and minor versions of the MPI standard that is
 * being tested.  The override should work similarly if added to the CPPFLAGS at
 * configure time. */
#ifndef MTEST_MPI_VERSION
#define MTEST_MPI_VERSION MPI_VERSION
#endif
#ifndef MTEST_MPI_SUBVERSION
#define MTEST_MPI_SUBVERSION MPI_SUBVERSION
#endif

/* Makes it easier to conditionally compile test code for a particular minimum
 * version of the MPI Standard.  Right now there is only a MIN flavor but it
 * would be easy to add MAX or EXACT flavors if they become necessary at some
 * point.  Example usage:
 ------8<-------
#if MTEST_HAVE_MIN_MPI_VERSION(2,2)
... test for some feature that is only available in MPI-2.2 or later ...
#endif
 ------8<-------
 */
#define MTEST_HAVE_MIN_MPI_VERSION(major_,minor_) \
    ((MTEST_MPI_VERSION == (major_) && MTEST_MPI_SUBVERSION >= (minor_)) ||   \
    (MTEST_MPI_VERSION > (major_)))

/* useful for avoid valgrind warnings about padding bytes */
#define MTEST_VG_MEM_INIT(addr_, size_) \
do {                                    \
    memset(addr_, 0, size_);            \
} while (0)

#endif
