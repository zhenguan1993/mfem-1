#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
   const char *mesh_file = "../data/star.mesh";
   int ref_levels = 0;
   int order = 3;
   bool simplex = true;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh", "Mesh file to use.");
   args.AddOption(&ref_levels, "-r", "--refine", "Uniform refinements.");
   args.AddOption(&order, "-o", "--order", "Polynomial degree.");
   args.AddOption(&simplex, "-s", "--simplex", "-no-s", "--no-simplex",
                  "Simplex LOR?");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   Mesh mesh(mesh_file, 1, 1);
   int dim = mesh.Dimension();

   mesh.EnsureNodes();

   for (int l = 0; l < ref_levels; l++)
   {
      mesh.UniformRefinement();
   }

   H1_FECollection fec_ho(order, dim);
   FiniteElementSpace fes_ho(&mesh, &fec_ho);

   Mesh mesh_lor_tensor(&mesh, order, BasisType::GaussLobatto);

   Mesh mesh_lor;
   mesh_lor.MakeSimplicial(mesh_lor_tensor);
   mesh_lor.PrintBdrVTU("bdr_lor");
   mesh_lor.PrintVTU("mesh_lor");

   H1_FECollection fec_lor(1, dim);
   FiniteElementSpace fes_lor(&mesh_lor, &fec_lor);

   Array<int> ess_tdof_list;
   if (mesh.bdr_attributes.Size())
   {
      Array<int> ess_bdr(mesh.bdr_attributes.Max());
      ess_bdr = 1;
      fes_ho.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   }

   ConstantCoefficient one(1.0);

   BilinearForm a_ho(&fes_ho);
   a_ho.AddDomainIntegrator(new DiffusionIntegrator(one));
   a_ho.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   a_ho.Assemble();
   LinearForm b_ho(&fes_ho);
   b_ho.AddDomainIntegrator(new DomainLFIntegrator(one));
   b_ho.Assemble();

   BilinearForm a_lor(&fes_lor);
   a_lor.AddDomainIntegrator(new DiffusionIntegrator(one));
   a_lor.Assemble();
   LinearForm b_lor(&fes_lor);
   b_lor.AddDomainIntegrator(new DomainLFIntegrator(one));
   b_lor.Assemble();

   GridFunction x_ho(&fes_ho), x_lor(&fes_lor);
   x_ho = 0.0;
   x_lor = 0.0;

   Vector X_ho, B_ho, X_lor, B_lor;
   OperatorHandle A_ho, A_lor;
   a_ho.FormLinearSystem(ess_tdof_list, x_ho, b_ho, A_ho, X_ho, B_ho);
   a_lor.FormLinearSystem(ess_tdof_list, x_lor, b_lor, A_lor, X_lor, B_lor);

   UMFPackSolver solv_lor;
   solv_lor.SetOperator(*A_lor);
   CGSolver cg;
   cg.SetAbsTol(0.0);
   cg.SetRelTol(1e-12);
   cg.SetMaxIter(100);
   cg.SetPrintLevel(1);
   cg.SetOperator(*A_ho);
   cg.SetPreconditioner(solv_lor);
   cg.Mult(B_ho, X_ho);
   a_ho.RecoverFEMSolution(X_ho, b_ho, x_ho);

   ParaViewDataCollection dc("LOR", &mesh);
   dc.SetPrefixPath("ParaView");
   dc.SetHighOrderOutput(true);
   dc.SetLevelsOfDetail(order);
   dc.RegisterField("u", &x_ho);
   dc.SetCycle(0);
   dc.SetTime(0.0);
   dc.Save();

   solv_lor.Mult(B_lor, X_lor);
   a_lor.RecoverFEMSolution(X_lor, b_lor, x_lor);
   dc.SetMesh(&mesh_lor);
   dc.DeregisterField("u");
   dc.RegisterField("u", &x_lor);
   dc.SetLevelsOfDetail(1);
   dc.SetCycle(1);
   dc.SetTime(1.0);
   dc.Save();

   return 0;
}