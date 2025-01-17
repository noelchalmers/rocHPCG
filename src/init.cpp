
//@HEADER
// ***************************************************
//
// HPCG: High Performance Conjugate Gradient Benchmark
//
// Contact:
// Michael A. Heroux ( maherou@sandia.gov)
// Jack Dongarra     (dongarra@eecs.utk.edu)
// Piotr Luszczek    (luszczek@eecs.utk.edu)
//
// ***************************************************
//@HEADER

/* ************************************************************************
 * Modifications (c) 2019-2023 Advanced Micro Devices, Inc.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ************************************************************************ */

#ifndef HPCG_NO_MPI
#include <mpi.h>
#endif

#ifndef HPCG_NO_OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
const char* NULLDEVICE="nul";
#else
const char* NULLDEVICE="/dev/null";
#endif

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fstream>
#include <iostream>
#include <hip/hip_runtime_api.h>

#include "utils.hpp"
#include "hpcg.hpp"

#include "ReadHpcgDat.hpp"

hipStream_t stream_interior;
hipStream_t stream_halo;
hipEvent_t halo_gather;
void* workspace;
hipAllocator_t allocator;

std::ofstream HPCG_fout; //!< output file stream for logging activities during HPCG run

static int
startswith(const char * s, const char * prefix) {
  size_t n = strlen( prefix );
  if (strncmp( s, prefix, n ))
    return 0;
  return 1;
}

__global__ void kernel_warmup()
{
}

/*!
  Initializes an HPCG run by obtaining problem parameters (from a file or
  command line) and then broadcasts them to all nodes. It also initializes
  login I/O streams that are used throughout the HPCG run. Only MPI rank 0
  performs I/O operations.

  The function assumes that MPI has already been initialized for MPI runs.

  @param[in] argc_p the pointer to the "argc" parameter passed to the main() function
  @param[in] argv_p the pointer to the "argv" parameter passed to the main() function
  @param[out] params the reference to the data structures that is filled the basic parameters of the run

  @return returns 0 upon success and non-zero otherwise

  @see HPCG_Finalize
*/
int
HPCG_Init(int * argc_p, char ** *argv_p, HPCG_Params & params) {
  int argc = *argc_p;
  char ** argv = *argv_p;
  char fname[80];
  int i, j, *iparams;
  bool verify = true;
  double fparam = 0.0;
  char cparams[][8] = {"--nx=", "--ny=", "--nz=", "--rt=", "--pz=", "--zl=", "--zu=", "--npx=", "--npy=", "--npz=", "--dev="};
  time_t rawtime;
  tm * ptm;
  const int nparams = (sizeof cparams) / (sizeof cparams[0]);
  bool broadcastParams = false; // Make true if parameters read from file.

  iparams = (int *)malloc(sizeof(int) * nparams);

  // Initialize iparams
  for (i = 0; i < nparams; ++i) iparams[i] = 0;

  /* for sequential and some MPI implementations it's OK to read first three args */
  for (i = 0; i < nparams; ++i)
    if (argc <= i+1 || sscanf(argv[i+1], "%d", iparams+i) != 1 || iparams[i] < 10) iparams[i] = 0;

  /* for some MPI environments, command line arguments may get complicated so we need a prefix */
  for (i = 1; i <= argc && argv[i]; ++i)
  {
    for (j = 0; j < nparams; ++j)
      if (startswith(argv[i], cparams[j]))
        if (sscanf(argv[i]+strlen(cparams[j]), "%d", iparams+j) != 1)
          iparams[j] = 0;
    if(startswith(argv[i], "--tol="))
      if(sscanf(argv[i]+strlen("--tol="), "%lf", &fparam))
        verify = false;
  }

  // Check if --rt was specified on the command line
  int * rt  = iparams+3;  // Assume runtime was not specified and will be read from the hpcg.dat file
  if (iparams[3]) rt = 0; // If --rt was specified, we already have the runtime, so don't read it from file
  if (! iparams[0] && ! iparams[1] && ! iparams[2]) { /* no geometry arguments on the command line */
    ReadHpcgDat(iparams, rt, iparams+7);
    broadcastParams = true;
  }

  // Check for small or unspecified nx, ny, nz values
  // If any dimension is less than 16, make it the max over the other two dimensions, or 16, whichever is largest
  for (i = 0; i < 3; ++i) {
    if (iparams[i] < 16)
      for (j = 1; j <= 2; ++j)
        if (iparams[(i+j)%3] > iparams[i])
          iparams[i] = iparams[(i+j)%3];
    if (iparams[i] < 16)
      iparams[i] = 16;
  }

// Broadcast values of iparams to all MPI processes
#ifndef HPCG_NO_MPI
  if (broadcastParams) {
    MPI_Bcast( iparams, nparams, MPI_INT, 0, MPI_COMM_WORLD );
  }
#endif

  params.nx = iparams[0];
  params.ny = iparams[1];
  params.nz = iparams[2];

  params.runningTime = iparams[3];
  params.pz = iparams[4];
  params.zl = iparams[5];
  params.zu = iparams[6];

  params.npx = iparams[7];
  params.npy = iparams[8];
  params.npz = iparams[9];

  params.device = iparams[10];
  params.verify = verify;
  params.tol    = fparam;

#ifndef HPCG_NO_MPI
  MPI_Comm_rank( MPI_COMM_WORLD, &params.comm_rank );
  MPI_Comm_size( MPI_COMM_WORLD, &params.comm_size );
#else
  params.comm_rank = 0;
  params.comm_size = 1;
#endif

  // Simple device management
  int ndevs = 0;
  HIP_CHECK(hipGetDeviceCount(&ndevs));

  // Single GPU device can be selected via cli
  // Multi GPU devices are selected automatically
  if(params.comm_size == 1)
  {
    if(ndevs <= params.device)
    {
      fprintf(stderr, "Error: invalid device ID\n");
      hipDeviceReset();
      exit(1);
    }
  }
  else
  {
    params.device = params.comm_rank % ndevs;
  }

  // Set device
  HIP_CHECK(hipSetDevice(params.device));

  // Create streams
  HIP_CHECK(hipStreamCreate(&stream_interior));
  HIP_CHECK(hipStreamCreate(&stream_halo));

  // Create Events
  HIP_CHECK(hipEventCreate(&halo_gather));

  // Initialize memory allocator
#ifdef HPCG_MEMMGMT
  HIP_CHECK(allocator.Initialize(params.comm_rank,
                                 params.comm_size,
                                 params.nx,
                                 params.ny,
                                 params.nz));
#endif

  // Allocate device workspace
  HIP_CHECK(deviceMalloc((void**)&workspace, sizeof(global_int_t) * 1024));

#ifdef HPCG_NO_OPENMP
  params.numThreads = 1;
#else
  #pragma omp parallel
  params.numThreads = omp_get_num_threads();
#endif
//  for (i = 0; i < nparams; ++i) std::cout << "rank = "<< params.comm_rank << " iparam["<<i<<"] = " << iparams[i] << "\n";

  time ( &rawtime );
  ptm = localtime(&rawtime);
  sprintf( fname, "hpcg%04d%02d%02dT%02d%02d%02d.txt",
      1900 + ptm->tm_year, ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec );

  if (0 == params.comm_rank) {
    HPCG_fout.open(fname);
  } else {
#if defined(HPCG_DEBUG) || defined(HPCG_DETAILED_DEBUG)
    sprintf( fname, "hpcg%04d%02d%02dT%02d%02d%02d_%d.txt",
        1900 + ptm->tm_year, ptm->tm_mon+1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec, params.comm_rank );
    HPCG_fout.open(fname);
#else
    HPCG_fout.open(NULLDEVICE);
#endif
  }

  free( iparams );

  return 0;
}
