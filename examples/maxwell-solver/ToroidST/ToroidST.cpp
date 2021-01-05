
#include "ToroidST.hpp"


void ToroidST::SetupSubdomainProblems()
{
   // Sesquilinear forms and Operator
   sqf.SetSize(nrsubdomains);
   Optr.SetSize(nrsubdomains);
   // Subdomain Matrix and its LU factorization
   PmlMat.SetSize(nrsubdomains);
   PmlMatInv.SetSize(nrsubdomains);
   // Right hand sides
   f_orig.SetSize(nrsubdomains);
   f_transf.SetSize(nrsubdomains);

   for (int ip=0; ip<nrsubdomains; ip++)
   {
      SetMaxwellPmlSystemMatrix(ip);
      PmlMat[ip] = Optr[ip]->As<ComplexSparseMatrix>();
      PmlMatInv[ip] = new ComplexUMFPackSolver;
      PmlMatInv[ip]->Control[UMFPACK_ORDERING] = UMFPACK_ORDERING_METIS;
      PmlMatInv[ip]->SetOperator(*PmlMat[ip]);
      int ndofs = fespaces[ip]->GetTrueVSize();
      f_orig[ip] = new Vector(2*ndofs);
      f_transf[ip] = new Vector(2*ndofs);
   }
}

void ToroidST::SetMaxwellPmlSystemMatrix(int ip)
{
   Mesh * mesh = fespaces[ip]->GetMesh();
   MFEM_VERIFY(mesh, "Null mesh pointer");
   int dim = mesh->Dimension();
   ToroidPML tpml(mesh);
   Vector zlim, rlim, alim;
   tpml.GetDomainBdrs(zlim,rlim,alim);
   Vector zpml(2); zpml = 0.0;
   Vector rpml(2); rpml = 0.0;
   Vector apml(2); apml = 0.0; 
   bool zstretch = false;
   bool astretch = true;
   bool rstretch = false;
   if (ip == 0) 
   {
      apml[0] = aPmlThickness[0];
   }
   else if (ip == nrsubdomains-1)
   {
      apml[1] = aPmlThickness[1];
   }
   else
   {
      apml = aPmlThickness[1]; // just for this test (toroid waveguide)
   }
   tpml.SetPmlAxes(zstretch,rstretch,astretch);
   tpml.SetPmlWidth(zpml,rpml,apml);
   tpml.SetOmega(omega); 

   ComplexOperator::Convention conv = bf->GetConvention();
   tpml.SetAttributes(mesh);
   Array<int> ess_tdof_list;
   Array<int> ess_bdr;
   if (mesh->bdr_attributes.Size())
   {
      ess_bdr.SetSize(mesh->bdr_attributes.Max());
      ess_bdr = 1;
   }
   fespaces[ip]->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   Array<int> attr;
   Array<int> attrPML;
   if (mesh->attributes.Size())
   {
      attr.SetSize(mesh->attributes.Max());
      attrPML.SetSize(mesh->attributes.Max());
      attr = 0;   attr[0] = 1;
      attrPML = 0;
      if (mesh->attributes.Max() > 1)
      {
         attrPML[1] = 1;
      }
   }
   ConstantCoefficient one(1.0);
   ConstantCoefficient omeg(-pow(omega, 2));
   RestrictedCoefficient restr_one(one,attr);
   RestrictedCoefficient restr_omeg(omeg,attr);

     // Integrators inside the computational domain (excluding the PML region)
   sqf[ip] = new SesquilinearForm(fespaces[ip], conv);
   sqf[ip]->AddDomainIntegrator(new CurlCurlIntegrator(restr_one),NULL);
   sqf[ip]->AddDomainIntegrator(new VectorFEMassIntegrator(restr_omeg),NULL);

   PMLMatrixCoefficient pml_c1_Re(dim,detJ_inv_JT_J_Re, &tpml);
   PMLMatrixCoefficient pml_c1_Im(dim,detJ_inv_JT_J_Im, &tpml);
   ScalarMatrixProductCoefficient c1_Re(one,pml_c1_Re);
   ScalarMatrixProductCoefficient c1_Im(one,pml_c1_Im);
   MatrixRestrictedCoefficient restr_c1_Re(c1_Re,attrPML);
   MatrixRestrictedCoefficient restr_c1_Im(c1_Im,attrPML);

   PMLMatrixCoefficient pml_c2_Re(dim, detJ_JT_J_inv_Re,&tpml);
   PMLMatrixCoefficient pml_c2_Im(dim, detJ_JT_J_inv_Im,&tpml);
   ScalarMatrixProductCoefficient c2_Re(omeg,pml_c2_Re);
   ScalarMatrixProductCoefficient c2_Im(omeg,pml_c2_Im);
   MatrixRestrictedCoefficient restr_c2_Re(c2_Re,attrPML);
   MatrixRestrictedCoefficient restr_c2_Im(c2_Im,attrPML);

   // Integrators inside the PML region
   sqf[ip]->AddDomainIntegrator(new CurlCurlIntegrator(restr_c1_Re),
                        new CurlCurlIntegrator(restr_c1_Im));
   sqf[ip]->AddDomainIntegrator(new VectorFEMassIntegrator(restr_c2_Re),
                        new VectorFEMassIntegrator(restr_c2_Im));
   sqf[ip]->Assemble(0);

   Optr[ip] = new OperatorPtr;
   sqf[ip]->FormSystemMatrix(ess_tdof_list,*Optr[ip]);
}





ToroidST::ToroidST(SesquilinearForm * bf_, const Vector & aPmlThickness_, 
       double omega_, int nrsubdomains_)
: bf(bf_), aPmlThickness(aPmlThickness_), omega(omega_), nrsubdomains(nrsubdomains_)       
{
   fes = bf->FESpace();
   cout << "In ToroidST" << endl;

   overlap = 5.0;
   double ovlp = overlap + aPmlThickness[1]; // for now
   //-------------------------------------------------------
   // Step 0: Generate Mesh and FiniteElementSpace Partition
   // ------------------------------------------------------
   Array<Array<int> *> ElemMaps;
   PartitionFE(fes,nrsubdomains,ovlp,fespaces, ElemMaps, 
               DofMaps0, DofMaps1, OvlpMaps0, OvlpMaps1);
   for (int i = 0; i<nrsubdomains; i++) delete ElemMaps[i];

   //-------------------------------------------------------
   // Step 1: Setup local Maxwell Problems PML
   // ------------------------------------------------------
   SetupSubdomainProblems();


   // Test local to global dof Maps
   // cout << "Testing local to global maps " << endl;
   // for (int i = 0; i<nrsubdomains; i++)
   // {
   //    DofMapTests(*fespaces[i],*fes,*DofMaps0[i], *DofMaps1[i]);
   //    // DofMapTests(*fes,*fespaces[i], *DofMaps1[i], *DofMaps0[i]);
   // }

   // Test local to neighbor dof Maps
   // cout << "Testing local to neighbor maps " << endl;
   // for (int i = 0; i<nrsubdomains-1; i++)
   // {
   //    DofMapTests(*fespaces[i],*fespaces[i+1],*OvlpMaps0[i], *OvlpMaps1[i]);
   //    DofMapTests(*fespaces[i+1],*fespaces[i],*OvlpMaps1[i], *OvlpMaps0[i]);
   // }

   // Test local to overlap dof maps
   // cout << "Testing local to overlap maps " << endl;
   // for (int i = 0; i<nrsubdomains; i++)
   // {
   //    Array<int> rdofs;
   //    RestrictDofs(*fespaces[i],1,overlap,rdofs);
   //    RestrictDofs(*fespaces[i],-1,overlap,rdofs);
   //    DofMapOvlpTest(*fespaces[i],rdofs);
   // }
}


void ToroidST::Mult(const Vector & r, Vector & z) const 
{

}


ToroidST::~ToroidST()
{
   for (int i = 0; i<nrsubdomains-1; i++)
   {
      delete DofMaps0[i];
      delete DofMaps1[i];
      delete OvlpMaps0[i];
      delete OvlpMaps1[i];
   }
   delete DofMaps0[nrsubdomains-1];     
   delete DofMaps1[nrsubdomains-1];     
}