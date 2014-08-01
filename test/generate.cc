#include <PCU.h>
#include <MeshSim.h>
#include <SimPartitionedMesh.h>
#include <SimUtil.h>
#include <apfSIM.h>
#include <apfMDS.h>
#include <gmi_sim.h>
#include <apf.h>
#include <apfConvert.h>
#include <apfMesh2.h>

#ifndef AdvMeshing_EXPORT
#define AdvMeshing_EXPORT
#endif

#include "SimAttribute.h"
#include "ModelTypes.h"

/* The fields in this sturcture are set mainly based on the "Surface Meshing" and "Volume Meshing"
   attributes in the meshing case. */
struct AdvMeshing_EXPORT MeshingOptions {
  bool surfaceRun;                // whether to run surface meshing
  bool surfaceDoFixIntersections; // whether to run fix self intersections
  bool surfaceDoCurve;            // whether to curve surface mesh

  // These correspond as indicated to the fields in the "Surface Meshing" attribute
  int surfaceSmoothingLevel;      // "Smoothing Level"
  int surfaceSmoothingType;       // "Smoothing Type"
  int surfaceFixIntersections;    // "Fix Intersections"
  int surfaceOptimization;        // "Optimization"
  int surfaceEnforceGradation;    // "Enforce Spatial Gradation"
  double surfaceFaceRotationLimit;// "Discrete Face Rotation Limit"
  int surfaceCurveType;           // ignore

  bool volumeRun;                 // whether to run volume meshing
  bool volumeDoStructured;        // always true
  bool volumeDoUnstructured;      // always true
  bool volumeDoCurve;             // whether to run mesh curving

  // These correspond as indicated to the fields in the "Volume Meshing" attribute
  int volumeEnforceSize;          // "Enforce Size"
  int volumeSmoothingLevel;       // "Smoothing Level"
  int volumeSmoothingType;        // "Smoothing Type"
  int volumeOptimization;         // "Optimization"
  int volumeCurveType;            // ignore
};

AdvMeshing_EXPORT void MS_setupSimModelerMeshCase(pACase sourceCase, pACase meshCase, MeshingOptions *options);
pAManager SModel_attManager(pModel model);

namespace {

const char* modelFile = 0;
const char* caseName = 0;
const char* outMeshFile = 0;

void messageHandler(int type, const char* msg)
{
  switch (type) {
    case Sim_WarningMsg:
      if(!PCU_Comm_Self())
        fprintf(stdout, "Warning SimModeler %s\n", msg);
      break;
    case Sim_ErrorMsg:
      if(!PCU_Comm_Self()) 
        fprintf(stdout, "Error SimModeler %s ... exiting\n", msg);
      MPI_Finalize();
      exit(EXIT_SUCCESS); 
      break;
    default:
      // Ignore sim info and debug messages
      break;
  }
}

pParMesh generate(pGModel mdl, std::string meshCaseName) {
  pAManager attmngr = SModel_attManager(mdl);
  if(0==PCU_Comm_Self())
    fprintf(stdout, "Loading mesh case %s...\n", meshCaseName.c_str());
  pACase mcaseFile = AMAN_findCase(attmngr, meshCaseName.c_str());
  assert(mcaseFile);

  AttCase_setModel(mcaseFile, mdl);
  pACase mcase = MS_newMeshCase(mdl);
  MeshingOptions meshingOptions;
  MS_setupSimModelerMeshCase(mcaseFile, mcase, &meshingOptions);
  AttCase_setModel(mcase, mdl);

  pParMesh pmesh = PM_new(0, mdl, PMU_size());

  if(0==PCU_Comm_Self())
    fprintf(stdout, "Meshing surface...\n");
  pSurfaceMesher surfaceMesher = SurfaceMesher_new(mcase, pmesh);
  SurfaceMesher_execute(surfaceMesher, NULL);

  if(0==PCU_Comm_Self())
    fprintf(stdout, "Meshing volume...\n");
  pVolumeMesher volumeMesher = VolumeMesher_new(mcase, pmesh);
  VolumeMesher_execute(volumeMesher, NULL);

  SurfaceMesher_delete(surfaceMesher);
  VolumeMesher_delete(volumeMesher);
  return pmesh;
}

void getConfig(int argc, char** argv)
{
  if (argc != 4) {
    if(0==PCU_Comm_Self())
      printf("Usage: %s <model (.smd)> <mesh case name> <outMesh>\n", argv[0]);
    MPI_Finalize();
    exit(EXIT_SUCCESS);
  }
  modelFile = argv[1];
  caseName = argv[2];
  outMeshFile = argv[3];
}

} //end unnamed namespace

int main(int argc, char** argv)
{
  MPI_Init(&argc, &argv);
  PCU_Comm_Init();
  PCU_Protect();
  getConfig(argc,argv);

  SimModel_start();
  SimPartitionedMesh_start(NULL,NULL);
  Sim_readLicenseFile(NULL);
  MS_init();
  Sim_setMessageHandler(messageHandler);

  pGModel simModel = GM_load(modelFile, NULL, NULL);

  const double t0 = MPI_Wtime();
  pParMesh sim_mesh = generate(simModel, caseName);
  const double t1 = MPI_Wtime();
  if(0==PCU_Comm_Self())
    fprintf(stdout, "Mesh generated in %f seconds\n", t1-t0);
  apf::Mesh* simApfMesh = apf::createMesh(sim_mesh);
  
  gmi_register_sim();
  gmi_model* model = gmi_import_sim(simModel);
  apf::Mesh2* mesh = apf::createMdsMesh(model, simApfMesh);
  apf::destroyMesh(simApfMesh);
  M_release(sim_mesh);
  if (alignMdsMatches(mesh))
    printf("fixed misaligned matches\n");
  else
    printf("matches (if any) are aligned ok\n");
  mesh->verify();
  mesh->writeNative(outMeshFile);

  mesh->destroyNative();
  apf::destroyMesh(mesh);

  SimModel_stop();
  SimPartitionedMesh_stop();
  Sim_unregisterAllKeys();
  PCU_Comm_Free();
  MPI_Finalize();
}
