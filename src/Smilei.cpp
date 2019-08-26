////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////                                                                                                                ////
////                                                                                                                ////
////                                   PARTICLE-IN-CELL CODE SMILEI                                                 ////
////                    Simulation of Matter Irradiated by Laser at Extreme Intensity                               ////
////                                                                                                                ////
////                          Cooperative OpenSource Object-Oriented Project                                        ////
////                                      from the Plateau de Saclay                                                ////
////                                          started January 2013                                                  ////
////                                                                                                                ////
////                                                                                                                ////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <ctime>
#include <cstdlib>
#include <unistd.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <omp.h>

#include "Smilei.h"
#include "SmileiMPI_test.h"
#include "Params.h"
#include "PatchesFactory.h"
#include "SyncVectorPatch.h"
#include "Checkpoint.h"
#include "Solver.h"
#include "SimWindow.h"
#include "Diagnostic.h"
#include "Domain.h"
#include "SyncCartesianPatch.h"
#include "Timers.h"
#include "RadiationTables.h"
#include "MultiphotonBreitWheelerTables.h"

using namespace std;

// ---------------------------------------------------------------------------------------------------------------------
//                                                   MAIN CODE
// ---------------------------------------------------------------------------------------------------------------------
int main( int argc, char *argv[] )
{
    cout.setf( ios::fixed,  ios::floatfield ); // floatfield set to fixed

    // -------------------------
    // Simulation Initialization
    // -------------------------

    // Create MPI environment :

#ifdef SMILEI_TESTMODE
    SmileiMPI_test smpi( &argc, &argv );
#else
    SmileiMPI smpi( &argc, &argv );
#endif

    MESSAGE( "                   _            _" );
    MESSAGE( " ___           _  | |        _  \\ \\   Version : " << __VERSION );
    MESSAGE( "/ __|  _ __   (_) | |  ___  (_)  | |   " );
    MESSAGE( "\\__ \\ | '  \\   _  | | / -_)  _   | |" );
    MESSAGE( "|___/ |_|_|_| |_| |_| \\___| |_|  | |  " );
    MESSAGE( "                                /_/    " );
    MESSAGE( "" );

    // Read and print simulation parameters
    TITLE( "Reading the simulation parameters" );
    Params params( &smpi, vector<string>( argv + 1, argv + argc ) );
    OpenPMDparams openPMD( params );

    // Need to move it here because of domain decomposition need in smpi->init(_patch_count)
    //     abstraction of Hilbert curve
    VectorPatch vecPatches( params );

    // Initialize MPI environment with simulation parameters
    TITLE( "Initializing MPI" );
    smpi.init( params, vecPatches.domain_decomposition_ );

    // Create timers
    Timers timers( &smpi );

    // Print in stdout MPI, OpenMP, patchs parameters
    params.print_parallelism_params( &smpi );

    TITLE( "Initializing the restart environment" );
    Checkpoint checkpoint( params, &smpi );

    // ------------------------------------------------------------------------
    // Initialize the simulation times time_prim at n=0 and time_dual at n=+1/2
    // Update in "if restart" if necessary
    // ------------------------------------------------------------------------

    // time at integer time-steps (primal grid)
    double time_prim = 0;
    // time at half-integer time-steps (dual grid)
    double time_dual = 0.5 * params.timestep;

    // -------------------------------------------
    // Declaration of the main objects & operators
    // -------------------------------------------
    // --------------------
    // Define Moving Window
    // --------------------
    TITLE( "Initializing moving window" );
    SimWindow *simWindow = new SimWindow( params );

    // ------------------------------------------------------------------------
    // Init nonlinear inverse Compton scattering
    // ------------------------------------------------------------------------
    RadiationTables RadiationTables;

    // ------------------------------------------------------------------------
    // Create MultiphotonBreitWheelerTables object for multiphoton
    // Breit-Wheeler pair creation
    // ------------------------------------------------------------------------
    MultiphotonBreitWheelerTables MultiphotonBreitWheelerTables;

    // ---------------------------------------------------
    // Initialize patches (including particles and fields)
    // ---------------------------------------------------
    TITLE( "Initializing particles & fields" );

    if( smpi.test_mode ) {
        execute_test_mode( vecPatches, &smpi, simWindow, params, checkpoint, openPMD );
        return 0;
    }


    // ---------------------------------------------------------------------
    // Init and compute tables for radiation effects
    // (nonlinear inverse Compton scattering)
    // ---------------------------------------------------------------------
    RadiationTables.initializeParameters( params, &smpi);
    //RadiationTables.computeTables( params, &smpi );
    //RadiationTables.outputTables( &smpi );

    // ---------------------------------------------------------------------
    // Init and compute tables for multiphoton Breit-Wheeler pair creation
    // ---------------------------------------------------------------------
    MultiphotonBreitWheelerTables.initialization( params, &smpi );
    //MultiphotonBreitWheelerTables.computeTables( params, &smpi );
    //MultiphotonBreitWheelerTables.outputTables( &smpi );

    // reading from dumped file the restart values
    if( params.restart ) {
        // smpi.patch_count recomputed in readPatchDistribution
        checkpoint.readPatchDistribution( &smpi, simWindow );
        // allocate patches according to smpi.patch_count
        PatchesFactory::createVector( vecPatches, params, &smpi, openPMD, checkpoint.this_run_start_step+1, simWindow->getNmoved() );
        // vecPatches data read in restartAll according to smpi.patch_count
        checkpoint.restartAll( vecPatches, &smpi, simWindow, params, openPMD );
        vecPatches.sort_all_particles( params );

        // Patch reconfiguration for the adaptive vectorization
        if( params.has_adaptive_vectorization ) {
            vecPatches.configuration( params, timers, 0 );
        }

        // time at integer time-steps (primal grid)
        time_prim = checkpoint.this_run_start_step * params.timestep;
        // time at half-integer time-steps (dual grid)
        time_dual = ( checkpoint.this_run_start_step +0.5 ) * params.timestep;

        TITLE( "Initializing diagnostics" );
        vecPatches.initAllDiags( params, &smpi );

    } else {

        PatchesFactory::createVector( vecPatches, params, &smpi, openPMD, 0 );
        vecPatches.sort_all_particles( params );
        //MESSAGE ("create vector");
        // Initialize the electromagnetic fields
        // -------------------------------------

        TITLE( "Applying external fields at time t = 0" );
        vecPatches.applyExternalFields();
        vecPatches.saveExternalFields( params );

        // Solve "Relativistic Poisson" problem (including proper centering of fields)
        // Note: the mean gamma for initialization will be computed for all the species
        // whose fields are initialized at this iteration
        if( params.solve_relativistic_poisson == true ) {
            vecPatches.runRelativisticModule( time_prim, params, &smpi,  timers );
        }

        vecPatches.computeCharge();
        vecPatches.sumDensities( params, time_dual, timers, 0, simWindow, &smpi );
        
        // Apply antennas
        // --------------
        vecPatches.applyAntennas( 0.5 * params.timestep );
        // Init electric field (Ex/1D, + Ey/2D)
        if( params.solve_poisson == true && !vecPatches.isRhoNull( &smpi ) ) {
            TITLE( "Solving Poisson at time t = 0" );
            vecPatches.solvePoisson( params, &smpi );
        }

        // Patch reconfiguration
        if( params.has_adaptive_vectorization ) {
            vecPatches.configuration( params, timers, 0 );
        }

        // if Laser Envelope is used, execute particles and envelope sections of ponderomotive loop
        if( params.Laser_Envelope_model ) {
            // initialize new envelope from scratch, following the input namelist
            vecPatches.init_new_envelope( params );
        } // end condition if Laser Envelope Model is used

        // Project charge and current densities (and susceptibility if envelope is used) only for diags at t=0
        vecPatches.projection_for_diags( params, &smpi, simWindow, time_dual, timers, 0 );

        // If Laser Envelope is used, comm and synch susceptibility at t=0
        if( params.Laser_Envelope_model ) {
            // comm and synch susceptibility
            vecPatches.sumSusceptibility( params, time_dual, timers, 0, simWindow, &smpi );
        } // end condition if Laser Envelope Model is used

        // Comm and synch charge and current densities
        vecPatches.sumDensities( params, time_dual, timers, 0, simWindow, &smpi );

        TITLE( "Initializing diagnostics" );
        vecPatches.initAllDiags( params, &smpi );
        TITLE( "Running diags at time t = 0" );
        vecPatches.runAllDiags( params, &smpi, 0, timers, simWindow );
    }

    TITLE( "Species creation summary" );
    vecPatches.printNumberOfParticles( &smpi );

    timers.reboot();


    Domain domain( params );
    unsigned int global_factor( 1 );
#ifdef _PICSAR
    for( unsigned int iDim = 0 ; iDim < params.nDim_field ; iDim++ ) {
        global_factor *= params.global_factor[iDim];
    }
    // Force temporary usage of double grids, even if global_factor = 1
    //    especially to compare solvers
    //if (global_factor!=1) {
    domain.build( params, &smpi, vecPatches, openPMD );
    //}
#endif

    timers.global.reboot();

    // ------------------------------------------------------------------------
    // Check memory consumption & expected disk usage
    // ------------------------------------------------------------------------
    TITLE( "Memory consumption" );
    vecPatches.check_memory_consumption( &smpi );

    TITLE( "Expected disk usage (approximate)" );
    vecPatches.check_expected_disk_usage( &smpi, params, checkpoint );

    // ------------------------------------------------------------------------
    // check here if we can close the python interpreter
    // ------------------------------------------------------------------------
    TITLE( "Cleaning up python runtime environement" );
    params.cleanup( &smpi );

    /*tommaso
        // save latestTimeStep (used to test if we are at the latest timestep when running diagnostics at run's end)
        unsigned int latestTimeStep=checkpoint.this_run_start_step;
    */
    // ------------------------------------------------------------------
    //                     HERE STARTS THE PIC LOOP
    // ------------------------------------------------------------------

    TITLE( "Time-Loop started: number of time-steps n_time = " << params.n_time );
    if( smpi.isMaster() ) {
        params.print_timestep_headers();
    }

    #pragma omp parallel shared (time_dual,smpi,params, vecPatches, domain, simWindow, checkpoint)
    {

        unsigned int itime=checkpoint.this_run_start_step+1;
        while( ( itime <= params.n_time ) && ( !checkpoint.exit_asap ) ) {

            // calculate new times
            // -------------------
            #pragma omp single
            {
                time_prim += params.timestep;
                time_dual += params.timestep;
            }

            // Patch reconfiguration
            if( params.has_adaptive_vectorization && params.adaptive_vecto_time_selection->theTimeIsNow( itime ) ) {
                vecPatches.reconfiguration( params, timers, itime );
            }

            // apply collisions if requested
            vecPatches.applyCollisions( params, itime, timers );

            // Solve "Relativistic Poisson" problem (including proper centering of fields)
            // for species who stop to be frozen
            // Note: the mean gamma for initialization will be computed for all the species
            // whose fields are initialized at this iteration
            if( params.solve_relativistic_poisson == true ) {
                vecPatches.runRelativisticModule( time_prim, params, &smpi,  timers );
            }

            // (1) interpolate the fields at the particle position
            // (2) move the particle
            // (3) calculate the currents (charge conserving method)
            vecPatches.dynamics( params, &smpi, simWindow, RadiationTables,
                                 MultiphotonBreitWheelerTables,
                                 time_dual, timers, itime );

            // if Laser Envelope is used, execute particles and envelope sections of ponderomotive loop
            if( params.Laser_Envelope_model ) {
                vecPatches.runEnvelopeModule( params, &smpi, simWindow, time_dual, timers, itime );
            } // end condition if Laser Envelope Model is used

            // Sum densities
            vecPatches.sumDensities( params, time_dual, timers, itime, simWindow, &smpi );

            // apply currents from antennas
            vecPatches.applyAntennas( time_dual );

            // solve Maxwell's equations
#ifndef _PICSAR
            // Force temporary usage of double grids, even if global_factor = 1
            //    especially to compare solvers
            //if ( global_factor==1 )
            {
                if( time_dual > params.time_fields_frozen ) {
                    vecPatches.solveMaxwell( params, simWindow, itime, time_dual, timers, &smpi );
                }
            }
#else
            // Force temporary usage of double grids, even if global_factor = 1
            //    especially to compare solvers
            //if ( global_factor!=1 )
            {
                if( time_dual > params.time_fields_frozen ) {
                    SyncCartesianPatch::patchedToCartesian( vecPatches, domain, params, &smpi, timers, itime );
                    domain.solveMaxwell( params, simWindow, itime, time_dual, timers, &smpi );
                    SyncCartesianPatch::cartesianToPatches( domain, vecPatches, params, &smpi, timers, itime );
                }
            }
#endif

            // finalize particle exchanges and sort particles
            vecPatches.finalizeAndSortParticles( params, &smpi, simWindow,
                                                time_dual, timers, itime );

            // Particle merging
            vecPatches.mergeParticles(params, &smpi, time_dual,timers, itime );

            // Clean buffers and resize arrays
            vecPatches.cleanParticlesOverhead(params, timers, itime );

            // Finalize field synchronization and exchanges
            vecPatches.finalize_sync_and_bc_fields( params, &smpi, simWindow, time_dual, timers, itime );
            
            // call the various diagnostics
            vecPatches.runAllDiags( params, &smpi, itime, timers, simWindow );

            // Particle injection from the boundaries
            vecPatches.injectParticlesFromBoundaries(params, timers, itime );

            // if (0) {
            //     #pragma omp master
            //     {
            //         vector<unsigned int> init_space( 3, 1 );
            //         init_space[0] = 1;
            //         init_space[1] = params.n_space[1];
            //         init_space[2] = params.n_space[2];
            //         for (int iPatch=0 ; iPatch<vecPatches.size() ; iPatch++) {
            //
            //             // Create particles as if t0
            //             vector<int>  nparts_per_species(vecPatches(0)->vecSpecies.size(),0);
            //             for (int iSpecies=0 ; iSpecies<vecPatches(0)->vecSpecies.size() ; iSpecies++) {
            //                 nparts_per_species[iSpecies] = vecPatches(iPatch)->vecSpecies[iSpecies]->getNbrOfParticles();
            //                 if ( ( (vecPatches(iPatch)->isXmin()) && (iSpecies<2) ) ||
            //                      ( (vecPatches(iPatch)->isXmax()) && (iSpecies>1) ) ) {
            //                     int icell;
            //                     if (vecPatches(iPatch)->isXmin())
            //                         icell = 0;
            //                     if (vecPatches(iPatch)->isXmax())
            //                         icell = params.n_space[0]-1;
            //                     vecPatches(iPatch)->vecSpecies[iSpecies]->createParticles( init_space, params, vecPatches(iPatch), icell );
            //                 }
            //             }

                        // // Filter particles
                        // // same position
                        // for (int iSpecies=0 ; iSpecies<vecPatches(0)->vecSpecies.size() ; iSpecies=iSpecies+2) {
                        //
                        //     int new_nParts = vecPatches(iPatch)->vecSpecies[iSpecies]->getNbrOfParticles();
                        //     int new_nPart2 = vecPatches(iPatch)->vecSpecies[iSpecies+1]->getNbrOfParticles();
                        //     Particles* particles = vecPatches(iPatch)->vecSpecies[iSpecies]->particles;
                        //     Particles* particle2 = vecPatches(iPatch)->vecSpecies[iSpecies+1]->particles;
                        //
                        //     // Suppr not interesting parts ...
                        //     for ( int ip = new_nParts-1 ; ip >= nparts_per_species[iSpecies] ; ip-- ){
                        //         int ip2 = nparts_per_species[iSpecies+1]+(ip-nparts_per_species[iSpecies]);
                        //         if ( vecPatches(iPatch)->isXmin() ) {
                        //             particles->Position[0][ip] += ( params.timestep*particles->Momentum[0][ip]*particles->inv_lor_fac(ip)-params.cell_length[0] );
                        //             particle2->Position[0][ip2] = particles->Position[0][ip];
                        //             //particles->Position[0][ip] += ( params.timestep*0.4-params.cell_length[0] );
                        //             if ( ( particles->Position[0][ip] < 0. ) ) {
                        //                 particles->erase_particle(ip);
                        //                 particle2->erase_particle(ip2);
                        //                 new_nParts--;
                        //             }
                        //         }
                        //         else if ( vecPatches(iPatch)->isXmax() ) {
                        //             particles->Position[0][ip] += ( params.timestep*particles->Momentum[0][ip]*particles->inv_lor_fac(ip)+params.cell_length[0] );
                        //             particle2->Position[0][ip2] = particles->Position[0][ip];
                        //             //particles->Position[0][ip] += ( -params.timestep*0.4+params.cell_length[0] );
                        //             if (  particles->Position[0][ip] > params.grid_length[0] ) {
                        //                 particles->erase_particle(ip);
                        //                 particle2->erase_particle(ip2);
                        //                 new_nParts--;
                        //             }
                        //         }
                        //     }
                        //
                        //     // Move interesting parts to their place
                        //     for ( int ip = nparts_per_species[iSpecies] ; ip < new_nParts ; ip++ ){
                        //         int ip2 = nparts_per_species[iSpecies+1]+(ip-nparts_per_species[iSpecies]);
                        //         if ( vecPatches(iPatch)->isXmin() ) {
                        //             if ( ( particles->Position[0][ip] >= 0. ) ) {
                        //                 int new_cell_idx=0;
                        //                 particles->mv_particles(ip,vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[idx]++;
                        //                 }
                        //                 particle2->mv_particles(ip2,vecPatches(iPatch)->vecSpecies[iSpecies+1]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies+1]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index[idx]++;
                        //                 }
                        //             }
                        //         }
                        //         else if ( vecPatches(iPatch)->isXmax() ) {
                        //             if (  particles->Position[0][ip] <= params.grid_length[0] ) {
                        //                 int new_cell_idx=params.n_space[0]-1;
                        //                 particles->mv_particles(ip,vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[idx]++;
                        //                 }
                        //                 particle2->mv_particles(ip2,vecPatches(iPatch)->vecSpecies[iSpecies+1]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies+1]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies+1]->last_index[idx]++;
                        //                 }
                        //             }
                        //         }
                        //
                        //     }
                        // }
                        
                        // // Filter
                        // // Different position
                        // for (int iSpecies=0 ; iSpecies<vecPatches(0)->vecSpecies.size() ; iSpecies++) {
                        //
                        //     int new_nParts = vecPatches(iPatch)->vecSpecies[iSpecies]->getNbrOfParticles();
                        //     Particles* particles = vecPatches(iPatch)->vecSpecies[iSpecies]->particles;
                        //
                        //     // Suppr not interesting parts ...
                        //     for ( int ip = new_nParts-1 ; ip >= nparts_per_species[iSpecies] ; ip-- ){
                        //         if ( vecPatches(iPatch)->isXmin() ) {
                        //             particles->Position[0][ip] += ( params.timestep*particles->Momentum[0][ip]*particles->inv_lor_fac(ip)-params.cell_length[0] );
                        //             //particles->Position[0][ip] += ( params.timestep*0.4-params.cell_length[0] );
                        //             if ( ( particles->Position[0][ip] < 0. ) ) {
                        //                 particles->erase_particle(ip);
                        //                 new_nParts--;
                        //             }
                        //         }
                        //         else if ( vecPatches(iPatch)->isXmax() ) {
                        //             particles->Position[0][ip] += ( params.timestep*particles->Momentum[0][ip]*particles->inv_lor_fac(ip)+params.cell_length[0] );
                        //             //particles->Position[0][ip] += ( -params.timestep*0.4+params.cell_length[0] );
                        //             if (  particles->Position[0][ip] > params.grid_length[0] ) {
                        //                 particles->erase_particle(ip);
                        //                 new_nParts--;
                        //             }
                        //         }
                        //     }

                        //     // Move interesting parts to their place
                        //     for ( int ip = nparts_per_species[iSpecies] ; ip < new_nParts ; ip++ ){
                        //         if ( vecPatches(iPatch)->isXmin() ) {
                        //             if ( ( particles->Position[0][ip] >= 0. ) ) {
                        //                 int new_cell_idx=0;
                        //                 particles->mv_particles(ip,vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[idx]++;
                        //                 }
                        //             }
                        //         }
                        //         else if ( vecPatches(iPatch)->isXmax() ) {
                        //             if (  particles->Position[0][ip] <= params.grid_length[0] ) {
                        //                 int new_cell_idx=params.n_space[0]-1;
                        //                 particles->mv_particles(ip,vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[(new_cell_idx)/params.clrw]);
                        //                 vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[(new_cell_idx)/params.clrw]++;
                        //                 for ( int idx=(new_cell_idx)/params.clrw+1 ; idx<vecPatches(iPatch)->vecSpecies[iSpecies]->last_index.size() ; idx++ ) {
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->first_index[idx]++;
                        //                     vecPatches(iPatch)->vecSpecies[iSpecies]->last_index[idx]++;
                        //                 }
                        //             }
                        //         }
                        //
                        //     }
                        // }
            //         }
            //     }
            // }
            // #pragma omp barrier
            
            timers.movWindow.restart();
            simWindow->operate( vecPatches, &smpi, params, itime, time_dual );
            timers.movWindow.update();
            // ----------------------------------------------------------------------
            // Validate restart  : to do
            // Restart patched moving window : to do
            #pragma omp master
            checkpoint.dump( vecPatches, itime, &smpi, simWindow, params );
            #pragma omp barrier
            // ----------------------------------------------------------------------


            if( params.has_load_balancing ) {
                if( params.load_balancing_time_selection->theTimeIsNow( itime ) ) {
                    timers.loadBal.restart();
                    #pragma omp single
                    vecPatches.load_balance( params, time_dual, &smpi, simWindow, itime );
                    timers.loadBal.update( params.printNow( itime ) );
                }
            }

            // print message at given time-steps
            // --------------------------------
            if( smpi.isMaster() &&  params.printNow( itime ) ) {
                params.print_timestep( itime, time_dual, timers.global );    //contain a timer.update !!!
            }

            if( params.printNow( itime ) ) {
                #pragma omp master
                timers.consolidate( &smpi );
                #pragma omp barrier
            }

            itime++;

        }//END of the time loop

    } //End omp parallel region

    smpi.barrier();

    // ------------------------------------------------------------------
    //                      HERE ENDS THE PIC LOOP
    // ------------------------------------------------------------------
    TITLE( "End time loop, time dual = " << time_dual );
    timers.global.update();

    TITLE( "Time profiling : (print time > 0.001%)" );
    timers.profile( &smpi );

    smpi.barrier();

    /*tommaso
        // ------------------------------------------------------------------
        //                      Temporary validation diagnostics
        // ------------------------------------------------------------------

        if (latestTimeStep==params.n_time)
            vecPatches.runAllDiags(params, smpi, &diag_flag, params.n_time, timer, simWindow);
    */

    // ------------------------------
    //  Cleanup & End the simulation
    // ------------------------------
    if( global_factor!=1 ) {
        domain.clean();
    }
    vecPatches.close( &smpi );
    smpi.barrier(); // Don't know why but sync needed by HDF5 Phasespace managment
    delete simWindow;
    PyTools::closePython();
    TITLE( "END" );

    return 0;

}//END MAIN

// ---------------------------------------------------------------------------------------------------------------------
//                                               END MAIN CODE
// ---------------------------------------------------------------------------------------------------------------------


int execute_test_mode( VectorPatch &vecPatches, SmileiMPI *smpi, SimWindow *simWindow, Params &params, Checkpoint &checkpoint, OpenPMDparams &openPMD )
{
    int itime = 0;
    int moving_window_movement = 0;

    if( params.restart ) {
        checkpoint.readPatchDistribution( smpi, simWindow );
        itime = checkpoint.this_run_start_step+1;
        moving_window_movement = simWindow->getNmoved();
    }

    PatchesFactory::createVector( vecPatches, params, smpi, openPMD, itime, moving_window_movement );

    if( params.restart ) {
        checkpoint.restartAll( vecPatches, smpi, simWindow, params, openPMD );
    }

    if( params.print_expected_disk_usage ) {
        TITLE( "Expected disk usage (approximate)" );
        vecPatches.check_expected_disk_usage( smpi, params, checkpoint );
    }

    // If test mode enable, code stops here
    TITLE( "Cleaning up python runtime environement" );
    params.cleanup( smpi );
    delete simWindow;
    PyTools::closePython();
    TITLE( "END TEST MODE" );

    return 0;
}
