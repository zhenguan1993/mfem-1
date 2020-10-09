// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "mesh_operators.hpp"
#include "pmesh.hpp"

namespace mfem
{

MeshOperatorSequence::~MeshOperatorSequence()
{
   // delete in reverse order
   for (int i = sequence.Size()-1; i >= 0; i--)
   {
      delete sequence[i];
   }
}

int MeshOperatorSequence::ApplyImpl(Mesh &mesh)
{
   if (sequence.Size() == 0) { return NONE; }
next_step:
   step = (step + 1) % sequence.Size();
   bool last = (step == sequence.Size() - 1);
   int mod = sequence[step]->ApplyImpl(mesh);
   switch (mod & MASK_ACTION)
   {
      case NONE:     if (last) { return NONE; } goto next_step;
      case CONTINUE: return last ? mod : (REPEAT | (mod & MASK_INFO));
      case STOP:     return STOP;
      case REPEAT:    --step; return mod;
   }
   return NONE;
}

void MeshOperatorSequence::Reset()
{
   for (int i = 0; i < sequence.Size(); i++)
   {
      sequence[i]->Reset();
   }
   step = 0;
}


ThresholdRefiner::ThresholdRefiner(ErrorEstimator &est)
   : estimator(est)
{
   aniso_estimator = dynamic_cast<AnisotropicErrorEstimator*>(&estimator);
   total_norm_p = infinity();
   total_err_goal = 0.0;
   total_fraction = 0.5;
   local_err_goal = 0.0;
   max_elements = std::numeric_limits<long>::max();
   amr_levels=max_elements;
   xRange_levels=max_elements; //if xRange_levels is trun on, it will ignore xRang if levels<xRange_levels
   xRange=false;
   yRange_levels=max_elements;
   yRange=false;
   xmax=std::numeric_limits<double>::max();
   ymax=xmax;
   xmin=std::numeric_limits<double>::lowest();
   ymin=xmin;

   threshold = 0.0;
   num_marked_elements = 0L;
   current_sequence = -1;

   non_conforming = -1;
   nc_limit = 0;
}

double ThresholdRefiner::GetNorm(const Vector &local_err, Mesh &mesh) const
{
#ifdef MFEM_USE_MPI
   ParMesh *pmesh = dynamic_cast<ParMesh*>(&mesh);
   if (pmesh)
   {
      return ParNormlp(local_err, total_norm_p, pmesh->GetComm());
   }
#endif
   return local_err.Normlp(total_norm_p);
}

int ThresholdRefiner::ApplyImpl(Mesh &mesh)
{
   threshold = 0.0;
   num_marked_elements = 0;
   marked_elements.SetSize(0);
   current_sequence = mesh.GetSequence();

   const long num_elements = mesh.GetGlobalNE();
   if (num_elements >= max_elements) { return STOP; }

   const int NE = mesh.GetNE();
   const Vector &local_err = estimator.GetLocalErrors();
   MFEM_ASSERT(local_err.Size() == NE, "invalid size of local_err");

   double vert[3];
   double yMean, xMean;
   long elementLevel;

   /*
   // the local err is set to be 0 if it is in the x range and the elementLevel reaches the 
   // maximal levels that xRange allows (so refinement will skip that)
   if (xRange && mesh.Nonconforming())
   {
      Vector &local_err_ = const_cast<Vector &>(local_err);
      FiniteElementSpace * fes = mesh.GetNodes()->FESpace();
      Array<int> dofs;
      for (int el = 0; el < NE; el++)
      {
        fes->GetElementDofs(el, dofs);
        int ndof=dofs.Size();
        xMean=0.0;
        elementLevel=mesh.ncmesh->GetElementDepth(el);
        for (int j = 0; j < ndof; j++)
        {
           mesh.GetNode(dofs[j], vert);
           xMean+=vert[0];
        }
        xMean=xMean/ndof;
        
        if ((xMean<=xmin || xMean>=xmax) && elementLevel==xRange_levels)
           local_err_(el) =0.;
      }
   }
   */

   const double total_err = GetNorm(local_err, mesh);
   if (total_err <= total_err_goal) { return STOP; }

   if (total_norm_p < infinity())
   {
      threshold = std::max(total_err * total_fraction *
                           std::pow(num_elements, -1.0/total_norm_p),
                           local_err_goal);
   }
   else
   {
      threshold = std::max(total_err * total_fraction, local_err_goal);
   }


   for (int el = 0; el < NE; el++)
   { 
      if ((yRange || xRange) && mesh.Nonconforming())
      {
        FiniteElementSpace * fes = mesh.GetNodes()->FESpace();
        Array<int> dofs;
        fes->GetElementDofs(el, dofs);
        int ndof=dofs.Size();
        yMean=0.0;
        xMean=0.0;
        for (int j = 0; j < ndof; j++)
        {
           mesh.GetNode(dofs[j], vert);
           yMean+=vert[1];
           xMean+=vert[0];
        }
        yMean=yMean/ndof;
        xMean=xMean/ndof;

        elementLevel=mesh.ncmesh->GetElementDepth(el);
        
        if (local_err(el) > threshold && elementLevel < amr_levels &&
            mesh.ncmesh->GetElementDepth(el) < amr_levels && 
            ((yMean>ymin && yMean<ymax) || elementLevel<yRange_levels) &&
            ((xMean>xmin && xMean<xmax) || elementLevel<xRange_levels)
            )
        {
           marked_elements.Append(Refinement(el));
        }

      }
      else if (mesh.Nonconforming())
      {
        //std::cout <<"el="<<el<<" level="<<mesh.ncmesh->GetElementDepth(el)<< '\n';
        if (local_err(el) > threshold && 
            mesh.ncmesh->GetElementDepth(el) < amr_levels)
        {
           marked_elements.Append(Refinement(el));
        }
      }
      else
      {
        if (local_err(el) > threshold )
        {
           marked_elements.Append(Refinement(el));
        }
      }
   }

   if (aniso_estimator)
   {
      const Array<int> &aniso_flags = aniso_estimator->GetAnisotropicFlags();
      if (aniso_flags.Size() > 0)
      {
         for (int i = 0; i < marked_elements.Size(); i++)
         {
            Refinement &ref = marked_elements[i];
            ref.ref_type = aniso_flags[ref.index];
         }
      }
   }

   num_marked_elements = mesh.ReduceInt(marked_elements.Size());
   if (num_marked_elements == 0) { return STOP; }

   mesh.GeneralRefinement(marked_elements, non_conforming, nc_limit);
   return CONTINUE + REFINED;
}

void ThresholdRefiner::Reset()
{
   estimator.Reset();
   current_sequence = -1;
   num_marked_elements = 0;
   // marked_elements.SetSize(0); // not necessary
}

double ThresholdDerefiner::GetNorm(const Vector &local_err, Mesh &mesh) const
{
#ifdef MFEM_USE_MPI

   ParMesh *pmesh = dynamic_cast<ParMesh*>(&mesh);
   if (pmesh)
   {
      return ParNormlp(local_err, total_norm_p, pmesh->GetComm());
   }
#endif
   return local_err.Normlp(total_norm_p);
}

int ThresholdDerefiner::ApplyImpl(Mesh &mesh)
{
   if (mesh.Conforming()) { return NONE; }

   const Vector &local_err = estimator.GetLocalErrors();

   const double total_err = GetNorm(local_err, mesh);
   const double true_threshold = std::max(total_err * total_fraction, threshold);

   bool derefs = mesh.DerefineByError(local_err, true_threshold, nc_limit, op);

   return derefs ? CONTINUE + DEREFINED : NONE;
}


int Rebalancer::ApplyImpl(Mesh &mesh)
{
#ifdef MFEM_USE_MPI
   ParMesh *pmesh = dynamic_cast<ParMesh*>(&mesh);
   if (pmesh && pmesh->Nonconforming())
   {
      pmesh->Rebalance();
      return CONTINUE + REBALANCED;
   }
#endif
   return NONE;
}


} // namespace mfem
