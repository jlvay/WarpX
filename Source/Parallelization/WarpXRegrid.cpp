/* Copyright 2019 Andrew Myers, Ann Almgren, Axel Huebl
 * David Grote, Maxence Thevenet, Michael Rowan
 * Remi Lehe, Weiqun Zhang, levinem, Revathi Jambunathan
 *
 * This file is part of WarpX.
 *
 * License: BSD-3-Clause-LBNL
 */
#include "WarpX.H"

#include "Diagnostics/MultiDiagnostics.H"
#include "Diagnostics/ReducedDiags/MultiReducedDiags.H"
#include "EmbeddedBoundary/Enabled.H"
#include "EmbeddedBoundary/WarpXFaceInfoBox.H"
#include "FieldSolver/FiniteDifferenceSolver/HybridPICModel/HybridPICModel.H"
#include "Initialization/ExternalField.H"
#include "Particles/MultiParticleContainer.H"
#include "Particles/ParticleBoundaryBuffer.H"
#include "Particles/WarpXParticleContainer.H"
#include "Utils/TextMsg.H"
#include "Utils/WarpXAlgorithmSelection.H"
#include "Utils/WarpXProfilerWrapper.H"

#include <AMReX.H>
#include <AMReX_BLassert.H>
#include <AMReX_Box.H>
#include <AMReX_BoxArray.H>
#include <AMReX_Config.H>
#include <AMReX_DistributionMapping.H>
#include <AMReX_FabFactory.H>
#include <AMReX_IArrayBox.H>
#include <AMReX_IndexType.H>
#include <AMReX_LayoutData.H>
#include <AMReX_MFIter.H>
#include <AMReX_MakeType.H>
#include <AMReX_MultiFab.H>
#include <AMReX_ParIter.H>
#include <AMReX_ParallelContext.H>
#include <AMReX_ParallelDescriptor.H>
#include <AMReX_REAL.H>
#include <AMReX_Vector.H>
#include <AMReX_iMultiFab.H>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

using namespace amrex;

void
WarpX::CheckLoadBalance (int step)
{
    if (step > 0 && load_balance_intervals.contains(step+1))
    {
        LoadBalance();

        // Reset the costs to 0
        ResetCosts();
    }
    if (!costs.empty())
    {
        RescaleCosts(step);
    }
}

void
WarpX::LoadBalance ()
{
    WARPX_PROFILE_REGION("LoadBalance");
    WARPX_PROFILE("WarpX::LoadBalance()");

    AMREX_ALWAYS_ASSERT(!costs.empty());
    AMREX_ALWAYS_ASSERT(costs[0] != nullptr);

#ifdef AMREX_USE_MPI
    if (load_balance_costs_update_algo == LoadBalanceCostsUpdateAlgo::Heuristic)
    {
        // compute the costs on a per-rank basis
        ComputeCostsHeuristic(costs);
    }

    // By default, do not do a redistribute; this toggles to true if RemakeLevel
    // is called for any level
    int loadBalancedAnyLevel = false;

    const int nLevels = finestLevel();
    for (int lev = 0; lev <= nLevels; ++lev)
    {
        int doLoadBalance = false;

        // Compute the new distribution mapping
        DistributionMapping newdm;
        const amrex::Real nboxes = costs[lev]->size();
        const amrex::Real nprocs = ParallelContext::NProcsSub();
        const int nmax = static_cast<int>(std::ceil(nboxes/nprocs*load_balance_knapsack_factor));
        // These store efficiency (meaning, the  average 'cost' over all ranks,
        // normalized to max cost) for current and proposed distribution mappings
        amrex::Real currentEfficiency = 0.0;
        amrex::Real proposedEfficiency = 0.0;

        newdm = (load_balance_with_sfc)
            ? DistributionMapping::makeSFC(*costs[lev],
                                           currentEfficiency, proposedEfficiency,
                                           false,
                                           ParallelDescriptor::IOProcessorNumber())
            : DistributionMapping::makeKnapSack(*costs[lev],
                                                currentEfficiency, proposedEfficiency,
                                                nmax,
                                                false,
                                                ParallelDescriptor::IOProcessorNumber());
        // As specified in the above calls to makeSFC and makeKnapSack, the new
        // distribution mapping is NOT communicated to all ranks; the loadbalanced
        // dm is up-to-date only on root, and we can decide whether to broadcast
        if ((load_balance_efficiency_ratio_threshold > 0.0)
            && (ParallelDescriptor::MyProc() == ParallelDescriptor::IOProcessorNumber()))
        {
            doLoadBalance = (proposedEfficiency > load_balance_efficiency_ratio_threshold*currentEfficiency);
        }

        ParallelDescriptor::Bcast(&doLoadBalance, 1,
                                  ParallelDescriptor::IOProcessorNumber());

        if (doLoadBalance)
        {
            Vector<int> pmap;
            if (ParallelDescriptor::MyProc() == ParallelDescriptor::IOProcessorNumber())
            {
                pmap = newdm.ProcessorMap();
            } else
            {
                pmap.resize(static_cast<std::size_t>(nboxes));
            }
            ParallelDescriptor::Bcast(pmap.data(), pmap.size(), ParallelDescriptor::IOProcessorNumber());

            if (ParallelDescriptor::MyProc() != ParallelDescriptor::IOProcessorNumber())
            {
                newdm = DistributionMapping(pmap);
            }

            RemakeLevel(lev, t_new[lev], boxArray(lev), newdm);

            // Record the load balance efficiency
            setLoadBalanceEfficiency(lev, proposedEfficiency);
        }

        loadBalancedAnyLevel = loadBalancedAnyLevel || doLoadBalance;
    }
    if (loadBalancedAnyLevel)
    {
        mypc->Redistribute();
        mypc->defineAllParticleTiles();

        // redistribute particle boundary buffer
        m_particle_boundary_buffer->redistribute();

        // diagnostics & reduced diagnostics
        // not yet needed:
        //multi_diags->LoadBalance();
        reduced_diags->LoadBalance();
    }
#endif
}

void
WarpX::RemakeLevel (int lev, Real /*time*/, const BoxArray& ba, const DistributionMapping& dm)
{

    const auto RemakeMultiFab = [&](auto& mf, const bool redistribute){
        if (mf == nullptr) { return; }
        const IntVect& ng = mf->nGrowVect();
        auto pmf = std::remove_reference_t<decltype(mf)>{};
        AllocInitMultiFab(pmf, mf->boxArray(), dm, mf->nComp(), ng, lev, mf->tags()[0]);
        if (redistribute) { pmf->Redistribute(*mf, 0, 0, mf->nComp(), ng); }
        mf = std::move(pmf);
    };

    bool const eb_enabled = EB::enabled();
    if (ba == boxArray(lev))
    {
        if (ParallelDescriptor::NProcs() == 1) { return; }

        // Fine patch
        for (int idim=0; idim < 3; ++idim)
        {
            RemakeMultiFab(Bfield_fp[lev][idim], true);
            RemakeMultiFab(Efield_fp[lev][idim], true);
            if (m_p_ext_field_params->B_ext_grid_type == ExternalFieldType::read_from_file) {
                RemakeMultiFab(Bfield_fp_external[lev][idim], true);
            }
            if (m_p_ext_field_params->E_ext_grid_type == ExternalFieldType::read_from_file) {
                RemakeMultiFab(Efield_fp_external[lev][idim], true);
            }
            if (mypc->m_B_ext_particle_s == "read_from_file") {
                RemakeMultiFab(B_external_particle_field[lev][idim], true);
            }
            if (mypc->m_E_ext_particle_s == "read_from_file") {
                RemakeMultiFab(E_external_particle_field[lev][idim], true);
            }
            RemakeMultiFab(current_fp[lev][idim], false);
            RemakeMultiFab(current_store[lev][idim], false);
            if (current_deposition_algo == CurrentDepositionAlgo::Vay) {
                RemakeMultiFab(current_fp_vay[lev][idim], false);
            }
            if (do_current_centering) {
                RemakeMultiFab(current_fp_nodal[lev][idim], false);
            }
            if (fft_do_time_averaging) {
                RemakeMultiFab(Efield_avg_fp[lev][idim], true);
                RemakeMultiFab(Bfield_avg_fp[lev][idim], true);
            }
            if (WarpX::electromagnetic_solver_id == ElectromagneticSolverAlgo::HybridPIC) {
                RemakeMultiFab(m_hybrid_pic_model->current_fp_temp[lev][idim], true);
                RemakeMultiFab(m_hybrid_pic_model->current_fp_ampere[lev][idim], false);
                RemakeMultiFab(m_hybrid_pic_model->current_fp_external[lev][idim],true);
            }
            if (eb_enabled) {
                if (WarpX::electromagnetic_solver_id != ElectromagneticSolverAlgo::PSATD) {
                    RemakeMultiFab(m_edge_lengths[lev][idim], false);
                    RemakeMultiFab(m_face_areas[lev][idim], false);
                    if (WarpX::electromagnetic_solver_id == ElectromagneticSolverAlgo::ECT) {
                        RemakeMultiFab(Venl[lev][idim], false);
                        RemakeMultiFab(m_flag_info_face[lev][idim], false);
                        RemakeMultiFab(m_flag_ext_face[lev][idim], false);
                        RemakeMultiFab(m_area_mod[lev][idim], false);
                        RemakeMultiFab(ECTRhofield[lev][idim], false);
                        m_borrowing[lev][idim] = std::make_unique<amrex::LayoutData<FaceInfoBox>>(amrex::convert(ba, Bfield_fp[lev][idim]->ixType().toIntVect()), dm);
                    }
                }
            }
        }

        RemakeMultiFab(F_fp[lev], true);
        RemakeMultiFab(rho_fp[lev], false);
        // phi_fp should be redistributed since we use the solution from
        // the last step as the initial guess for the next solve
        RemakeMultiFab(phi_fp[lev], true);

        if (WarpX::electromagnetic_solver_id == ElectromagneticSolverAlgo::HybridPIC) {
            RemakeMultiFab(m_hybrid_pic_model->rho_fp_temp[lev], true);
            RemakeMultiFab(m_hybrid_pic_model->electron_pressure_fp[lev], false);
        }

        if (eb_enabled) {
            RemakeMultiFab(m_distance_to_eb[lev], false);

#ifdef AMREX_USE_EB
            int const max_guard = guard_cells.ng_FieldSolver.max();
            m_field_factory[lev] = amrex::makeEBFabFactory(Geom(lev), ba, dm,
                                                           {max_guard, max_guard, max_guard},
                                                           amrex::EBSupport::full);
#endif
            InitializeEBGridData(lev);
        } else {
            m_field_factory[lev] = std::make_unique<FArrayBoxFactory>();
        }

#ifdef WARPX_USE_FFT
        if (electromagnetic_solver_id == ElectromagneticSolverAlgo::PSATD) {
            if (spectral_solver_fp[lev] != nullptr) {
                // Get the cell-centered box
                BoxArray realspace_ba = ba;   // Copy box
                realspace_ba.enclosedCells(); // Make it cell-centered
                auto ngEB = getngEB();
                auto dx = CellSize(lev);

#   ifdef WARPX_DIM_RZ
                if ( !fft_periodic_single_box ) {
                    realspace_ba.grow(1, ngEB[1]); // add guard cells only in z
                }
                AllocLevelSpectralSolverRZ(spectral_solver_fp,
                                           lev,
                                           realspace_ba,
                                           dm,
                                           dx);
#   else
                if ( !fft_periodic_single_box ) {
                    realspace_ba.grow(ngEB);   // add guard cells
                }
                bool const pml_flag_false = false;
                AllocLevelSpectralSolver(spectral_solver_fp,
                                         lev,
                                         realspace_ba,
                                         dm,
                                         dx,
                                         pml_flag_false);
#   endif
            }
        }
#endif

        // Aux patch
        if (lev == 0 && Bfield_aux[0][0]->ixType() == Bfield_fp[0][0]->ixType())
        {
            for (int idim = 0; idim < 3; ++idim) {
                Bfield_aux[lev][idim] = std::make_unique<MultiFab>(*Bfield_fp[lev][idim], amrex::make_alias, 0, Bfield_aux[lev][idim]->nComp());
                Efield_aux[lev][idim] = std::make_unique<MultiFab>(*Efield_fp[lev][idim], amrex::make_alias, 0, Efield_aux[lev][idim]->nComp());
            }
        } else {
            for (int idim=0; idim < 3; ++idim)
            {
                RemakeMultiFab(Bfield_aux[lev][idim], false);
                RemakeMultiFab(Efield_aux[lev][idim], false);
            }
        }

        // Coarse patch
        if (lev > 0) {
            for (int idim=0; idim < 3; ++idim)
            {
                RemakeMultiFab(Bfield_cp[lev][idim], true);
                RemakeMultiFab(Efield_cp[lev][idim], true);
                RemakeMultiFab(current_cp[lev][idim], false);
                if (fft_do_time_averaging) {
                    RemakeMultiFab(Efield_avg_cp[lev][idim], true);
                    RemakeMultiFab(Bfield_avg_cp[lev][idim], true);
                }
            }
            RemakeMultiFab(F_cp[lev], true);
            RemakeMultiFab(rho_cp[lev], false);

#ifdef WARPX_USE_FFT
            if (electromagnetic_solver_id == ElectromagneticSolverAlgo::PSATD) {
                if (spectral_solver_cp[lev] != nullptr) {
                    BoxArray cba = ba;
                    cba.coarsen(refRatio(lev-1));
                    const std::array<Real,3> cdx = CellSize(lev-1);

                    // Get the cell-centered box
                    BoxArray c_realspace_ba = cba;  // Copy box
                    c_realspace_ba.enclosedCells(); // Make it cell-centered

                    auto ngEB = getngEB();

#   ifdef WARPX_DIM_RZ
                    c_realspace_ba.grow(1, ngEB[1]); // add guard cells only in z
                    AllocLevelSpectralSolverRZ(spectral_solver_cp,
                                               lev,
                                               c_realspace_ba,
                                               dm,
                                               cdx);
#   else
                    c_realspace_ba.grow(ngEB);
                    bool const pml_flag_false = false;
                    AllocLevelSpectralSolver(spectral_solver_cp,
                                             lev,
                                             c_realspace_ba,
                                             dm,
                                             cdx,
                                             pml_flag_false);
#   endif
                }
            }
#endif
        }

        if (lev > 0 && (n_field_gather_buffer > 0 || n_current_deposition_buffer > 0)) {
            for (int idim=0; idim < 3; ++idim)
            {
                RemakeMultiFab(Bfield_cax[lev][idim], false);
                RemakeMultiFab(Efield_cax[lev][idim], false);
                RemakeMultiFab(current_buf[lev][idim], false);
            }
            RemakeMultiFab(charge_buf[lev], false);
            // we can avoid redistributing these since we immediately re-build the values via BuildBufferMasks()
            RemakeMultiFab(current_buffer_masks[lev], false);
            RemakeMultiFab(gather_buffer_masks[lev], false);

            if (current_buffer_masks[lev] || gather_buffer_masks[lev]) {
                BuildBufferMasks();
            }
        }

        // Re-initialize the lattice element finder with the new ba and dm.
        m_accelerator_lattice[lev]->InitElementFinder(lev, ba, dm);

        if (costs[lev] != nullptr)
        {
            costs[lev] = std::make_unique<LayoutData<Real>>(ba, dm);
            const auto iarr = costs[lev]->IndexArray();
            for (const auto& i : iarr)
            {
                (*costs[lev])[i] = 0.0;
                setLoadBalanceEfficiency(lev, -1);
            }
        }

        SetDistributionMap(lev, dm);

    } else
    {
        WARPX_ABORT_WITH_MESSAGE("RemakeLevel: to be implemented");
    }

    // Re-initialize diagnostic functors that stores pointers to the user-requested fields at level, lev.
    multi_diags->InitializeFieldFunctors( lev );

    // Reduced diagnostics
    // not needed yet
}

void
WarpX::ComputeCostsHeuristic (amrex::Vector<std::unique_ptr<amrex::LayoutData<amrex::Real> > >& a_costs)
{
    for (int lev = 0; lev <= finest_level; ++lev)
    {
        const auto & mypc_ref = GetInstance().GetPartContainer();
        const auto nSpecies = mypc_ref.nSpecies();

        // Species loop
        for (int i_s = 0; i_s < nSpecies; ++i_s)
        {
            auto & myspc = mypc_ref.GetParticleContainer(i_s);

            // Particle loop
            for (WarpXParIter pti(myspc, lev); pti.isValid(); ++pti)
            {
                (*a_costs[lev])[pti.index()] += costs_heuristic_particles_wt*pti.numParticles();
            }
        }

        // Cell loop
        MultiFab* Ex = Efield_fp[lev][0].get();
        for (MFIter mfi(*Ex, false); mfi.isValid(); ++mfi)
        {
            const Box& gbx = mfi.growntilebox();
            (*a_costs[lev])[mfi.index()] += costs_heuristic_cells_wt*gbx.numPts();
        }
    }
}

void
WarpX::ResetCosts ()
{
    AMREX_ALWAYS_ASSERT(!costs.empty());
    AMREX_ALWAYS_ASSERT(costs[0] != nullptr);

    for (int lev = 0; lev <= finest_level; ++lev)
    {
        const auto iarr = costs[lev]->IndexArray();
        for (const auto& i : iarr)
        {
            // Reset costs
            (*costs[lev])[i] = 0.0;
        }
    }
}

void
WarpX::RescaleCosts (int step)
{
    // rescale is only used for timers
    if (WarpX::load_balance_costs_update_algo != LoadBalanceCostsUpdateAlgo::Timers)
    {
        return;
    }

    AMREX_ALWAYS_ASSERT(costs.size() == finest_level + 1);

    for (int lev = 0; lev <= finest_level; ++lev)
    {
        if (costs[lev])
        {
            // Perform running average of the costs
            // (Giving more importance to most recent costs; only needed
            // for timers update, heuristic load balance considers the
            // instantaneous costs)
            for (const auto& i : costs[lev]->IndexArray())
            {
                (*costs[lev])[i] *= (1._rt - 2._rt/load_balance_intervals.localPeriod(step+1));
            }
        }
    }
}
