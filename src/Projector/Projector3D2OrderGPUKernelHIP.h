//! HIP CUDA implementation

#if defined( SMILEI_ACCELERATOR_MODE )

#if defined( __HIP__ )
    #include <hip/hip_runtime.h>
#endif

#include "Params.h"
#include "gpu.h"

//namespace hip {
    namespace detail {

// For HIP compiler
#if defined( __HIP__ )
        static inline void
        checkErrors( ::hipError_t an_error_code,
                     const char  *file_name,
                     int          line )
        {
            if( an_error_code != ::hipError_t::hipSuccess ) {
                std::cout << "HIP error at " << file_name << ":" << line
                          << " -> " << ::hipGetErrorString( an_error_code ) << std::endl;
                std::exit( EXIT_FAILURE );
            }
        }
// For NVIDIA compiler
#elif defined( __CUDA_ARCH__ )
        static inline void
        checkErrors( ::cudaError_t an_error_code,
                     const char  *file_name,
                     int          line )
        {
            if( an_error_code != ::cudaError_t::cudaSuccess ) {
                std::cout << "HIP error at " << file_name << ":" << line
                          << " -> " << ::cudaGetErrorString( an_error_code ) << std::endl;
                std::exit( EXIT_FAILURE );
            }
        }
#endif

    } // namespace detail

    #define checkHIPErrors( an_expression )                           \
        do {                                                          \
            detail::checkErrors( an_expression, __FILE__, __LINE__ ); \
        } while( 0 )

    //namespace kernel {
        namespace atomic {
            namespace LDS {
                __device__ void
                AddNoReturn( float *a_pointer, float a_value )
                {
        #if defined( __gfx90a__ )
                    ::unsafeAtomicAdd( a_pointer, a_value );

                    // uint32_t *as_uint32{ reinterpret_cast<uint32_t *>( a_pointer ) };
                    // uint32_t  last_seen_value{ __atomic_load_n( as_uint32, __ATOMIC_RELAXED ) };
                    // uint32_t  assumed;
                    // do {
                    //     assumed         = last_seen_value;
                    //     last_seen_value = ::atomicCAS( as_uint32,
                    //                                    last_seen_value,
                    //                                    __float_as_uint( a_value + __float_as_uint( last_seen_value ) ) );
                    // } while( assumed != last_seen_value );
        #else
                    ::atomicAdd( a_pointer, a_value );
        #endif
                }

                __device__ void
                AddNoReturn( double *a_pointer, double a_value )
                {
        #if defined( __gfx90a__ )
                    ::unsafeAtomicAdd( a_pointer, a_value );
        #else
                    ::atomicAdd( a_pointer, a_value );
        #endif
                }
            } // namespace LDS

            namespace GDS {
                __device__ void
                AddNoReturn( double *a_pointer, double a_value )
                {
        #if defined( __gfx90a__ )
                    ::unsafeAtomicAdd( a_pointer, a_value );
        #else
                    ::atomicAdd( a_pointer, a_value );
        #endif
                }
            } // namespace GDS
        }     // namespace atomic

        // template <typename ComputeFloat,
        //           typename ReductionFloat,
        //           std::size_t kWorkgroupSize>
        __global__ void
        // __launch_bounds__(kWorkgroupSize, 1)
        DepositCurrentDensity_3D_Order2( double *__restrict__ device_Jx,
                                         double *__restrict__ device_Jy,
                                         double *__restrict__ device_Jz,
                                         int Jx_size,
                                         int Jy_size,
                                         int Jz_size,
                                         const double *__restrict__ device_particle_position_x,
                                         const double *__restrict__ device_particle_position_y,
                                         const double *__restrict__ device_particle_position_z,
                                         const short *__restrict__ device_particle_charge,
                                         const double *__restrict__ device_particle_weight,
                                         const int *__restrict__ device_bin_index,
                                         const double *__restrict__ device_invgf_,
                                         int *__restrict__ device_iold,
                                         const double *__restrict__ device_deltaold_,
                                         /*ComputeFloat*/ double inv_cell_volume,
                                         /*ComputeFloat*/ double dx_inv,
                                         /*ComputeFloat*/ double dy_inv,
                                         /*ComputeFloat*/ double dz_inv,
                                         /*ComputeFloat*/ double dx_ov_dt,
                                         /*ComputeFloat*/ double dy_ov_dt,
                                         /*ComputeFloat*/ double dz_ov_dt,
                                         int          i_domain_begin,
                                         int          j_domain_begin,
                                         int          k_domain_begin,
                                         int          nprimy,
                                         int          nprimz,
                                         int          not_spectral )
        {
            // TODO(Etienne M): refactor this function. Break it into smaller
            // pieces (lds init/store, coeff computation, deposition etc..)
            // TODO(Etienne M): __ldg could be used to slightly improve GDS load
            // speed. This would only have an effect on Nvidia cards as this
            // operation is a no op on AMD.
            const unsigned int workgroup_size = 128 ;//kWorkgroupSize; // blockDim.x;
            const unsigned int bin_count      = gridDim.x * gridDim.y * gridDim.z;
            const unsigned int loop_stride    = workgroup_size; // This stride should enable better memory access coalescing

            const unsigned int x_cluster_coordinate          = blockIdx.x;
            const unsigned int y_cluster_coordinate          = blockIdx.y;
            const unsigned int z_cluster_coordinate          = blockIdx.z;
            const unsigned int workgroup_dedicated_bin_index = x_cluster_coordinate * gridDim.y * gridDim.z + y_cluster_coordinate * gridDim.z + z_cluster_coordinate; // The indexing order is: x * ywidth * zwidth + y * zwidth + z
            const unsigned int thread_index_offset           = threadIdx.x;

//#if defined ( __CUDA_ARCH__ )
//// For the moment on NVIDIA GPU we don't use the Params:: static constexpr methods such as Params::getGPUClusterWidth
//// because it causes a compilation issue : nvcc error   : 'ptxas' died due to signal 8 (Floating point exception)
//// Ideally, we should have here the same implementation between CUDA and HIP 
//            // The unit is the cell
//            const unsigned int global_x_scratch_space_coordinate_offset = x_cluster_coordinate * 4;
//            const unsigned int global_y_scratch_space_coordinate_offset = y_cluster_coordinate * 4;
//            const unsigned int global_z_scratch_space_coordinate_offset = z_cluster_coordinate * 4;
//
//            const int    GPUClusterWithGCWidth = 4 /* GPUClusterWidth */ + 5 /* GPUClusterGhostCellBorderWidth  */ ;
//#else
            // The unit is the cell
            const unsigned int global_x_scratch_space_coordinate_offset = x_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );
            const unsigned int global_y_scratch_space_coordinate_offset = y_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );
            const unsigned int global_z_scratch_space_coordinate_offset = z_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );

            const int    GPUClusterWithGCWidth = Params::getGPUClusterWithGhostCellWidth( 3 /* 3D */, 2 /* 2nd order interpolation */ );
//#endif
            /*ComputeFloat*/ double one_third             = 1. / 3.;

            // NOTE: We gain from the particles not being sorted inside a
            // cluster because it reduces the bank conflicts one gets when
            // multiple threads access the same part of the shared memory. Such
            // "conflicted" accesses are serialized !
            // NOTE: We use a bit to much LDS. For Jx, the first row could be
            // discarded, for Jy we could remove the first column.

//#if defined ( __CUDA_ARCH__ )
//            static constexpr unsigned int kFieldScratchSpaceSize = 9*9*9;
//#else
            static constexpr unsigned int kFieldScratchSpaceSize = Params::getGPUInterpolationClusterCellVolume( 3 /* 3D */, 2 /* 2nd order interpolation */ );
//#endif

            // NOTE: I tried having only one cache and reusing it. Doing that
            // requires you to iterate multiple time over the particle which is
            // possible but cost more bandwidth. The speedup was ~x0.92.
            __shared__ /*ReductionFloat*/ double Jx_scratch_space[kFieldScratchSpaceSize];
            __shared__ /*ReductionFloat*/ double Jy_scratch_space[kFieldScratchSpaceSize];
            __shared__ /*ReductionFloat*/ double Jz_scratch_space[kFieldScratchSpaceSize];

            // Init the shared memory

            for( unsigned int field_index = thread_index_offset;
                 field_index < kFieldScratchSpaceSize;
                 field_index += workgroup_size ) {
                Jx_scratch_space[field_index] = static_cast</*ReductionFloat*/ double>( 0.0 );
                Jy_scratch_space[field_index] = static_cast</*ReductionFloat*/ double>( 0.0 );
                Jz_scratch_space[field_index] = static_cast</*ReductionFloat*/ double>( 0.0 );
            }

            __syncthreads();

            const unsigned int particle_count = device_bin_index[bin_count - 1];

            // This workgroup has to process distance(last_particle,
            // first_particle) particles
            const unsigned int first_particle = workgroup_dedicated_bin_index == 0 ? 0 :
                                                                                     device_bin_index[workgroup_dedicated_bin_index - 1];
            const unsigned int last_particle  = device_bin_index[workgroup_dedicated_bin_index];

            for( unsigned int particle_index = first_particle + thread_index_offset;
                 particle_index < last_particle;
                 particle_index += loop_stride ) {
                const /*ComputeFloat*/ double invgf                  = static_cast</*ComputeFloat*/ double>( device_invgf_[particle_index] );
                const int *const __restrict__ iold        = &device_iold[particle_index];
                const double *const __restrict__ deltaold = &device_deltaold_[particle_index];

                /*ComputeFloat*/ double Sx0[5];
                /*ComputeFloat*/ double Sx1[5];
                /*ComputeFloat*/ double Sy0[5];
                /*ComputeFloat*/ double Sy1[5];
                /*ComputeFloat*/ double Sz0[5];
                /*ComputeFloat*/ double Sz1[5];
                // double DSx[5];
                // double DSy[5];

                // Variable declaration & initialization
                // Esirkepov's paper: https://arxiv.org/pdf/physics/9901047.pdf

                // Locate the particle on the primal grid at former time-step & calculate coeff. S0
                {
                    const /*ComputeFloat*/ double delta  = deltaold[0 * particle_count];
                    const /*ComputeFloat*/ double delta2 = delta * delta;

                    Sx0[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sx0[1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sx0[2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sx0[3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sx0[4] = static_cast</*ComputeFloat*/ double>( 0.0 );
                }
                {
                    const /*ComputeFloat*/ double delta  = deltaold[1 * particle_count];
                    const /*ComputeFloat*/ double delta2 = delta * delta;

                    Sy0[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sy0[1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sy0[2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sy0[3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sy0[4] = static_cast</*ComputeFloat*/ double>( 0.0 );
                }
                {
                    const /*ComputeFloat*/ double delta  = deltaold[2 * particle_count];
                    const /*ComputeFloat*/ double delta2 = delta * delta;

                    Sz0[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sz0[1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sz0[2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sz0[3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sz0[4] = static_cast</*ComputeFloat*/ double>( 0.0 );
                }

                // Locate the particle on the primal grid at current time-step & calculate coeff. S1
                {
                    // const int    ip             = static_cast<int>( xpn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double xpn      = static_cast</*ComputeFloat*/ double>( device_particle_position_x[particle_index] ) * dx_inv;
                    const int          ip       = std::round( xpn );
                    const int          ipo      = iold[0 * particle_count];
                    const int          ip_m_ipo = ip - ipo - i_domain_begin;
                    const /*ComputeFloat*/ double delta    = xpn - static_cast</*ComputeFloat*/ double>( ip );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sx1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sx1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sx1[2] = 0.0; // Always set below
                    Sx1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sx1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sx1[ip_m_ipo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sx1[ip_m_ipo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sx1[ip_m_ipo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }
                {
                    // const int    jp             = static_cast<int>( ypn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double ypn      = static_cast</*ComputeFloat*/ double>( device_particle_position_y[particle_index] ) * dy_inv;
                    const int          jp       = std::round( ypn );
                    const int          jpo      = iold[1 * particle_count];
                    const int          jp_m_jpo = jp - jpo - j_domain_begin;
                    const /*ComputeFloat*/ double delta    = ypn - static_cast</*ComputeFloat*/ double>( jp );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sy1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sy1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sy1[2] = 0.0; // Always set below
                    Sy1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sy1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sy1[jp_m_jpo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sy1[jp_m_jpo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sy1[jp_m_jpo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }
                {
                    // const int    kp             = static_cast<int>( zpn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double zpn      = static_cast</*ComputeFloat*/ double>( device_particle_position_z[particle_index] ) * dz_inv;
                    const int          kp       = std::round( zpn );
                    const int          kpo      = iold[2 * particle_count];
                    const int          kp_m_kpo = kp - kpo - k_domain_begin;
                    const /*ComputeFloat*/ double delta    = zpn - static_cast</*ComputeFloat*/ double>( kp );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sz1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sz1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sz1[2] = 0.0; // Always set below
                    Sz1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sz1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sz1[kp_m_kpo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sz1[kp_m_kpo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sz1[kp_m_kpo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }

                // (x,y,z) components of the current density for the macro-particle
                const /*ComputeFloat*/ double charge_weight = inv_cell_volume * static_cast</*ComputeFloat*/ double>( device_particle_charge[particle_index] ) * static_cast</*ComputeFloat*/ double>( device_particle_weight[particle_index] );
                const /*ComputeFloat*/ double crx_p         = charge_weight * dx_ov_dt;
                const /*ComputeFloat*/ double cry_p         = charge_weight * dy_ov_dt;
                const /*ComputeFloat*/ double crz_p         = charge_weight * dz_ov_dt;

                // This is the particle position as grid index
                // This minus 2 come from the order 2 scheme, based on a 5 points stencil from -2 to +2.
                const int ipo = iold[0 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_x_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;
                const int jpo = iold[1 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_y_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;
                const int kpo = iold[2 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_z_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;

                // Jx

                for( unsigned int j = 0; j < 5; ++j ) {
                    for( unsigned int k = 0; k < 5; ++k ) {
                        /*ComputeFloat*/ double tmp = crx_p * (   Sy0[j]*Sz0[k] 
                                                     + static_cast</*ComputeFloat*/ double>( 0.5 )     * ( ( Sy1[j] - Sy0[j] )*Sz0[k] + ( Sz1[k] - Sz0[k] )*Sy0[j] ) 
                                                     +                           one_third  *   ( Sy1[j] - Sy0[j] )    *     ( Sz1[k] - Sz0[k] ) );
                        /*ComputeFloat*/ double tmp_reduction{};
                        const int jk_loc = ( ipo * GPUClusterWithGCWidth + jpo + j ) * GPUClusterWithGCWidth + kpo + k;
                        for( unsigned int i = 1; i < 5; ++i ) {
                            tmp_reduction -= ( Sx1[i-1] - Sx0[i-1] ) * tmp;
                            const int loc = i*GPUClusterWithGCWidth*GPUClusterWithGCWidth + jk_loc;
                            atomic::LDS::AddNoReturn( &Jx_scratch_space[loc], static_cast</*ReductionFloat*/ double>( tmp_reduction ) );
                        }
                    }
                }

                // Jy

                for( unsigned int i = 0; i < 5; ++i ) {
                    for( unsigned int k = 0; k < 5; ++k ) {
                        /*ComputeFloat*/ double tmp = cry_p * (   Sx0[i]*Sz0[k] 
                                                     + static_cast</*ComputeFloat*/ double>( 0.5 )     * ( ( Sx1[i] - Sx0[i] )*Sz0[k] + ( Sz1[k] - Sz0[k] )*Sx0[i] ) 
                                                     +                           one_third  *   ( Sx1[i] - Sx0[i] )    *     ( Sz1[k] - Sz0[k] ) );
                        /*ComputeFloat*/ double tmp_reduction{};
                        const int ik_loc = (( i + ipo ) * GPUClusterWithGCWidth + jpo ) * GPUClusterWithGCWidth + kpo + k;
                        for( unsigned int j = 1; j < 5; ++j ) {
                            tmp_reduction -= ( Sy1[j-1] - Sy0[j-1] ) * tmp;
                            const int loc = j*GPUClusterWithGCWidth + ik_loc;
                            atomic::LDS::AddNoReturn( &Jy_scratch_space[loc], static_cast</*ReductionFloat*/ double>( tmp_reduction ) );
                        }
                    }
                }

                // Jz

                for( unsigned int i = 0; i < 5; ++i ) {
                    for( unsigned int j = 0; j < 5; ++j ) {
                        /*ComputeFloat*/ double tmp = crz_p * (   Sx0[i]*Sy0[j] 
                                                     + static_cast</*ComputeFloat*/ double>( 0.5 )     * ( ( Sx1[i] - Sx0[i] )*Sy0[j] + ( Sy1[j] - Sy0[j] )*Sx0[i] ) 
                                                     +                           one_third  *   ( Sx1[i] - Sx0[i] )    *     ( Sy1[j] - Sy0[j] ) );
                        /*ComputeFloat*/ double tmp_reduction{};
                        const int ij_loc = (( i + ipo ) * GPUClusterWithGCWidth + (jpo + j)) * GPUClusterWithGCWidth + kpo;
                        for( unsigned int k = 1; k < 5; ++k ) {
                            tmp_reduction -= ( Sz1[k-1] - Sz0[k-1] ) * tmp;
                            const int loc =  k  + ij_loc;
                            atomic::LDS::AddNoReturn( &Jz_scratch_space[loc], static_cast</*ReductionFloat*/ double>( tmp_reduction ) );
                        }
                    }
                }
            } //end particule loop

            __syncthreads();

            for( unsigned int field_index = thread_index_offset;
                 field_index < kFieldScratchSpaceSize;
                 field_index += workgroup_size ) {

                // The indexing order is: x * ywidth * zwidth + y * zwidth + z
                const unsigned int local_x_scratch_space_coordinate = field_index / ( GPUClusterWithGCWidth * GPUClusterWithGCWidth );
                const unsigned int local_y_scratch_space_coordinate = ( field_index % ( GPUClusterWithGCWidth * GPUClusterWithGCWidth ) ) / GPUClusterWithGCWidth;
                const unsigned int local_z_scratch_space_coordinate = field_index % GPUClusterWithGCWidth;

                const unsigned int global_x_scratch_space_coordinate = global_x_scratch_space_coordinate_offset + local_x_scratch_space_coordinate;
                const unsigned int global_y_scratch_space_coordinate = global_y_scratch_space_coordinate_offset + local_y_scratch_space_coordinate;
                const unsigned int global_z_scratch_space_coordinate = global_z_scratch_space_coordinate_offset + local_z_scratch_space_coordinate;

                // The indexing order is: x * ywidth * zwidth + y * zwidth + z
                const unsigned int global_memory_index = ( global_x_scratch_space_coordinate * nprimy + global_y_scratch_space_coordinate ) * nprimz + global_z_scratch_space_coordinate;

                // These atomics are basically free (very few of them).
                atomic::GDS::AddNoReturn( &device_Jx[global_memory_index],                                                                                             static_cast<double>( Jx_scratch_space[field_index] ) );
                atomic::GDS::AddNoReturn( &device_Jy[global_memory_index + /* We handle the FTDT/picsar */ not_spectral * global_x_scratch_space_coordinate * nprimz], static_cast<double>( Jy_scratch_space[field_index] ) );
                atomic::GDS::AddNoReturn( &device_Jz[global_memory_index + /* We handle the FTDT/picsar */ not_spectral * (global_x_scratch_space_coordinate * nprimy + global_y_scratch_space_coordinate)],                                                                                             static_cast<double>(  Jz_scratch_space[field_index] ) );
            }
        } // end DepositCurrent


        // template <typename ComputeFloat,
        //           typename ReductionFloat,
        //           std::size_t kWorkgroupSize>
        __global__ void
        // __launch_bounds__(kWorkgroupSize, 1)
        DepositDensity_3D_Order2( 
                                            double *__restrict__ device_rho,
                                            int rho_size,
                                            const double *__restrict__ device_particle_position_x,
                                            const double *__restrict__ device_particle_position_y,
                                            const double *__restrict__ device_particle_position_z,
                                            const short *__restrict__ device_particle_charge,
                                            const double *__restrict__ device_particle_weight,
                                            const int *__restrict__ device_bin_index,
                                            const double *__restrict__ device_invgf_,
                                            int *__restrict__ device_iold,
                                            const double *__restrict__ device_deltaold_,
                                            /*ComputeFloat*/ double inv_cell_volume,
                                            /*ComputeFloat*/ double dx_inv,
                                            /*ComputeFloat*/ double dy_inv,
                                            /*ComputeFloat*/ double dz_inv,
                                            /*ComputeFloat*/ double dx_ov_dt,
                                            /*ComputeFloat*/ double dy_ov_dt,
                                            /*ComputeFloat*/ double dz_ov_dt,
                                            int          i_domain_begin,
                                            int          j_domain_begin,
                                            int          k_domain_begin,
                                            int          nprimy,
                                            int          nprimz,
                                            int          not_spectral )
        {
            // TODO(Etienne M): refactor this function. Break it into smaller
            // pieces (lds init/store, coeff computation, deposition etc..)
            // TODO(Etienne M): __ldg could be used to slightly improve GDS load
            // speed. This would only have an effect on Nvidia cards as this
            // operation is a no op on AMD.
            const unsigned int workgroup_size = 128 ;//kWorkgroupSize; // blockDim.x;
            const unsigned int bin_count      = gridDim.x * gridDim.y * gridDim.z;
            const unsigned int loop_stride    = workgroup_size; // This stride should enable better memory access coalescing

            const unsigned int x_cluster_coordinate          = blockIdx.x;
            const unsigned int y_cluster_coordinate          = blockIdx.y;
            const unsigned int z_cluster_coordinate          = blockIdx.z;
            const unsigned int workgroup_dedicated_bin_index = x_cluster_coordinate * gridDim.y * gridDim.z + y_cluster_coordinate * gridDim.z + z_cluster_coordinate; // The indexing order is: x * ywidth * zwidth + y * zwidth + z
            const unsigned int thread_index_offset           = threadIdx.x;

            // The unit is the cell
            const unsigned int global_x_scratch_space_coordinate_offset = x_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );
            const unsigned int global_y_scratch_space_coordinate_offset = y_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );
            const unsigned int global_z_scratch_space_coordinate_offset = z_cluster_coordinate * Params::getGPUClusterWidth( 3 /* 3D */ );

            const int    GPUClusterWithGCWidth = Params::getGPUClusterWithGhostCellWidth( 3 /* 3D */, 2 /* 2nd order interpolation */ );
            /*ComputeFloat*/ double one_third             = 1. / 3.;

            // NOTE: We gain from the particles not being sorted inside a
            // cluster because it reduces the bank conflicts one gets when
            // multiple threads access the same part of the shared memory. Such
            // "conflicted" accesses are serialized !
            // NOTE: We use a bit to much LDS. For Jx, the first row could be
            // discarded, for Jy we could remove the first column.

            static constexpr unsigned int kFieldScratchSpaceSize = Params::getGPUInterpolationClusterCellVolume( 3 /* 3D */, 2 /* 2nd order interpolation */ );

            // NOTE: I tried having only one cache and reusing it. Doing that
            // requires you to iterate multiple time over the particle which is
            // possible but cost more bandwidth. The speedup was ~x0.92.
            __shared__ /*ReductionFloat*/ double rho_scratch_space[kFieldScratchSpaceSize];

            // Init the shared memory

            for( unsigned int field_index = thread_index_offset;
                 field_index < kFieldScratchSpaceSize;
                 field_index += workgroup_size ) {
                rho_scratch_space[field_index] = static_cast</*ReductionFloat*/ double>( 0.0 );
            }

            __syncthreads();

            const unsigned int particle_count = device_bin_index[bin_count - 1];

            // This workgroup has to process distance(last_particle,
            // first_particle) particles
            const unsigned int first_particle = workgroup_dedicated_bin_index == 0 ? 0 :
                                                                                     device_bin_index[workgroup_dedicated_bin_index - 1];
            const unsigned int last_particle  = device_bin_index[workgroup_dedicated_bin_index];

            for( unsigned int particle_index = first_particle + thread_index_offset;
                 particle_index < last_particle;
                 particle_index += loop_stride ) {
                const /*ComputeFloat*/ double invgf                  = static_cast</*ComputeFloat*/ double>( device_invgf_[particle_index] );
                const int *const __restrict__ iold        = &device_iold[particle_index];
                const double *const __restrict__ deltaold = &device_deltaold_[particle_index];


                /*ComputeFloat*/ double Sx1[5];
                /*ComputeFloat*/ double Sy1[5];
                /*ComputeFloat*/ double Sz1[5];
                // double DSx[5];
                // double DSy[5];

                // Variable declaration & initialization

                // Locate the particle on the primal grid at current time-step & calculate coeff. S1
                {
                    // const int    ip             = static_cast<int>( xpn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double xpn      = static_cast</*ComputeFloat*/ double>( device_particle_position_x[particle_index] ) * dx_inv;
                    const int          ip       = std::round( xpn );
                    const int          ipo      = iold[0 * particle_count];
                    const int          ip_m_ipo = ip - ipo - i_domain_begin;
                    const /*ComputeFloat*/ double delta    = xpn - static_cast</*ComputeFloat*/ double>( ip );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sx1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sx1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sx1[2] = 0.0; // Always set below
                    Sx1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sx1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sx1[ip_m_ipo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sx1[ip_m_ipo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sx1[ip_m_ipo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }
                {
                    // const int    jp             = static_cast<int>( ypn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double ypn      = static_cast</*ComputeFloat*/ double>( device_particle_position_y[particle_index] ) * dy_inv;
                    const int          jp       = std::round( ypn );
                    const int          jpo      = iold[1 * particle_count];
                    const int          jp_m_jpo = jp - jpo - j_domain_begin;
                    const /*ComputeFloat*/ double delta    = ypn - static_cast</*ComputeFloat*/ double>( jp );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sy1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sy1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sy1[2] = 0.0; // Always set below
                    Sy1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sy1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sy1[jp_m_jpo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sy1[jp_m_jpo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sy1[jp_m_jpo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }
                {
                    // const int    kp             = static_cast<int>( zpn + 0.5 ); // std::round | rounding approximation which is correct enough and faster in this case
                    const /*ComputeFloat*/ double zpn      = static_cast</*ComputeFloat*/ double>( device_particle_position_z[particle_index] ) * dz_inv;
                    const int          kp       = std::round( zpn );
                    const int          kpo      = iold[2 * particle_count];
                    const int          kp_m_kpo = kp - kpo - k_domain_begin;
                    const /*ComputeFloat*/ double delta    = zpn - static_cast</*ComputeFloat*/ double>( kp );
                    const /*ComputeFloat*/ double delta2   = delta * delta;

                    Sz1[0] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sz1[1] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    // Sz1[2] = 0.0; // Always set below
                    Sz1[3] = static_cast</*ComputeFloat*/ double>( 0.0 );
                    Sz1[4] = static_cast</*ComputeFloat*/ double>( 0.0 );

                    Sz1[kp_m_kpo + 1] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 - delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                    Sz1[kp_m_kpo + 2] = static_cast</*ComputeFloat*/ double>( 0.75 ) - delta2;
                    Sz1[kp_m_kpo + 3] = static_cast</*ComputeFloat*/ double>( 0.5 ) * ( delta2 + delta + static_cast</*ComputeFloat*/ double>( 0.25 ) );
                }

                // (x,y,z) components of the current density for the macro-particle
                const /*ComputeFloat*/ double charge_weight = inv_cell_volume * static_cast</*ComputeFloat*/ double>( device_particle_charge[particle_index] ) * static_cast</*ComputeFloat*/ double>( device_particle_weight[particle_index] );
                // const ComputeFloat crx_p         = charge_weight * dx_ov_dt;
                // const ComputeFloat cry_p         = charge_weight * dy_ov_dt;
                // const ComputeFloat crz_p         = charge_weight * dz_ov_dt;

                // This is the particle position as grid index
                // This minus 2 come from the order 2 scheme, based on a 5 points stencil from -2 to +2.
                const int ipo = iold[0 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_x_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;
                const int jpo = iold[1 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_y_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;
                const int kpo = iold[2 * particle_count] -
                                2 /* Offset so we dont uses negative numbers in the loop */ -
                                global_z_scratch_space_coordinate_offset /* Offset to get cluster relative coordinates */;

                // Rho

                for( unsigned int i = 0; i < 5; ++i ) {
                    for( unsigned int j = 0; j < 5; ++j ) {
                        /*ComputeFloat*/ double tmp = charge_weight * Sx1[i]*Sy1[j]; 
                        const int ij_loc = (( i + ipo ) * GPUClusterWithGCWidth + (jpo + j)) * GPUClusterWithGCWidth + kpo;
                        for( unsigned int k = 0; k < 5; ++k ) {
                            const int loc =  ij_loc + k;
                            atomic::LDS::AddNoReturn( &rho_scratch_space[loc], static_cast</*ReductionFloat*/ double>( tmp * Sz1[k] ) );
                        }
                    }
                }
            } //end loop particule

            __syncthreads();

            for( unsigned int field_index = thread_index_offset;
                 field_index < kFieldScratchSpaceSize;
                 field_index += workgroup_size ) {

                // The indexing order is: x * ywidth * zwidth + y * zwidth + z
                const unsigned int local_x_scratch_space_coordinate = field_index / ( GPUClusterWithGCWidth * GPUClusterWithGCWidth );
                const unsigned int local_y_scratch_space_coordinate = ( field_index % ( GPUClusterWithGCWidth * GPUClusterWithGCWidth ) ) / GPUClusterWithGCWidth;
                const unsigned int local_z_scratch_space_coordinate = field_index % GPUClusterWithGCWidth;

                const unsigned int global_x_scratch_space_coordinate = global_x_scratch_space_coordinate_offset + local_x_scratch_space_coordinate;
                const unsigned int global_y_scratch_space_coordinate = global_y_scratch_space_coordinate_offset + local_y_scratch_space_coordinate;
                const unsigned int global_z_scratch_space_coordinate = global_z_scratch_space_coordinate_offset + local_z_scratch_space_coordinate;

                // The indexing order is: x * ywidth * zwidth + y * zwidth + z
                const unsigned int global_memory_index = ( global_x_scratch_space_coordinate * nprimy + global_y_scratch_space_coordinate ) * nprimz + global_z_scratch_space_coordinate;

                // These atomics are basically free (very few of them).
                atomic::GDS::AddNoReturn( &device_rho[global_memory_index], static_cast<double>( rho_scratch_space[field_index] ) );
            }
        }


   //} // namespace kernel

static inline void
    currentDepositionKernel3D( double *__restrict__ host_Jx,
                               double *__restrict__ host_Jy,
                               double *__restrict__ host_Jz,
                               int Jx_size,
                               int Jy_size,
                               int Jz_size,
                               const double *__restrict__ device_particle_position_x,
                               const double *__restrict__ device_particle_position_y,
                               const double *__restrict__ device_particle_position_z,
                               const short *__restrict__ device_particle_charge,
                               const double *__restrict__ device_particle_weight,
                               const int *__restrict__ host_bin_index,
                               unsigned int x_dimension_bin_count,
                               unsigned int y_dimension_bin_count,
                               unsigned int z_dimension_bin_count,
                               const double *__restrict__ host_invgf_,
                               int *__restrict__ host_iold,
                               const double *__restrict__ host_deltaold_,
                               const unsigned int number_of_particles,
                               double inv_cell_volume,
                               double dx_inv,
                               double dy_inv,
                               double dz_inv,
                               double dx_ov_dt,
                               double dy_ov_dt,
                               double dz_ov_dt,
                               int    i_domain_begin,
                               int    j_domain_begin,
                               int    k_domain_begin,
                               int    nprimy,
                               int    nprimz,
                               int    not_spectral )
    {
        SMILEI_ASSERT( Params::getGPUClusterWidth( 3 /* 2D */ ) != -1 &&
                       Params::getGPUClusterGhostCellBorderWidth( 2 /* 2nd order interpolation */ ) != -1 );

        // NOTE:
        // This cluster is very strongly bound by atomic operations in LDS (shared memory)
        // TODO(Etienne M): Find a way to lessen the atomic usage

        const ::dim3 kGridDimension /* In blocks */ { static_cast<uint32_t>( x_dimension_bin_count ), static_cast<uint32_t>( y_dimension_bin_count ), static_cast<uint32_t>( z_dimension_bin_count ) };

        static constexpr std::size_t kWorkgroupSize = 128;
        const ::dim3                 kBlockDimension{ static_cast<uint32_t>( kWorkgroupSize ), 1, 1 };

        // NOTE: On cards lacking hardware backed Binary64 atomic operations,
        // falling back to Binary32 (supposing hardware support for atomic
        // operations) can lead to drastic performance improvement.
        // One just need to assign 'float' to ReductionFloat.
        //
        // using ComputeFloat   = double;
        // using ReductionFloat = double;


#if defined ( __HIP__ )
        auto KernelFunction = kernel::DepositCurrentDensity_3D_Order2<ComputeFloat, ReductionFloat, kWorkgroupSize>;
        //auto KernelFunction = /*kernel::*/ DepositCurrentDensity_3D_Order2;
        hipLaunchKernelGGL
                          ( KernelFunction,
                            kGridDimension,
                            kBlockDimension,
                            0, // Shared memory
                            0, // Stream
                            // Kernel arguments
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jx ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jy ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jz ),
                            Jx_size, Jy_size, Jz_size,
                            device_particle_position_x,
                            device_particle_position_y,
                            device_particle_position_z,
                            device_particle_charge,
                            device_particle_weight,
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_bin_index ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_invgf_ ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_iold ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_deltaold_ ),
                            inv_cell_volume,
                            dx_inv, dy_inv, dz_inv,
                            dx_ov_dt, dy_ov_dt, dz_ov_dt,
                            i_domain_begin, j_domain_begin, k_domain_begin,
                            nprimy, nprimz,
                            not_spectral );

        checkHIPErrors( ::hipDeviceSynchronize() );

#elif defined ( __CUDA_ARCH__ )
        DepositCurrentDensity_3D_Order2 <<< 
                            kGridDimension,
                            kBlockDimension
                       //     0, // Shared memory
                       //     0, // Stream
                       >>>
                       (
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jx ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jy ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_Jz ),
                            Jx_size, Jy_size, Jz_size,
                            device_particle_position_x,
                            device_particle_position_y,
                            device_particle_position_z,
                            device_particle_charge,
                            device_particle_weight,
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_bin_index ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_invgf_ ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_iold ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_deltaold_ ),
                            inv_cell_volume,
                            dx_inv, dy_inv, dz_inv,
                            dx_ov_dt, dy_ov_dt, dz_ov_dt,
                            i_domain_begin, j_domain_begin, k_domain_begin,
                            nprimy, nprimz,
                            not_spectral
                       );
        checkHIPErrors( ::cudaDeviceSynchronize() );
#endif
    }

    static inline void
    densityDepositionKernel3D( 
                                         double *__restrict__ host_rho,
                                         int rho_size,
                                         const double *__restrict__ device_particle_position_x,
                                         const double *__restrict__ device_particle_position_y,
                                         const double *__restrict__ device_particle_position_z,
                                         const short *__restrict__ device_particle_charge,
                                         const double *__restrict__ device_particle_weight,
                                         const int *__restrict__ host_bin_index,
                                         unsigned int x_dimension_bin_count,
                                         unsigned int y_dimension_bin_count,
                                         unsigned int z_dimension_bin_count,
                                         const double *__restrict__ host_invgf_,
                                         int *__restrict__ host_iold,
                                         const double *__restrict__ host_deltaold_,
                                         const unsigned int number_of_particles,
                                         double inv_cell_volume,
                                         double dx_inv,
                                         double dy_inv,
                                         double dz_inv,
                                         double dx_ov_dt,
                                         double dy_ov_dt,
                                         double dz_ov_dt,
                                         int    i_domain_begin,
                                         int    j_domain_begin,
                                         int    k_domain_begin,
                                         int    nprimy,
                                         int    nprimz,
                                         int    not_spectral )
    {
        SMILEI_ASSERT( Params::getGPUClusterWidth( 3 /* 2D */ ) != -1 &&
                       Params::getGPUClusterGhostCellBorderWidth( 2 /* 2nd order interpolation */ ) != -1 );

        // NOTE:
        // This cluster is very strongly bound by atomic operations in LDS (shared memory)
        // TODO(Etienne M): Find a way to lessen the atomic usage

        const ::dim3 kGridDimension /* In blocks */ { static_cast<uint32_t>( x_dimension_bin_count ), static_cast<uint32_t>( y_dimension_bin_count ), static_cast<uint32_t>( z_dimension_bin_count ) };

        static constexpr std::size_t kWorkgroupSize = 128;
        const ::dim3                 kBlockDimension{ static_cast<uint32_t>( kWorkgroupSize ), 1, 1 };

        // NOTE: On cards lacking hardware backed Binary64 atomic operations,
        // falling back to Binary32 (supposing hardware support for atomic
        // operations) can lead to drastic performance improvement.
        // One just need to assign 'float' to ReductionFloat.
        //
        // using ComputeFloat   = double;
        // using ReductionFloat = double;


#if defined ( __HIP__ )

        auto KernelFunction = kernel::DepositDensity_3D_Order2<ComputeFloat, ReductionFloat, kWorkgroupSize>;
        //auto KernelFunction = /*kernel::*/DepositDensity_3D_Order2;
        
        hipLaunchKernelGGL( KernelFunction,
                            kGridDimension,
                            kBlockDimension,
                            0, // Shared memory
                            0, // Stream
                            // Kernel arguments
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_rho ),
                            rho_size,
                            device_particle_position_x,
                            device_particle_position_y,
                            device_particle_position_z,
                            device_particle_charge,
                            device_particle_weight,
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_bin_index ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_invgf_ ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_iold ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_deltaold_ ),
                            inv_cell_volume,
                            dx_inv, dy_inv, dz_inv,
                            dx_ov_dt, dy_ov_dt, dz_ov_dt,
                            i_domain_begin, j_domain_begin, k_domain_begin,
                            nprimy, nprimz,
                            not_spectral );

        checkHIPErrors( ::hipDeviceSynchronize() );
#elif defined ( __CUDA_ARCH__ )
        DepositDensity_3D_Order2 <<<
                            kGridDimension,
                            kBlockDimension,
                            0, // Shared memory
                            0 // Stream
                       >>>
                       (
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_rho ),
                            rho_size,
                            device_particle_position_x,
                            device_particle_position_y,
                            device_particle_position_z,
                            device_particle_charge,
                            device_particle_weight,
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_bin_index ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_invgf_ ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_iold ),
                            smilei::tools::gpu::HostDeviceMemoryManagement::GetDevicePointer( host_deltaold_ ),
                            inv_cell_volume,
                            dx_inv, dy_inv, dz_inv,
                            dx_ov_dt, dy_ov_dt, dz_ov_dt,
                            i_domain_begin, j_domain_begin, k_domain_begin,
                            nprimy, nprimz,
                            not_spectral
                       );
        checkHIPErrors( ::cudaDeviceSynchronize() );
#endif
    }


//} // namespace hip

#endif
