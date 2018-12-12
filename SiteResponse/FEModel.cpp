/* ********************************************************************* **
**                 Site Response Analysis Tool                           **
**   -----------------------------------------------------------------   **
**                                                                       **
**   Developed by: Alborz Ghofrani (alborzgh@uw.edu)                     **
**                 University of Washington                              **
**                                                                       **
**   Date: October 2018                                                  **
**                                                                       **
** ********************************************************************* */

#include <vector>
#include <iostream>
#include <sstream>

#include "FEModel.h"

#include "Vector.h"
#include "Matrix.h"

#include "Node.h"
#include "Element.h"
#include "NDMaterial.h"
#include "SP_Constraint.h"
#include "MP_Constraint.h"
#include "LinearSeries.h"
#include "PathSeries.h"
#include "PathTimeSeries.h"
#include "LoadPattern.h"
#include "NodalLoad.h"
#include "AnalysisModel.h"
#include "CTestNormDispIncr.h"
#include "StaticAnalysis.h"
#include "DirectIntegrationAnalysis.h"
#include "EquiSolnAlgo.h"
#include "StaticIntegrator.h"
#include "TransientIntegrator.h"
#include "ConstraintHandler.h"
#include "RCM.h"
#include "DOF_Numberer.h"
#include "BandGenLinSolver.h"

#include "LinearSOE.h"
#include "NodeIter.h"
#include "ElementIter.h"
#include "DataFileStream.h"
#include "Recorder.h"
#include "UniaxialMaterial.h"
#include "ElementStateParameter.h"

#include "SSPbrick.h"
#include "SSPquad.h"
#include "SSPquadUP.h"
#include "Brick.h"
#include "J2CyclicBoundingSurface.h"
#include "ElasticIsotropicMaterial.h"
#include "PM4Sand.h"
#include "ElasticMaterial.h"
#include "NewtonRaphson.h"
#include "LoadControl.h"
#include "Newmark.h"
#include "PenaltyConstraintHandler.h"
#include "TransformationConstraintHandler.h"
#include "BandGenLinLapackSolver.h"
#include "BandGenLinSOE.h"
#include "GroundMotion.h"
#include "ImposedMotionSP.h"
#include "TimeSeriesIntegrator.h"
#include "MultiSupportPattern.h"
#include "UniformExcitation.h"
#include "VariableTimeStepDirectIntegrationAnalysis.h"
#include "NodeRecorder.h"
#include "ElementRecorder.h"
#include "ViscousMaterial.h"
#include "ZeroLength.h"
#include "SingleDomParamIter.h"

#include "Information.h"
#include <vector> 

#include <map>

#define PRINTDEBUG true

#if defined(WIN32) || defined(_WIN32)
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

SiteResponseModel::SiteResponseModel() : theModelType("2D"),
										 theMotionX(0),
										 theMotionZ(0),
										 theOutputDir(".")
{
}

SiteResponseModel::SiteResponseModel(SiteLayering layering, std::string modelType, OutcropMotion *motionX, OutcropMotion *motionY) : SRM_layering(layering),
																																	 theModelType(modelType),
																																	 theMotionX(motionX),
																																	 theMotionZ(motionY),
																																	 theOutputDir(".")
{
	if (theMotionX->isInitialized() || theMotionZ->isInitialized())
		theDomain = new Domain();
	else
	{
		opserr << "No motion is specified." << endln;
		exit(-1);
	}
}

SiteResponseModel::SiteResponseModel(SiteLayering layering, std::string modelType, OutcropMotion *motionX) : SRM_layering(layering),
																											 theModelType(modelType),
																											 theMotionX(motionX),
																											 theOutputDir(".")
{
	if (theMotionX->isInitialized())
		theDomain = new Domain();
	else
	{
		opserr << "No motion is specified." << endln;
		exit(-1);
	}
}

SiteResponseModel::~SiteResponseModel()
{
	if (theDomain != NULL)
		delete theDomain;
	theDomain = NULL;
}

int SiteResponseModel::runTotalStressModel()
{
	Vector zeroVec(3);
	zeroVec.Zero();

	std::vector<int> layerNumElems;
	std::vector<int> layerNumNodes;
	std::vector<double> layerElemSize;

	// setup the geometry and mesh parameters
	int numLayers = SRM_layering.getNumLayers();
	int numElems = 0;
	int numNodes = 0;
	int numNodesPerLayer = theModelType.compare("2D") ? 4 : 2;
	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		double thisLayerThick = SRM_layering.getLayer(layerCount).getThickness();
		double thisLayerVS = SRM_layering.getLayer(layerCount).getShearVelocity();
		double thisLayerMinWL = thisLayerVS / MAX_FREQUENCY;

		thisLayerThick = (thisLayerThick < thisLayerMinWL) ? thisLayerMinWL : thisLayerThick;

		int thisLayerNumEle = NODES_PER_WAVELENGTH * static_cast<int>(thisLayerThick / thisLayerMinWL) - 1;

		layerNumElems.push_back(thisLayerNumEle);

		layerNumNodes.push_back(numNodesPerLayer * (thisLayerNumEle + (layerCount == 0)));
		layerElemSize.push_back(thisLayerThick / thisLayerNumEle);

		numElems += thisLayerNumEle;
		numNodes += numNodesPerLayer * (thisLayerNumEle + (layerCount == numLayers - 2));

		if (PRINTDEBUG)
			opserr << "Layer " << SRM_layering.getLayer(layerCount).getName().c_str() << " : Num Elements = " << thisLayerNumEle
				   << "(" << thisLayerThick / thisLayerNumEle << "), "
				   << ", Num Nodes = " << ((!theModelType.compare("2D")) ? 2 : 4) * (thisLayerNumEle + (layerCount == 0)) << endln;
	}

	// create the nodes
	Node *theNode;

	double yCoord = 0.0;
	int nCount = 0;
	for (int layerCount = numLayers - 2; layerCount > -1; --layerCount)
	{
		if (PRINTDEBUG)
			opserr << "layer : " << SRM_layering.getLayer(layerCount).getName().c_str() << " - Number of Elements = "
				   << layerNumElems[layerCount] << " - Number of Nodes = " << layerNumNodes[layerCount]
				   << " - Element Thickness = " << layerElemSize[layerCount] << endln;

		for (int nodeCount = 0; nodeCount < layerNumNodes[layerCount]; nodeCount += numNodesPerLayer)
		{
			if (!theModelType.compare("2D"))
			{ //2D
				theNode = new Node(nCount + nodeCount + 1, 2, 0.0, yCoord);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 2, 2, 1.0, yCoord);
				theDomain->addNode(theNode);
			}
			else
			{ //3D
				theNode = new Node(nCount + nodeCount + 1, 3, 0.0, yCoord, 0.0);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 2, 3, 0.0, yCoord, 1.0);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 3, 3, 1.0, yCoord, 1.0);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 4, 3, 1.0, yCoord, 0.0);
				theDomain->addNode(theNode);
			}

			if (PRINTDEBUG)
			{
				if (!theModelType.compare("2D"))
				{
					opserr << "Node " << nCount + nodeCount + 1 << " - 0.0"
						   << ", " << yCoord << endln;
					opserr << "Node " << nCount + nodeCount + 2 << " - 1.0"
						   << ", " << yCoord << endln;
				}
				else
				{
					opserr << "Node " << nCount + nodeCount + 1 << " - 0.0"
						   << ", " << yCoord << ", 0.0" << endln;
					opserr << "Node " << nCount + nodeCount + 2 << " - 0.0"
						   << ", " << yCoord << ", 1.0" << endln;
					opserr << "Node " << nCount + nodeCount + 3 << " - 1.0"
						   << ", " << yCoord << ", 1.0" << endln;
					opserr << "Node " << nCount + nodeCount + 4 << " - 1.0"
						   << ", " << yCoord << ", 0.0" << endln;
				}
			}

			yCoord += layerElemSize[layerCount];
		}
		nCount += layerNumNodes[layerCount];
	}

	// apply fixities
	SP_Constraint *theSP;
	int sizeTheSPtoRemove = (!theModelType.compare("2D")) ? 2 : 8;
	ID theSPtoRemove(sizeTheSPtoRemove); // these fixities should be removed later on if compliant base is used
	if (!theModelType.compare("2D"))
	{ //2D

		theSP = new SP_Constraint(1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(0) = theSP->getTag();
		theSP = new SP_Constraint(1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		theSP = new SP_Constraint(2, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(1) = theSP->getTag();
		theSP = new SP_Constraint(2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
	}
	else
	{ //3D

		theSP = new SP_Constraint(1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(0) = theSP->getTag();
		theSP = new SP_Constraint(1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(1, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(1) = theSP->getTag();

		theSP = new SP_Constraint(2, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(2) = theSP->getTag();
		theSP = new SP_Constraint(2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(2, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(3) = theSP->getTag();

		theSP = new SP_Constraint(3, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(4) = theSP->getTag();
		theSP = new SP_Constraint(3, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(3, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(5) = theSP->getTag();

		theSP = new SP_Constraint(4, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(6) = theSP->getTag();
		theSP = new SP_Constraint(4, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(4, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(7) = theSP->getTag();
	}

	// apply equalDOF
	MP_Constraint *theMP;
	int crrdim = (!theModelType.compare("2D")) ? 2 : 3;
	Matrix Ccr(crrdim, crrdim);
	ID rcDOF(crrdim);
	if (!theModelType.compare("2D"))
	{
		Ccr(0, 0) = 1.0;
		Ccr(1, 1) = 1.0;
		rcDOF(0) = 0;
		rcDOF(1) = 1;
		for (int nodeCount = 2; nodeCount < numNodes; nodeCount += 2)
		{
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 2, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
		}
	}
	else
	{
		Ccr(0, 0) = 1.0;
		Ccr(1, 1) = 1.0;
		Ccr(2, 2) = 1.0;
		rcDOF(0) = 0;
		rcDOF(1) = 1;
		rcDOF(2) = 2;
		for (int nodeCount = 4; nodeCount < numNodes; nodeCount += 4)
		{
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 2, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 3, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 4, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
		}
	}

	// create the materials
	NDMaterial *theMat;
	SoilLayer theLayer;
	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		theLayer = (SRM_layering.getLayer(numLayers - layerCount - 2));
		//theMat = new J2CyclicBoundingSurface(numLayers - layerCount - 1, theLayer.getMatShearModulus(), theLayer.getMatBulkModulus(),
		//	theLayer.getSu(), theLayer.getRho(), theLayer.getMat_h() * theLayer.getMatShearModulus(), theLayer.getMat_m(), 0.0, 0.5);
		//theMat = new ElasticIsotropicMaterial(numLayers - layerCount - 1, 20000.0, 0.3, theLayer.getRho());

		/*
		double emax = 0.8;
		double emin = 0.5;
		double Dr = 0.463;
		double evoid = emax - Dr *(emax-emin) ;
		double Gs = 2.67;
		double rho_d = Gs / (1.0 + evoid);

		//nDmaterial PM4Sand $matTag $Dr $G0   $hpo   $Den <$patm $h0     $emax $emin  $nb  $nd  $Ado  $zmax $cz $  ce     $phic $nu $cgd $cdr $ckaf $Q $R $m $Fsed_min $p_sedo>
		//nDMaterial PM4Sand 3       $Dr 468.3 0.463 $rho_d 101.3 -1.00   $emax  $emin 0.5  0.1  -1.0  -1.0  250.0  -1.00  33.0  $nu
		theMat = new PM4Sand(numLayers - layerCount - 1, 0.463, 468.3, 0.45, rho_d, 
			101.3, -1.0, emax, emin, 0.5, 0.1, -1.0, -1.0, 250.0, -1.0, 33.0, 0.5/(1+0.5),
			2.0, -1.0, -1.0, 10.0, 1.5, 0.01, -1.0, -1.0,
			1, 1, 1e-4, 1e-4);
		*/

		double emax = 0.8;
		double emin = 0.5;

		double Gs = 2.67;
		double G0 = theLayer.getMatShearModulus();
		double rho_d = theLayer.getRho();
		double evoid = Gs / rho_d - 1;
		double Dr = (emax - evoid) / (emax - emin);
		double hpo = 0.053;
		theMat = new PM4Sand(numLayers - layerCount - 1, Dr, G0, hpo, rho_d);

		OPS_addNDMaterial(theMat);

		if (PRINTDEBUG)
			opserr << "Material " << theLayer.getName().c_str() << " tag = " << numLayers - layerCount - 1 << endln;
	}

	// create soil elements and add the material state parameter
	Element *theEle;
	Parameter *theParameter;
	char **paramArgs = new char *[2];

	paramArgs[0] = new char[15];
	paramArgs[1] = new char[5];
	sprintf(paramArgs[0], "materialState");

	int nElem = 0;
	int node1Tag;

	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		theMat = OPS_getNDMaterial(numLayers - layerCount - 1);
		for (int elemCount = 0; elemCount < layerNumElems[numLayers - layerCount - 2]; ++elemCount)
		{
			node1Tag = numNodesPerLayer * (nElem + elemCount);

			if (!theModelType.compare("2D"))
			{ //2D
				theEle = new SSPquad(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 4, node1Tag + 3,
									 *theMat, "PlaneStrain", 1.0, 0.0, -9.81 * theMat->getRho());
				/*
				 //element SSPquadUP [expr ($nElemX)*($j+$count-1) + $k] $nI $nJ $nK $nL $i $thick($i) $uBulk($i) 1.0 1.0 1.0 $eInit($i) 0.0 $xWgt($i) $yWgt($i)
				double emax = 0.8;
				double emin = 0.5;
				double Dr = 0.463;
				double evoid = emax - Dr *(emax-emin) ;
				theEle = new SSPquadUP(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 3, node1Tag + 4,
				 *theMat, 1.0,2.2e6, 1.0, 1.0, 1.0,evoid, 0.0, 0.0, -9.81 * theMat->getRho()); 
				 */
			}
			else
			{ //3D
				theEle = new SSPbrick(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 3, node1Tag + 4, node1Tag + 5,
									  node1Tag + 6, node1Tag + 7, node1Tag + 8, *theMat, 0.0, -9.81 * theMat->getRho(), 0.0);
			}

			theDomain->addElement(theEle);

			theParameter = new Parameter(nElem + elemCount + 1, 0, 0, 0);
			sprintf(paramArgs[1], "%d", theMat->getTag());
			theEle->setParameter(const_cast<const char **>(paramArgs), 2, *theParameter);
			theDomain->addParameter(theParameter);

			if (PRINTDEBUG)
				opserr << "Element " << nElem + elemCount + 1 << ": Nodes = " << node1Tag + 1 << " to " << node1Tag + ((!theModelType.compare("2D")) ? 4 : 8) << "  - Mat tag = " << numLayers - layerCount - 1 << endln;
		}
		nElem += layerNumElems[numLayers - layerCount - 2];
	}

	if (PRINTDEBUG)
		opserr << "Total number of elements = " << nElem << endln;
	/*
	OPS_Stream *theOutputStreamAll;
	theOutputStreamAll = new DataFileStream("Domain.out", OVERWRITE, 2, 0, false, 6, false);
	theDomain->Print(*theOutputStreamAll);
	opserr << theOutputStreamAll;
	*/

	// update material stage
	ParameterIter &theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		theParameter->update(0.0);
	}

	// create analysis objects - I use static analysis for gravity
	AnalysisModel *theModel = new AnalysisModel();
	CTestNormDispIncr *theTest = new CTestNormDispIncr(1.0e-7, 30, 1);
	EquiSolnAlgo *theSolnAlgo = new NewtonRaphson(*theTest);
	StaticIntegrator *theIntegrator = new LoadControl(0.05, 1, 0.05, 1.0);
	//TransientIntegrator* theIntegrator = new Newmark(0.5, 0.25);
	//ConstraintHandler* theHandler = new PenaltyConstraintHandler(1.0e14, 1.0e14);
	ConstraintHandler *theHandler = new TransformationConstraintHandler();
	RCM *theRCM = new RCM();
	DOF_Numberer *theNumberer = new DOF_Numberer(*theRCM);
	BandGenLinSolver *theSolver = new BandGenLinLapackSolver();
	LinearSOE *theSOE = new BandGenLinSOE(*theSolver);

	//DirectIntegrationAnalysis* theAnalysis;
	//theAnalysis = new DirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	//VariableTimeStepDirectIntegrationAnalysis* theAnalysis;
	//theAnalysis = new VariableTimeStepDirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	StaticAnalysis *theAnalysis;
	theAnalysis = new StaticAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator);
	theAnalysis->setConvergenceTest(*theTest);

	for (int analysisCount = 0; analysisCount < 2; ++analysisCount)
	{
		//int converged = theAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		int converged = theAnalysis->analyze(1);
		if (!converged)
		{
			opserr << "Converged at time " << theDomain->getCurrentTime() << endln;
		}
	}

	// update material response to plastic
	theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		theParameter->update(1.0);
	}

	for (int analysisCount = 0; analysisCount < 2; ++analysisCount)
	{
		//int converged = theAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		int converged = theAnalysis->analyze(1);
		if (!converged)
		{
			opserr << "Converged at time " << theDomain->getCurrentTime() << endln;
		}
	}

	// add the compliant base - use the last layer properties
	double vis_C = SRM_layering.getLayer(numLayers - 1).getShearVelocity() * SRM_layering.getLayer(numLayers - 1).getRho();
	int numberTheViscousMats = (!theModelType.compare("2D")) ? 1 : 2;
	opserr << numberTheViscousMats << endln;
	UniaxialMaterial *theViscousMats[numberTheViscousMats];
	theViscousMats[0] = new ViscousMaterial(numLayers + 10, vis_C, 1.0);
	OPS_addUniaxialMaterial(theViscousMats[0]);
	if (theModelType.compare("2D")) // 3D
	{
		theViscousMats[1] = new ViscousMaterial(numLayers + 20, vis_C, 1.0);
		OPS_addUniaxialMaterial(theViscousMats[1]);
	}
	int numberDirections = (!theModelType.compare("2D")) ? 1 : 2;
	ID directions(numberDirections);
	directions(0) = 0;
	if (theModelType.compare("2D")) // 3D
		directions(1) = 2;

	// create dashpot nodes and apply proper fixities
	if (!theModelType.compare("2D")) //2D
	{
		theNode = new Node(numNodes + 1, 2, 0.0, 0.0);
		theDomain->addNode(theNode); // TODO ?
		theNode = new Node(numNodes + 2, 2, 0.0, 0.0);
		theDomain->addNode(theNode); // TODO ?

		theSP = new SP_Constraint(numNodes + 1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		theSP = new SP_Constraint(numNodes + 2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
	}
	else
	{ //3D
		theNode = new Node(numNodes + 1, 3, 0.0, 0.0, 0.0, NULL);
		theDomain->addNode(theNode);
		theNode = new Node(numNodes + 2, 3, 0.0, 0.0, 0.0, NULL);
		theDomain->addNode(theNode);
		theSP = new SP_Constraint(numNodes + 1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
	}

	// apply equalDOF to the node connected to the column
	theMP = new MP_Constraint(1, numNodes + 2, Ccr, rcDOF, rcDOF);
	theDomain->addMP_Constraint(theMP);

	// remove fixities created for gravity
	for (int i_remove = 0; i_remove < sizeTheSPtoRemove; i_remove++)
	{
		theSP = theDomain->removeSP_Constraint(theSPtoRemove(i_remove));
		delete theSP;
	}
	/*
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(1)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(2)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(3)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(4)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(5)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(6)); delete theSP;
	theSP = theDomain->removeSP_Constraint(theSPtoRemove(7)); delete theSP;
	*/

	// equalDOF the first 4 nodes (3D) or 2 nodes (2D)
	int numMP1 = (!theModelType.compare("2D")) ? 1 : 2;
	Matrix constrainInXZ(numMP1, numMP1);
	ID constDOF(numMP1);
	if (!theModelType.compare("2D")) //2D
	{
		constrainInXZ(0, 0) = 1.0;
		constDOF(0) = 0;
		theMP = new MP_Constraint(1, 2, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
	}
	else //3D
	{
		constrainInXZ(0, 0) = 1.0;
		constrainInXZ(1, 1) = 1.0;
		constDOF(0) = 0;
		constDOF(1) = 2;
		theMP = new MP_Constraint(1, 2, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
		theMP = new MP_Constraint(1, 3, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
		theMP = new MP_Constraint(1, 4, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
	}

	// create the dashpot element
	int numDimDashpot = 3; //(!theModelType.compare("2D")) ? 2 : 3;
	Vector x(numDimDashpot);
	Vector y(numDimDashpot);
	if (!theModelType.compare("2D")) //2D
	{
		x(0) = 1.0;
		x(1) = 0.0;
		x(2) = 0.0;
		y(1) = 1.0;
		y(0) = 0.0;
		y(2) = 0.0;
		//element zeroLength [expr $nElemT+1]  $dashF $dashS -mat [expr $numLayers+1]  -dir 1
		theEle = new ZeroLength(numElems + 1, 2, numNodes + 1, numNodes + 2, x, y, 1, theViscousMats, directions); //TODO ?
	}
	else
	{ //3D
		x(0) = 1.0;
		x(1) = 0.0;
		x(2) = 0.0;
		y(1) = 1.0;
		y(0) = 0.0;
		y(2) = 0.0;
		theEle = new ZeroLength(numElems + 1, 3, numNodes + 1, numNodes + 2, x, y, 2, theViscousMats, directions); //TODO ?
	}
	theDomain->addElement(theEle);

	// apply the motion
	int numSteps = 0;
	std::vector<double> dt;

	// using multiple support
	//MultiSupportPattern* theLP = new MultiSupportPattern(1);
	//theLP->addMotion(*theMotionX->getGroundMotion(), 1);
	//theLP->addSP_Constraint(new ImposedMotionSP(1, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(2, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(3, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(4, 0, 1, 1));

	// using uniform excitation
	// LoadPattern* theLP = new UniformExcitation(*theMotion, 1, 1, 0.0, 1.0);
	//theDomain->addLoadPattern(theLP);

	// using a stress input with the dashpot
	if (theMotionX->isInitialized())
	{
		LoadPattern *theLP = new LoadPattern(1, vis_C);
		theLP->setTimeSeries(theMotionX->getVelSeries());

		NodalLoad *theLoad;
		int numLoads = (!theModelType.compare("2D")) ? 2 : 3;
		Vector load(numLoads);
		load(0) = 1.0;
		load(1) = 0.0;
		if (theModelType.compare("2D")) // 3D
			load(2) = 0.0;

		theLoad = new NodalLoad(1, numNodes + 2, load, false);
		theLP->addNodalLoad(theLoad);
		theDomain->addLoadPattern(theLP);

		// update the number of steps as well as the dt vector
		int temp = theMotionX->getNumSteps();
		if (temp > numSteps)
		{
			numSteps = temp;
			dt = theMotionX->getDTvector();
		}
	}

	if (theModelType.compare("2D")) // 3D
	{
		if (theMotionZ->isInitialized())
		{
			LoadPattern *theLP = new LoadPattern(2, vis_C);
			theLP->setTimeSeries(theMotionZ->getVelSeries());

			NodalLoad *theLoad;
			Vector load(3);
			load(0) = 0.0;
			load(1) = 0.0;
			load(2) = 1.0;

			theLoad = new NodalLoad(2, numNodes + 2, load, false);
			theLP->addNodalLoad(theLoad);
			theDomain->addLoadPattern(theLP);

			int temp = theMotionZ->getNumSteps();
			if (temp > numSteps)
			{
				numSteps = temp;
				dt = theMotionZ->getDTvector();
			}
		}
	}

	// I have to change to a transient analysis
	// remove the static analysis and create new transient objects
	delete theIntegrator;
	delete theAnalysis;

	TransientIntegrator *theTransientIntegrator = new Newmark(0.5, 0.25);
	theTest->setTolerance(1.0e-5);

	//DirectIntegrationAnalysis* theTransientAnalysis;
	//theTransientAnalysis = new DirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theTransientIntegrator, theTest);

	VariableTimeStepDirectIntegrationAnalysis *theTransientAnalysis;
	theTransientAnalysis = new VariableTimeStepDirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theTransientIntegrator, theTest);

	// reset time in the domain
	theDomain->setCurrentTime(0.0);

	// setup Rayleigh damping
	// apply 2% at the natural frequency and 5*natural frequency
	double natFreq = SRM_layering.getNaturalPeriod();
	double dampRatio = 0.02;
	double pi = 4.0 * atan(1.0);
	double a0 = dampRatio * (10.0 * pi * natFreq) / 3.0;
	double a1 = dampRatio / (6.0 * pi * natFreq);
	if (PRINTDEBUG)
	{
		opserr << "f1 = " << natFreq << "    f2 = " << 5.0 * natFreq << endln;
		opserr << "a0 = " << a0 << "    a1 = " << a1 << endln;
	}
	theDomain->setRayleighDampingFactors(a0, a1, 0.0, 0.0);

	// create the output streams
	OPS_Stream *theOutputStream;
	Recorder *theRecorder;

	// record last node's results
	ID nodesToRecord(1);
	nodesToRecord(0) = numNodes;

	int dimDofToRecord = (!theModelType.compare("2D")) ? 2 : 3;
	ID dofToRecord(dimDofToRecord);
	dofToRecord(0) = 0;
	dofToRecord(1) = 1;
	if (theModelType.compare("2D")) // 3D
		dofToRecord(2) = 2;

	std::string outFile = theOutputDir + PATH_SEPARATOR + "surface.acc";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "accel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "surface.vel";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "vel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "surface.disp";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "disp", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	// record element results
	// OPS_Stream* theOutputStream2;
	// ID elemsToRecord(5);
	// elemsToRecord(0) = 1;
	// elemsToRecord(1) = 2;
	// elemsToRecord(2) = 3;
	// elemsToRecord(3) = 4;
	// elemsToRecord(4) = 5;
	// const char* eleArgs = "stress";
	//
	// theOutputStream2 = new DataFileStream("Output2.out", OVERWRITE, 2, 0, false, 6, false);
	// theRecorder = new ElementRecorder(&elemsToRecord, &eleArgs, 1, true, *theDomain, *theOutputStream2, 0.0, NULL);
	// theDomain->addRecorder(*theRecorder);

	/*
	OPS_Stream* theOutputStreamAll;
	theOutputStreamAll = new DataFileStream("Domain.out", OVERWRITE, 2, 0, false, 6, false);
	theDomain->Print(*theOutputStreamAll);
	opserr << theOutputStreamAll;
	*/

	// perform analysis
	opserr << "Analysis started:" << endln;
	std::stringstream progressBar;
	for (int analysisCount = 0; analysisCount < numSteps; ++analysisCount)
	{
		//int converged = theAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		double stepDT = dt[analysisCount];
		int converged = theTransientAnalysis->analyze(1, stepDT, stepDT / 2.0, stepDT * 2.0, 1); // *
		//int converged = theTransientAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		//int converged = theTransientAnalysis->analyze(1, 0.002);
		if (!converged)
		{
			opserr << "Converged at time " << theDomain->getCurrentTime() << endln;

			if (analysisCount % (int)(numSteps / 20) == 0)
			{
				progressBar << "\r[";
				for (int ii = 0; ii < (int)(20 * analysisCount / numSteps); ii++)
					progressBar << ".";
				for (int ii = (int)(20 * analysisCount / numSteps); ii < 20; ii++)
					progressBar << "-";

				progressBar << "]  " << (int)(100 * analysisCount / numSteps) << "%";
				opsout << progressBar.str().c_str();
				opsout.flush();
			}
		}
		else
		{
			opserr << "Site response analysis did not converge." << endln;
			exit(-1);
		}
	}
	progressBar << "\r[";
	for (int ii = 0; ii < 20; ii++)
		progressBar << ".";

	progressBar << "] 100%";
	opsout << progressBar.str().c_str();
	opsout.flush();
	opsout << endln;

	//if (PRINTDEBUG)
	//{
	//	Information info;
	//	theEle = theDomain->getElement(1);
	//	theEle->getResponse(1, info);
	//	opserr << "Stress = " << info.getData();
	//	theEle->getResponse(2, info);
	//	opserr << "Strain = " << info.getData();
	//}

	//int count = 0;
	//NodeIter& theNodeIter = theDomain->getNodes();
	//Node * thisNode;
	//while ((thisNode = theNodeIter()) != 0)
	//{
	//	count++;
	//	opserr << "Node " << thisNode->getTag() << " = " << thisNode->getCrds() << endln;
	//}

	//int count = 0;
	//ElementIter& theEleIter = theDomain->getElements();
	//Element * thisEle;
	//while ((thisEle = theEleIter()) != 0)
	//{
	//	count++;
	//	opserr << "Element " << thisEle->getTag() << " = " << thisEle->getExternalNodes() << endln;
	//}

	return 0;
}

int SiteResponseModel::runEffectiveStressModel()
{
	Vector zeroVec(3);
	zeroVec.Zero();


	

	// set outputs for tcl 
	ofstream s ("model.tcl", std::ofstream::out);
	s << "# #########################################################" << "\n\n";
	s << "wipe \n\n";

	

	std::vector<int> layerNumElems;
	std::vector<int> layerNumNodes;
	std::vector<double> layerElemSize;
	std::vector<int> dryNodes;

	// ------------------------------------------
	// 1. setup the geometry and mesh parameters
	// ------------------------------------------

	s << "# ------------------------------------------ \n";
	s << "# 1. geometry and mesh info       \n";
	s << "# ------------------------------------------ \n \n";

	int numLayers = SRM_layering.getNumLayers();
	double totalThickness = SRM_layering.getTotThickness();
	double gwt = 2.0; // ground water table
	double colThickness = 1.0; // column thickness set as unit


	int nElemX = 1; //number of elements in horizontal direction
	int nNodeX = 2; // number of nodes in horizontal direction
	double sElemX = 0.25; // horizontal element size (m)


	int numElems = 0;
	int numNodes = 0;
	int numNodesPerLayer = theModelType.compare("2D") ? 4 : 2;
	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		double thisLayerThick = SRM_layering.getLayer(layerCount).getThickness();

		/*
		// control layer thickness : TODO
		double thisLayerVS = SRM_layering.getLayer(layerCount).getShearVelocity();
		double thisLayerMinWL = thisLayerVS / MAX_FREQUENCY;
		thisLayerThick = (thisLayerThick < thisLayerMinWL) ? thisLayerMinWL : thisLayerThick;
		*/



		//int thisLayerNumEle = NODES_PER_WAVELENGTH * static_cast<int>(thisLayerThick / thisLayerMinWL) - 1;
		int thisLayerNumEle = SRM_layering.getLayer(layerCount).getNumEle();

		layerNumElems.push_back(thisLayerNumEle);

		layerNumNodes.push_back(numNodesPerLayer * (thisLayerNumEle + (layerCount == 0)));
		layerElemSize.push_back(thisLayerThick / thisLayerNumEle);

		numElems += thisLayerNumEle;
		numNodes += numNodesPerLayer * (thisLayerNumEle + (layerCount == numLayers - 2));


		s << "# Layer " << SRM_layering.getLayer(layerCount).getName().c_str() << " : Num Elements = " << thisLayerNumEle
				   << " (esize = " << thisLayerThick / thisLayerNumEle << "), "
				   << "Num Nodes = " << ((!theModelType.compare("2D")) ? 2 : 4) * (thisLayerNumEle + (layerCount == 0)) << endln;

		if (PRINTDEBUG)
			opserr << "Layer " << SRM_layering.getLayer(layerCount).getName().c_str() << " : Num Elements = " << thisLayerNumEle
				   << "(" << thisLayerThick / thisLayerNumEle << "), "
				   << ", Num Nodes = " << ((!theModelType.compare("2D")) ? 2 : 4) * (thisLayerNumEle + (layerCount == 0)) << endln;
	}


	// ---------------------------------------------------------
	// 2. create the pore pressure nodes and boundary conditions
	// ---------------------------------------------------------

	s << "\n";
	s << "# ---------------------------------------------------------\n";
	s << "# 2. create the pore pressure nodes and boundary conditions\n";
	s << "# ---------------------------------------------------------\n\n";
	s << "model BasicBuilder -ndm 2 -ndf 3  \n\n";

	
	Node *theNode;

	// 2.1 create pore pressure nodes
	s << "# 2.1 create pore pressure nodes" << endln << endln;
	double yCoord = 0.0;
	int nCount = 0;

	s << "set sElemX " << sElemX << endln; 

	for (int layerCount = numLayers - 2; layerCount > -1; --layerCount)
	{
		if (PRINTDEBUG)
			opserr << "layer : " << SRM_layering.getLayer(layerCount).getName().c_str() << " - Number of Elements = "
				   << layerNumElems[layerCount] << " - Number of Nodes = " << layerNumNodes[layerCount]
				   << " - Element Thickness = " << layerElemSize[layerCount] << endln;

		for (int nodeCount = 0; nodeCount < layerNumNodes[layerCount]; nodeCount += numNodesPerLayer)
		{
			if (!theModelType.compare("2D"))
			{ //2D
				theNode = new Node(nCount + nodeCount + 1, 3, 0.0, yCoord);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 2, 3, sElemX, yCoord);
				theDomain->addNode(theNode);

				s << "node " << nCount + nodeCount + 1 << " 0.0 " << yCoord << endln;
				s << "node " << nCount + nodeCount + 2 << " " << sElemX << " " << yCoord << endln;

				if (yCoord >= (totalThickness - gwt))
				{ //record dry nodes above ground water table
					dryNodes.push_back(nCount + nodeCount + 1);
					dryNodes.push_back(nCount + nodeCount + 2);
				}
			}
			else
			{ //3D
				theNode = new Node(nCount + nodeCount + 1, 4, 0.0, yCoord, 0.0);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 2, 4, 0.0, yCoord, sElemX);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 3, 4, 1.0, yCoord, sElemX);
				theDomain->addNode(theNode);
				theNode = new Node(nCount + nodeCount + 4, 4, 1.0, yCoord, 0.0);
				theDomain->addNode(theNode);
				if (yCoord >= (totalThickness - gwt))
				{ //record dry nodes above ground water table
					dryNodes.push_back(nCount + nodeCount + 1);
					dryNodes.push_back(nCount + nodeCount + 2);
					dryNodes.push_back(nCount + nodeCount + 3);
					dryNodes.push_back(nCount + nodeCount + 4);
				}
			}

			if (PRINTDEBUG)
			{
				if (!theModelType.compare("2D"))
				{
					opserr << "Node " << nCount + nodeCount + 1 << " - 0.0"
						   << ", " << yCoord << endln;
					opserr << "Node " << nCount + nodeCount + 2 << " - " << sElemX
						   << ", " << yCoord << endln;
				}
				else
				{
					opserr << "Node " << nCount + nodeCount + 1 << " - 0.0"
						   << ", " << yCoord << ", 0.0" << endln;
					opserr << "Node " << nCount + nodeCount + 2 << " - 0.0"
						   << ", " << yCoord << ", 1.0" << endln;
					opserr << "Node " << nCount + nodeCount + 3 << " - "<<sElemX
						   << ", " << yCoord << ", 1.0" << endln;
					opserr << "Node " << nCount + nodeCount + 4 << " - "<<sElemX
						   << ", " << yCoord << ", 0.0" << endln;
				}
			}

			yCoord += layerElemSize[layerCount];
		}
		nCount += layerNumNodes[layerCount];
	}

	

	// 2.2 apply fixities
	s << "\n# 2.2 apply fixities for pore pressure nodes. \n" << endln;
	SP_Constraint *theSP;
	int sizeTheSPtoRemove = (!theModelType.compare("2D")) ? 2 : 8;
	ID theSPtoRemove(sizeTheSPtoRemove); // these fixities should be removed later on if compliant base is used
	if (!theModelType.compare("2D"))
	{ //2D

		theSP = new SP_Constraint(1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(0) = theSP->getTag();
		theSP = new SP_Constraint(1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		s << "fix 1 1 1 0" << endln;

		theSP = new SP_Constraint(2, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(1) = theSP->getTag();
		theSP = new SP_Constraint(2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		s << "fix 2 1 1 0" << endln << endln;
	}
	else
	{ //3D

		theSP = new SP_Constraint(1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(0) = theSP->getTag();
		theSP = new SP_Constraint(1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(1, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(1) = theSP->getTag();

		theSP = new SP_Constraint(2, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(2) = theSP->getTag();
		theSP = new SP_Constraint(2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(2, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(3) = theSP->getTag();

		theSP = new SP_Constraint(3, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(4) = theSP->getTag();
		theSP = new SP_Constraint(3, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(3, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(5) = theSP->getTag();

		theSP = new SP_Constraint(4, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(6) = theSP->getTag();
		theSP = new SP_Constraint(4, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(4, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSPtoRemove(7) = theSP->getTag();

		s << "fix 1 1 1 1 0" << endln;
		s << "fix 2 1 1 1 0" << endln;
		s << "fix 3 1 1 1 0" << endln;
		s << "fix 4 1 1 1 0" << endln << endln;
	}

	// 2.3 define periodic boundary conditions for remaining nodes by applying equalDOF
	s << "# 2.3 define periodic boundary conditions for remaining nodes by applying equalDOF" << endln << endln;
	MP_Constraint *theMP;
	int crrdim = (!theModelType.compare("2D")) ? 2 : 3;
	Matrix Ccr(crrdim, crrdim);
	ID rcDOF(crrdim);
	// TODO: get clearified about the dimensions of Crr and rfDOF
	if (!theModelType.compare("2D"))
	{
		Ccr(0, 0) = 1.0;
		Ccr(1, 1) = 1.0; 
		//Ccr(2, 2) = 1.0;
		rcDOF(0) = 0;
		rcDOF(1) = 1; 
		//rcDOF(2) = 2;
		for (int nodeCount = 2; nodeCount < numNodes; nodeCount += 2)
		{
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 2, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			s << "equalDOF " << nodeCount + 1 << " "<< nodeCount + 2 << " 1 2" << endln;
		}
	}
	else
	{
		Ccr(0, 0) = 1.0;
		Ccr(1, 1) = 1.0;
		Ccr(2, 2) = 1.0; 
		//Ccr(3, 3) = 1.0;
		rcDOF(0) = 0;
		rcDOF(1) = 1;
		rcDOF(2) = 2; 
		//rcDOF(3) = 3;
		for (int nodeCount = 4; nodeCount < numNodes; nodeCount += 4)
		{
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 2, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 3, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			theMP = new MP_Constraint(nodeCount + 1, nodeCount + 4, Ccr, rcDOF, rcDOF);
			theDomain->addMP_Constraint(theMP);
			s << "equalDOF " << nodeCount + 1 << " "<< nodeCount + 2 << " 1 2 3" << endln;
			s << "equalDOF " << nodeCount + 1 << " "<< nodeCount + 3 << " 1 2 3" << endln;
			s << "equalDOF " << nodeCount + 1 << " "<< nodeCount + 4 << " 1 2 3" << endln;
		}
	}

	// 2.4 define pore pressure boundaries for nodes above water table
	s << "\n# 2.4 define pore pressure boundaries for nodes above water table" << endln << endln;
	if (!theModelType.compare("2D"))
	{ //2D

		for (int i = 0; i < dryNodes.size(); i++)
		{
			theSP = new SP_Constraint(dryNodes[i], 2, 0.0, true);
			theDomain->addSP_Constraint(theSP);
			s << "fix " << dryNodes[i] << " 0 0 1" << endln;
		}
	}
	else
	{ //3D

		for (int i = 0; i < dryNodes.size(); i++)
		{
			theSP = new SP_Constraint(dryNodes[i], 3, 0.0, true);
			theDomain->addSP_Constraint(theSP);
			s << "fix " << dryNodes[i] << " 0 0 0 1" << endln;
		}
	}

	// ----------------------------------------------
	// 3. create the materials for soils
	// ----------------------------------------------
	s << "\n\n";
	s << "# ----------------------------------------------\n";
	s << "# 3. create the materials for soils             \n";
	s << "# ----------------------------------------------\n\n";

	NDMaterial *theMat;
	SoilLayer theLayer;
	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		theLayer = (SRM_layering.getLayer(numLayers - layerCount - 2));
		//theMat = new J2CyclicBoundingSurface(numLayers - layerCount - 1, theLayer.getMatShearModulus(), theLayer.getMatBulkModulus(),
		//	theLayer.getSu(), theLayer.getRho(), theLayer.getMat_h() * theLayer.getMatShearModulus(), theLayer.getMat_m(), 0.0, 0.5);

		//theMat = new ElasticIsotropicMaterial(numLayers - layerCount - 1, 20000.0, 0.3, theLayer.getRho());

		/*
		double emax = 0.8;
		double emin = 0.5;
		double Dr = 0.463;
		double evoid = emax - Dr *(emax-emin) ;
		double Gs = 2.67;
		double rho_d = Gs / (1.0 + evoid);

		//nDmaterial PM4Sand $matTag $Dr $G0   $hpo   $Den <$patm $h0     $emax $emin  $nb  $nd  $Ado  $zmax $cz $  ce     $phic $nu $cgd $cdr $ckaf $Q $R $m $Fsed_min $p_sedo>
		//nDMaterial PM4Sand 3       $Dr 468.3 0.463 $rho_d 101.3 -1.00   $emax  $emin 0.5  0.1  -1.0  -1.0  250.0  -1.00  33.0  $nu
		theMat = new PM4Sand(numLayers - layerCount - 1, 0.463, 468.3, 0.45, rho_d, 
			101.3, -1.0, emax, emin, 0.5, 0.1, -1.0, -1.0, 250.0, -1.0, 33.0, 0.5/(1+0.5),
			2.0, -1.0, -1.0, 10.0, 1.5, 0.01, -1.0, -1.0,
			1, 1, 1e-4, 1e-4);
		*/
		std::string thisMatType = theLayer.getMatType();
		if (!thisMatType.compare("ElasticIsotropic"))
		{
			//nDMaterial ElasticIsotropic $matTag $E $v <$rho>
			double E = 2.0 * theLayer.getRho() * pow(theLayer.getShearVelocity(),2) * (1+0.3);
			theMat = new ElasticIsotropicMaterial(numLayers - layerCount - 1, E , 0.3, theLayer.getRho());
			s << "nDMaterial ElasticIsotropic " << numLayers - layerCount - 1 << " "<< E <<" " << " 0.3 "<<theLayer.getRho()<<endln;
		}
		else if (!thisMatType.compare("PM4Sand"))
		{
			double emax = 0.8;
			double emin = 0.5;
			double Gs = 2.67;
			double G0 = 500. ; //theLayer.getMatShearModulus();
			double rho_d = theLayer.getRho();
			double evoid = Gs / rho_d - 1;
			double Dr = 0.466; //(emax - evoid) / (emax - emin);
			double hpo = 0.45;

			/*
			theMat = new PM4Sand(numLayers - layerCount - 1, Dr, G0, hpo, rho_d);
			s << "nDMaterial PM4Sand " << numLayers - layerCount - 1 << " " << Dr << " "<<G0<<" "<<hpo<<" "<<rho_d<<endln;
			*/

			// use N10_T3
			
			if ((numLayers - layerCount - 1)==2)
			{
				theMat = new PM4Sand(numLayers - layerCount - 1,0.4662524041201569, 584.1, 0.450, 2.00594878429427, 101.3, -1.00,   0.8,  0.5, 0.5,  0.1,  -1.0,  -1.0,  250.0,  -1.00,  33.0,  0.3333333333333333);
				s << "nDMaterial PM4Sand " << numLayers - layerCount - 1 << " " << "0.4662524041201569 584.1 0.450 2.00594878429427 101.3 -1.00   0.8  0.5 0.5  0.1  -1.0  -1.0  250.0  -1.00  33.0  0.3333333333333333 " << endln;
			}
			else
			{
				theMat = new PM4Sand(numLayers - layerCount - 1, 0.4662524041201569, 468.3, 0.463, 1.6083133257878446, 101.3, -1.00, 0.8, 0.5, 0.5, 0.1, -1.0, -1.0, 250.0, -1.00,  33.0,  0.3333333333333333);
				s << "nDMaterial PM4Sand " << numLayers - layerCount - 1 << " " << "0.4662524041201569 468.3 0.463 1.6083133257878446 101.3 -1.00   0.8  0.5 0.5  0.1  -1.0  -1.0  250.0  -1.00  33.0  0.3333333333333333" << endln;
			}
			
				
			
		}

		OPS_addNDMaterial(theMat);

		if (PRINTDEBUG)
			opserr << "Material " << theLayer.getName().c_str() << " tag = " << numLayers - layerCount - 1 << endln;
	}

	// ------------------------------------------------------------
	// 4. create soil elements and add the material state parameter
	// ------------------------------------------------------------
	s << "\n";
	s << "# ------------------------------------------------------------\n";
	s << "# 4. create soil elements and add the material state parameter\n";
	s << "# ------------------------------------------------------------\n\n";

	Element *theEle;
	Parameter *theParameter;
	char **paramArgs = new char *[2];

	paramArgs[0] = new char[15];
	paramArgs[1] = new char[5];
	sprintf(paramArgs[0], "materialState");

	int nElem = 0;
	int node1Tag;

	std::map<int, int> matNumDict;
	std::vector<int> soilMatTags;

	for (int layerCount = 0; layerCount < numLayers - 1; ++layerCount)
	{
		theMat = OPS_getNDMaterial(numLayers - layerCount - 1);
		soilMatTags.push_back(theMat->getTag());
		for (int elemCount = 0; elemCount < layerNumElems[numLayers - layerCount - 2]; ++elemCount)
		{
			node1Tag = numNodesPerLayer * (nElem + elemCount);

			if (!theModelType.compare("2D"))
			{ //2D

				/*
				theEle = new SSPquad(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 4, node1Tag + 3,
				 *theMat, "PlaneStrain", 1.0, 0.0, -9.81 * theMat->getRho());
				 */

				//element SSPquadUP [expr ($nElemX)*($j+$count-1) + $k] $nI $nJ $nK $nL $i $thick($i) $uBulk($i) 1.0 1.0 1.0 $eInit($i) 0.0 $xWgt($i) $yWgt($i)
				double emax = 0.8;
				double emin = 0.5;
				double Dr = 0.463;
				double evoid = emax - Dr * (emax - emin);
				theEle = new SSPquadUP(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 4, node1Tag + 3,
									   *theMat, 1.0, 2.2e6, 1.0, 1.0, 1.0, evoid, 0.0, 0.0, -9.81 * 1.0); // -9.81 * theMat->getRho() TODO: theMat->getRho()
				s << "element SSPquadUP "<<nElem + elemCount + 1<<" " 
					<<node1Tag + 1 <<" "<<node1Tag + 2<<" "<< node1Tag + 4<<" "<< node1Tag + 3<<" "
					<< theMat->getTag() << " " << "1.0 2.2e6 1.0 1.0 1.0 " << " " <<evoid << " "<< " 0.0 0.0 "<< -9.81 * 1.0 << endln;
			}
			else
			{
				//3D
				/*
				theEle = new SSPbrick(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 3, node1Tag + 4, node1Tag + 5, 
				node1Tag + 6, node1Tag + 7, node1Tag + 8, *theMat, 0.0, -9.81 * theMat->getRho(), 0.0);
				*/
				/*
				theEle = new SSPbrickUP(nElem + elemCount + 1, node1Tag + 1, node1Tag + 2, node1Tag + 3, node1Tag + 4, node1Tag + 5, 
				node1Tag + 6, node1Tag + 7, node1Tag + 8, *theMat, 0.0, -9.81 * theMat->getRho(), 0.0);
				*/
				// TODO
			}

			theDomain->addElement(theEle);

			matNumDict[nElem + elemCount + 1] = theMat->getTag();
			
			theParameter = new Parameter(nElem + elemCount + 1, 0, 0, 0);
			sprintf(paramArgs[1], "%d", theMat->getTag());
			theEle->setParameter(const_cast<const char**>(paramArgs), 2, *theParameter);
			theDomain->addParameter(theParameter);
			

			if (PRINTDEBUG)
				opserr << "Element " << nElem + elemCount + 1 << ": Nodes = " << node1Tag + 1 << " to " << node1Tag + ((!theModelType.compare("2D")) ? 4 : 8) << "  - Mat tag = " << numLayers - layerCount - 1 << endln;
		}
		nElem += layerNumElems[numLayers - layerCount - 2];
	}

	if (PRINTDEBUG)
		opserr << "Total number of elements = " << nElem << endln;



	// ------------------------------------------------------------
	// 5. gravity analysis
	// ------------------------------------------------------------
	s << "\n";
	s << "# ------------------------------------------------------------\n";
	s << "# 5. gravity analysis                                         \n";
	s << "# ------------------------------------------------------------\n\n";

	// update material stage to consider elastic behavior
	ParameterIter &theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		theParameter->update(0.0);
	}
	s << endln;

	for (int i=0; i != soilMatTags.size(); i++)
		s << "updateMaterialStage -material "<< soilMatTags[i] <<" -stage 0" << endln ; 



	// 5.0 add recorders for gravity analysis
	s << "# 5.0 add recorders for gravity analysis\n\n";

	// create the output streams
	OPS_Stream *theOutputStream;
	Recorder *theRecorder;

	// record last node's results
	ID nodesToRecord(1);
	nodesToRecord(0) = numNodes;

	int dimDofToRecord = (!theModelType.compare("2D")) ? 3 : 4;
	ID dofToRecord(dimDofToRecord);
	dofToRecord(0) = 0;
	dofToRecord(1) = 1;
	dofToRecord(2) = 2;
	if (theModelType.compare("2D")) // 3D
		dofToRecord(3) = 3;

	std::string outFile = theOutputDir + PATH_SEPARATOR + "surface_grav.disp";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "disp", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	// 5.1 elastic gravity analysis (transient)
	s << "# 5.1 elastic gravity analysis (transient) \n\n";


	double gamma = 5./6.;
	double beta = 5./9.;

	s << "constraints Transformation" << endln;
	s << "test NormDispIncr 1.0e-4 35 1" << endln;
	s << "algorithm   Newton" << endln;
	s << "numberer RCM" << endln;
	s << "system BandGeneral" << endln;
	s << "set gamma " << gamma << endln;
	s << "set beta " << beta << endln;
	s << "integrator  Newmark $gamma $beta" << endln;
	s << "analysis Transient" << endln << endln;
	
	s << "set startT  [clock seconds]" << endln;
	s << "analyze     10 1.0" << endln;
	s << "puts \"Finished with elastic gravity analysis...\"" << endln << endln;

	// create analysis objects - I use static analysis for gravity
	AnalysisModel *theModel = new AnalysisModel();
	CTestNormDispIncr *theTest = new CTestNormDispIncr(1.0e-4, 35, 1);                    // 2. test NormDispIncr 1.0e-7 30 1
	EquiSolnAlgo *theSolnAlgo = new NewtonRaphson(*theTest);                              // 3. algorithm   Newton (TODO: another option: KrylovNewton) 
	//StaticIntegrator *theIntegrator = new LoadControl(0.05, 1, 0.05, 1.0); // *
	//ConstraintHandler *theHandler = new TransformationConstraintHandler(); // *
	TransientIntegrator* theIntegrator = new Newmark(5./6., 4./9.);// * Newmark(0.5, 0.25) // 6. integrator  Newmark $gamma $beta
	ConstraintHandler* theHandler = new PenaltyConstraintHandler(1.0e16, 1.0e16);          // 1. constraints Penalty 1.0e15 1.0e15
	RCM *theRCM = new RCM();
	DOF_Numberer *theNumberer = new DOF_Numberer(*theRCM);                                 // 4. numberer RCM (another option: Plain)
	BandGenLinSolver *theSolver = new BandGenLinLapackSolver();                            // 5. system BandGeneral (TODO: switch to SparseGeneral)
	LinearSOE *theSOE = new BandGenLinSOE(*theSolver);

	DirectIntegrationAnalysis* theAnalysis;												   // 7. analysis    Transient
	theAnalysis = new DirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	//VariableTimeStepDirectIntegrationAnalysis* theAnalysis;
	//theAnalysis = new VariableTimeStepDirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	//StaticAnalysis *theAnalysis; // *
	//theAnalysis = new StaticAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator); // *
	theAnalysis->setConvergenceTest(*theTest);

	int converged = theAnalysis->analyze(10,1.0); 
	if (!converged)
	{
		opserr << "Converged at time " << theDomain->getCurrentTime() << endln;
	} else
	{
		opserr << "Didn't converge at time " << theDomain->getCurrentTime() << endln;
	}
	opserr << "Finished with elastic gravity analysis..." << endln;

	
	// 5.2 plastic gravity analysis (transient)
	s << "# 5.2 plastic gravity analysis (transient)" << endln << endln;

	// update material response to plastic
	theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		theParameter->update(1.0);
		// //updateMaterialStage -material $i -stage 1.0
		//s << "updateMaterialStage -material "<< theParameter->getTag() <<" -stage 1" << endln ; 
	}
	s << endln;

	for (int i=0; i != soilMatTags.size(); i++)
		s << "updateMaterialStage -material "<< soilMatTags[i] <<" -stage 1" << endln ; 

	// add parameters: FirstCall for plastic gravity analysis
	sprintf(paramArgs[0], "FirstCall");
	ElementIter &theElementIterFC = theDomain->getElements();
	int nParaPlus = 0;
	while ((theEle = theElementIterFC()) != 0)
	{
		int theEleTag = theEle->getTag();
		theParameter = new Parameter(nElem + nParaPlus + 1, 0, 0, 0);
		sprintf(paramArgs[1], "%d", matNumDict[theEleTag]);
		theEle->setParameter(const_cast<const char**>(paramArgs), 2, *theParameter);
		theDomain->addParameter(theParameter);
		nParaPlus += 1;

		//setParameter -value 0 -ele $elementTag FirstCall $matTag
		s << "setParameter -value 0 -ele "<<theEleTag<<" FirstCall "<< matNumDict[theEleTag] << endln;
	}

	// add parameters: poissonRatio for plastic gravity analysis
	sprintf(paramArgs[0], "poissonRatio");
	ElementIter &theElementIter = theDomain->getElements();
	while ((theEle = theElementIter()) != 0)
	{
		int theEleTag = theEle->getTag();
		theParameter = new Parameter(nElem + nParaPlus + 1, 0, 0, 0);
		sprintf(paramArgs[1], "%d", matNumDict[theEleTag]);
		theEle->setParameter(const_cast<const char**>(paramArgs), 2, *theParameter);
		theDomain->addParameter(theParameter);
		nParaPlus += 1;

		//setParameter -value 0 -ele $elementTag poissonRatio $matTag
		s << "setParameter -value 0.3 -ele "<< theEleTag <<" poissonRatio "<< matNumDict[theEleTag] << endln;
	}
	//TODO: the $i ?  in setParameter -value 0.3 -eleRange $layerBound($i) $layerBound([expr $i+1]) poissonRatio $i

	// update FirstCall and poissonRatio
	theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		int paraTag = theParameter->getTag();
		if (paraTag>nElem & paraTag<=(nElem+nParaPlus/2.))
		{// FirstCall
			theParameter->update(0.0);
		}else if (paraTag>(nElem+nParaPlus/2.)){// poissonRatio
			theParameter->update(0.3);
		}
	}
	s << endln;

	converged = theAnalysis->analyze(10,1.0); 
	s << "analyze     10 1.0" << endln;
	if (!converged)
	{
		opserr << "Converged at time " << theDomain->getCurrentTime() << endln;
	} else
	{
		opserr << "Didn't converge at time " << theDomain->getCurrentTime() << endln;
	}
	opserr << "Finished with plastic gravity analysis..." endln;
	s << "puts \"Finished with plastic gravity analysis...\"" << endln << endln;
	


	// 5.3 update element permeability for post gravity analysis
	s << "# 5.3 update element permeability for post gravity analysis"<< endln << endln;

	// add parameters: hPerm for dynamic analysis
	sprintf(paramArgs[0], "hPerm");
	ElementIter &theElementIterhPerm = theDomain->getElements();
	while ((theEle = theElementIterhPerm()) != 0)
	{
		int theEleTag = theEle->getTag();
		theParameter = new Parameter(nElem + nParaPlus + 1, 0, 0, 0);
		sprintf(paramArgs[1], "%d", matNumDict[theEleTag]);
		theEle->setParameter(const_cast<const char**>(paramArgs), 2, *theParameter);
		theDomain->addParameter(theParameter);
		nParaPlus += 1;
	}

	// add parameters: vPerm for dynamic analysis
	sprintf(paramArgs[0], "vPerm");
	ElementIter &theElementItervPerm = theDomain->getElements();
	while ((theEle = theElementItervPerm()) != 0)
	{
		int theEleTag = theEle->getTag();
		theParameter = new Parameter(nElem + nParaPlus + 1, 0, 0, 0);
		sprintf(paramArgs[1], "%d", matNumDict[theEleTag]);
		theEle->setParameter(const_cast<const char**>(paramArgs), 2, *theParameter);
		theDomain->addParameter(theParameter);
		nParaPlus += 1;
	}

	// update hPerm and vPerm 
	theParamIter = theDomain->getParameters();
	while ((theParameter = theParamIter()) != 0)
	{
		int paraTag = theParameter->getTag();
		if (paraTag>(nElem+nParaPlus/2.) & paraTag<=(nElem+3.*nParaPlus/4.))
		{// hperm
			theParameter->update(1.0e-7/9.81/*TODO*/);

		}else if (paraTag>(nElem+3.*nParaPlus/4.)){// vPerm
			theParameter->update(1.0e-7/9.81/*TODO*/);
		}
	}

	theElementIter = theDomain->getElements();
	while ((theEle = theElementIter()) != 0)
	{
		int theEleTag = theEle->getTag();
		//setParameter -value 1 -ele $elementTag hPerm $matTag
		s << "setParameter -value "<<1.0e-7/9.81/*TODO*/<<" -ele "<< theEleTag<<" hPerm "<<endln;
		s << "setParameter -value "<<1.0e-7/9.81/*TODO*/<<" -ele "<< theEleTag<<" vPerm "<<endln;
	}
	s << endln;


	// ------------------------------------------------------------
	// 6. add the compliant base
	// ------------------------------------------------------------
	s << "\n";
	s << "# ------------------------------------------------------------\n";
	s << "# 6. add the compliant base                                   \n";
	s << "# ------------------------------------------------------------\n\n";

	// 6.1 get basic property of the base: use the last layer properties
	int dashMatTag = numLayers + 10; // TODO
	SoilLayer rockLayer = SRM_layering.getLayer(numLayers - 1);
	double rockDen = rockLayer.getRho();
	double rockVs = rockLayer.getShearVelocity();

	double colArea = sElemX * colThickness; 
	double dashpotCoeff = rockVs * rockDen; 

	double vis_C = dashpotCoeff * colArea;
	double cFactor = colArea * dashpotCoeff;

	int numberTheViscousMats = (!theModelType.compare("2D")) ? 1 : 2;
	UniaxialMaterial *theViscousMats[numberTheViscousMats];
	theViscousMats[0] = new ViscousMaterial(dashMatTag, vis_C, 1.0);
	OPS_addUniaxialMaterial(theViscousMats[0]);
	
	s << "set colArea " << colArea << endln; // [expr $sElemX*$thick(1)]
	s << "set dashpotCoeff  "<< dashpotCoeff << endln; // [expr $rockVS*$rockDen]
	s << "uniaxialMaterial Viscous " << dashMatTag <<" "<<"[expr $dashpotCoeff*$colArea] 1"<<endln;
	s << "set cFactor [expr $colArea*$dashpotCoeff]" << endln;

	if (theModelType.compare("2D")) // 3D
	{
		theViscousMats[1] = new ViscousMaterial(numLayers + 20, vis_C, 1.0);
		OPS_addUniaxialMaterial(theViscousMats[1]);
		// TODO: s << 
	}
	int numberDirections = (!theModelType.compare("2D")) ? 1 : 2;
	ID directions(numberDirections);
	directions(0) = 0;
	if (theModelType.compare("2D")) // 3D
		directions(1) = 2;

	// 6.2 create dashpot nodes and apply proper fixities
	
	if (!theModelType.compare("2D")) //2D
	{
		theNode = new Node(numNodes + 1, 2, 0.0, 0.0);
		theDomain->addNode(theNode); // TODO ?
		theNode = new Node(numNodes + 2, 2, 0.0, 0.0);
		theDomain->addNode(theNode); // TODO ?

		s << "model BasicBuilder -ndm 2 -ndf 2" << endln << endln; 
		s << "node " << numNodes + 1 << " 0.0 0.0" << endln;
		s << "node " << numNodes + 2 << " 0.0 0.0" << endln;

		theSP = new SP_Constraint(numNodes + 1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		

		s << "fix " << numNodes + 1 << " 1 1" << endln;

		theSP = new SP_Constraint(numNodes + 2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		s << "fix " << numNodes + 2 << " 0 1" << endln;
	}
	else
	{ //3D
		theNode = new Node(numNodes + 1, 3, 0.0, 0.0, 0.0, NULL);
		theDomain->addNode(theNode);
		theNode = new Node(numNodes + 2, 3, 0.0, 0.0, 0.0, NULL);
		theDomain->addNode(theNode);

		s << "model BasicBuilder -ndm 3 -ndf 3" << endln << endln; 
		s << "node " << numNodes + 1 << " 0.0 0.0 0.0" << endln;
		s << "node " << numNodes + 2 << " 0.0 0.0 0.0" << endln;

		theSP = new SP_Constraint(numNodes + 1, 0, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(numNodes + 1, 2, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		s << "fix " << numNodes + 1 << " 1 1 1" << endln;

		theSP = new SP_Constraint(numNodes + 2, 1, 0.0, true);
		theDomain->addSP_Constraint(theSP);

		s << "fix " << numNodes + 2 << " 0 1 0" << endln;
	}

	// 6.3 apply equalDOF to the node connected to the column
	int numConn = (!theModelType.compare("2D")) ? 1 : 2;
	Matrix Ccrconn(numConn, numConn);
	ID rcDOFconn(numConn);
	if (!theModelType.compare("2D")) //2D
	{
		Ccrconn(0, 0) = 1.0;
		rcDOFconn(0) = 0;
		theMP = new MP_Constraint(1, numNodes + 2, Ccrconn, rcDOFconn, rcDOFconn);
		theDomain->addMP_Constraint(theMP); //TODO

		s << "equalDOF " << 1 << " "<< numNodes + 2 << " 1" << endln;
	}
	else
	{ //3D
		Ccrconn(0, 0) = 1.0;
		Ccrconn(1, 1) = 1.0;
		rcDOFconn(0) = 0;
		rcDOFconn(1) = 1;
		theMP = new MP_Constraint(1, numNodes + 2, Ccrconn, rcDOFconn, rcDOFconn);
		theDomain->addMP_Constraint(theMP); //TODO

		s << "equalDOF " << 1 << " "<< numNodes + 2 << " 1 2" << endln; // TODO: num of motions
	}

	// 6.4 remove fixities created for gravity
	for (int i_remove = 0; i_remove < sizeTheSPtoRemove; i_remove++)
	{
		theSP = theDomain->removeSP_Constraint(theSPtoRemove(i_remove));
		delete theSP;
	}
	// TODO:
	s << "remove sp 1 1" << endln;
	s << "remove sp 2 1" << endln;

	// 6.5 equalDOF the first 4 nodes (3D) or 2 nodes (2D)
	int numMP1 = (!theModelType.compare("2D")) ? 1 : 2;
	Matrix constrainInXZ(numMP1, numMP1);
	ID constDOF(numMP1);
	if (!theModelType.compare("2D")) //2D
	{
		constrainInXZ(0, 0) = 1.0;
		constDOF(0) = 0;
		theMP = new MP_Constraint(1, 2, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);

		s << "equalDOF " << 1 << " "<< 2 << " 1 " << endln;
	}
	else //3D
	{
		constrainInXZ(0, 0) = 1.0;
		constrainInXZ(1, 1) = 1.0;
		constDOF(0) = 0;
		constDOF(1) = 2;
		theMP = new MP_Constraint(1, 2, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
		theMP = new MP_Constraint(1, 3, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);
		theMP = new MP_Constraint(1, 4, constrainInXZ, constDOF, constDOF);
		theDomain->addMP_Constraint(theMP);

		s << "equalDOF 1 2 1 2" << endln;
		s << "equalDOF 1 3 1 2" << endln;
		s << "equalDOF 1 4 1 2" << endln;
	}

	// 6.6 create the dashpot element
	Vector x(3);
	Vector y(3);
	if (!theModelType.compare("2D")) //2D
	{
		x(0) = 1.0;
		x(1) = 0.0;
		x(2) = 0.0;
		y(1) = 1.0;
		y(0) = 0.0;
		y(2) = 0.0;
		//element zeroLength [expr $nElemT+1]  $dashF $dashS -mat [expr $numLayers+1]  -dir 1
		theEle = new ZeroLength(numElems + 1, 2, numNodes + 1, numNodes + 2, x, y, 1, theViscousMats, directions); //TODO ?
		s << "element zeroLength "<<numElems + 1 <<" "<< numNodes + 1 <<" "<< numNodes + 2<<" -mat "<<dashMatTag<<"  -dir 1" << endln;
	}
	else
	{ //3D
		x(0) = 1.0;
		x(1) = 0.0;
		x(2) = 0.0;
		y(1) = 1.0;
		y(0) = 0.0;
		y(2) = 0.0;
		theEle = new ZeroLength(numElems + 1, 3, numNodes + 1, numNodes + 2, x, y, 2, theViscousMats, directions); //TODO ?
		// TODO: s << 
	}
	theDomain->addElement(theEle);

	s << "\n\n";





	// ------------------------------------------------------------
	// 7. dynamic analysis
	// ------------------------------------------------------------
	s << "\n";
	s << "# ------------------------------------------------------------\n";
	s << "# 7. dynamic analysis                                         \n";
	s << "# ------------------------------------------------------------\n\n";

	s << "setTime 0.0" << endln;
	s << "wipeAnalysis" << endln;
	s << "remove recorders" << endln << endln;

	// ------------------------------------------------------------
	// 7.1 apply the motion
	// ------------------------------------------------------------
	int numSteps = 0;
	std::vector<double> dt;

	s << "model BasicBuilder -ndm 2 -ndf 3" << endln;

	// using multiple support
	//MultiSupportPattern* theLP = new MultiSupportPattern(1);
	//theLP->addMotion(*theMotionX->getGroundMotion(), 1);
	//theLP->addSP_Constraint(new ImposedMotionSP(1, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(2, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(3, 0, 1, 1));
	//theLP->addSP_Constraint(new ImposedMotionSP(4, 0, 1, 1));

	// using uniform excitation
	// LoadPattern* theLP = new UniformExcitation(*theMotion, 1, 1, 0.0, 1.0);
	//theDomain->addLoadPattern(theLP);

	double dT = 0.0001; // This is the time step in solution
	double motionDT =  0.005; // This is the time step in the motion record. TODO: use a funciton to get it
	int nSteps = 1998; // number of motions in the record. TODO: use a funciton to get it
	int remStep = nSteps * motionDT / dT;

	s << "set dT " << dT << endln;
	s << "set motionDT " << motionDT << endln;
	s << "set mSeries \"Path -dt $motionDT -filePath /Users/simcenter/Codes/SimCenter/SiteResponseTool/test/RSN766_G02_000_VEL.txt -factor $cFactor\""<<endln;
	// using a stress input with the dashpot
	if (theMotionX->isInitialized())
	{
		LoadPattern *theLP = new LoadPattern(1, vis_C);
		theLP->setTimeSeries(theMotionX->getVelSeries());

		NodalLoad *theLoad;
		int numLoads = (!theModelType.compare("2D")) ? 3 : 4;
		Vector load(numLoads);
		load(0) = 1.0;
		load(1) = 0.0;
		load(2) = 0.0;
		if (theModelType.compare("2D")) // 3D
		{
			load(3) = 0.0;
		}

		//theLoad = new NodalLoad(1, numNodes + 2, load, false); theLP->addNodalLoad(theLoad);
		theLoad = new NodalLoad(1, 1, load, false);
		theLP->addNodalLoad(theLoad);
		theDomain->addLoadPattern(theLP);

		s << "pattern Plain 10 $mSeries {"<<endln;
		s << "    load 1  1.0 0.0 0.0" << endln;
		s << "}" << endln << endln;

		// update the number of steps as well as the dt vector
		int temp = theMotionX->getNumSteps();
		if (temp > numSteps)
		{
			numSteps = temp;
			dt = theMotionX->getDTvector();
		}
	}

	if (theModelType.compare("2D")) // 3D TODO
	{
		if (theMotionZ->isInitialized())
		{
			LoadPattern *theLP = new LoadPattern(2, vis_C);
			theLP->setTimeSeries(theMotionZ->getVelSeries());

			NodalLoad *theLoad;
			Vector load(3);
			load(0) = 0.0;
			load(1) = 0.0;
			load(2) = 1.0;

			theLoad = new NodalLoad(2, numNodes + 2, load, false);
			theLP->addNodalLoad(theLoad);
			theDomain->addLoadPattern(theLP);

			int temp = theMotionZ->getNumSteps();
			if (temp > numSteps)
			{
				numSteps = temp;
				dt = theMotionZ->getDTvector();
			}
		}
	}

	// ------------------------------------------------------------
	// 7.2 define the analysis
	// ------------------------------------------------------------

	// I have to change to a transient analysis
	// remove the static analysis and create new transient objects
	delete theIntegrator;
	delete theAnalysis;

	//theTest->setTolerance(1.0e-5);

	s << "constraints Transformation" << endln; 
	s << "test NormDispIncr 1.0e-4 35 0" << endln; // TODO
	s << "algorithm   Newton" << endln;
	s << "numberer    RCM" << endln;
	s << "system BandGeneral" << endln;

	double gamma_dynm = 0.5;
	double beta_dynm = 0.25;
	TransientIntegrator* theTransientIntegrator = new Newmark(gamma_dynm, beta_dynm);// * Newmark(0.5, 0.25) // 6. integrator  Newmark $gamma $beta



	// setup Rayleigh damping   TODO: calcualtion of these paras
	// apply 2% at the natural frequency and 5*natural frequency
	double natFreq = SRM_layering.getNaturalPeriod();
	double pi = 4.0 * atan(1.0);

	/*
	double dampRatio = 0.02;
	double a0 = dampRatio * (10.0 * pi * natFreq) / 3.0;
	double a1 = dampRatio / (6.0 * pi * natFreq);
	*/

	// method in N10_T3 
	double fmin = 5.01;
	double Omegamin  = fmin * 2.0 * pi;
	double ximin = 0.025;
	double a0 = ximin * Omegamin; //# factor to mass matrix
	double a1 = ximin / Omegamin; //# factor to stiffness matrix

	if (PRINTDEBUG)
	{
		opserr << "f1 = " << natFreq << "    f2 = " << 5.0 * natFreq << endln;
		opserr << "a0 = " << a0 << "    a1 = " << a1 << endln;
	}
	theDomain->setRayleighDampingFactors(a0, a1, 0.0, 0.0);


	DirectIntegrationAnalysis* theTransientAnalysis;
	theTransientAnalysis = new DirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theTransientIntegrator, theTest);

	//VariableTimeStepDirectIntegrationAnalysis *theTransientAnalysis;
	//theTransientAnalysis = new VariableTimeStepDirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theTransientIntegrator, theTest);

	// reset time in the domain
	theDomain->setCurrentTime(0.0);

	s << "set gamma_dynm " << gamma_dynm << endln;
	s << "set beta_dynm " << beta_dynm << endln;
	s << "integrator  Newmark $gamma_dynm $beta_dynm" << endln;
	s << "set a0 " << a0 << endln;
	s << "set a1 " << a1 << endln;
	s << "rayleigh    $a0 $a1 0.0 0.0" << endln;
	//s << "analysis Transient" << endln << endln;
	s << "analysis Transient" << endln << endln;
	

	// ------------------------------------------------------------
	// 7.3 define outputs and recorders
	// ------------------------------------------------------------

	// record the response at the surface
	outFile = theOutputDir + PATH_SEPARATOR + "surface.acc";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "accel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "surface.vel";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "vel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "surface.disp";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "disp", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);






	s<< "eval \"recorder Node -file out/surface_tcl.disp -time -dT $motionDT -node "<<nodesToRecord[0]<<" -dof 1 2 3  disp\""<<endln;// 1 2
	s<< "eval \"recorder Node -file out/surface_tcl.acc -time -dT $motionDT -node "<<nodesToRecord[0]<<" -dof 1 2 3  accel\""<<endln;// 1 2
	s<< "eval \"recorder Node -file out/surface_tcl.vel -time -dT $motionDT -node "<<nodesToRecord[0]<<" -dof 1 2 3 vel\""<<endln;// 3

	s<< "eval \"recorder Node -file out/base_tcl.disp -time -dT $motionDT -node 1 -dof 1 2 3  disp\""<<endln;// 1 2
	s<< "eval \"recorder Node -file out/base_tcl.acc -time -dT $motionDT -node 1 -dof 1 2 3  accel\""<<endln;// 1 2
	s<< "eval \"recorder Node -file out/base_tcl.vel -time -dT $motionDT -node 1 -dof 1 2 3 vel\""<<endln;// 3
	s<< "eval \"recorder Node -file out/pwpLiq_tcl.out -time -dT $motionDT -node 17 -dof 3 vel\""<<endln;

	s<< "recorder Element -file out/stress_tcl.out -time -dT $motionDT  -eleRange 1 "<<numNodes<<"  stress 3"<<endln;
	s<< "recorder Element -file out/strain_tcl.out -time -dT $motionDT  -eleRange 1 "<<numNodes<<"  strain"<<endln;


	s<< endln << endln;
	
	// record the response of base node
	nodesToRecord(0) = 1;
	
	dofToRecord.resize(1);
	dofToRecord(0) = 0; // only record the x dof

	outFile = theOutputDir + PATH_SEPARATOR + "base.acc";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "accel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "base.vel";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "vel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	outFile = theOutputDir + PATH_SEPARATOR + "base.disp";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &nodesToRecord, 0, "disp", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);

	dofToRecord.resize(1);
	dofToRecord(0) = 2; // only record the pore pressure dof
	ID pwpNodesToRecord(1);
	pwpNodesToRecord(0) = 17;
	outFile = theOutputDir + PATH_SEPARATOR + "pwpLiq.out";
	theOutputStream = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new NodeRecorder(dofToRecord, &pwpNodesToRecord, 0, "vel", *theDomain, *theOutputStream, 0.0, true, NULL);
	theDomain->addRecorder(*theRecorder);




	// record element results
	OPS_Stream* theOutputStream2;
	ElementIter &theElementIterh = theDomain->getElements();
	std::vector<int> quadElem;
	while ((theEle = theElementIterh()) != 0)
	{
		int theEleTag = theEle->getTag();
		if (theEle->getNumDOF() == 12) // quad ele
			quadElem.push_back(theEleTag);
	}
	ID elemsToRecord(quadElem.size());
	for (int i=0;i<quadElem.size();i+=1)
		elemsToRecord(i) = quadElem[i];
	const char* eleArgs = "stress";
	outFile = theOutputDir + PATH_SEPARATOR + "stress.out";
	theOutputStream2 = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new ElementRecorder(&elemsToRecord, &eleArgs, 1, true, *theDomain, *theOutputStream2, 0.0, NULL);
	theDomain->addRecorder(*theRecorder);

	const char* eleArgsStrain = "strain";
	outFile = theOutputDir + PATH_SEPARATOR + "strain.out";
	theOutputStream2 = new DataFileStream(outFile.c_str(), OVERWRITE, 2, 0, false, 6, false);
	theRecorder = new ElementRecorder(&elemsToRecord, &eleArgsStrain, 1, true, *theDomain, *theOutputStream2, 0.0, NULL);
	theDomain->addRecorder(*theRecorder);


	s << endln;
	s << "print -file out/Domain_tcl.out" << endln << endln;

	//return 0;

	// ------------------------------------------------------------
	// 7.2 perform dynamic analysis
	// ------------------------------------------------------------

	s << "set nSteps " << nSteps << endln;
	s << "set remStep " << remStep << endln;
	s << "set success 0" << endln << endln;

	s << "proc subStepAnalyze {dT subStep} {" << endln;
	s << "	if {$subStep > 10} {" << endln;
	s << "		return -10" << endln;
	s << "	}" << endln;
	s << "	for {set i 1} {$i < 3} {incr i} {" << endln;
	s << "		puts \"Try dT = $dT\"" << endln;
	s << "		set success [analyze 1 $dT]" << endln;
	s << "		if {$success != 0} {" << endln;
	s << "			set success [subStepAnalyze [expr $dT/2.0] [expr $subStep+1]]" << endln;
	s << "			if {$success == -10} {" << endln;
	s << "				puts \"Did not converge.\"" << endln;
	s << "				return $success" << endln;
	s << "			}" << endln;
	s << "		} else {" << endln;
	s << "			if {$i==1} {" << endln;
	s << "				puts \"Substep $subStep : Left side converged with dT = $dT\"" << endln;
	s << "			} else {" << endln;
	s << "				puts \"Substep $subStep : Right side converged with dT = $dT\"" << endln;
	s << "			}" << endln;
	s << "		}" << endln;
	s << "	}" << endln;
	s << "	return $success" << endln;
	s << "}" << endln << endln << endln;


	// solution 1: direct steps 
	/*
	s << "set thisStep 0"<<endln;
	s << "set success 0"<<endln;
	s << "while {$thisStep < 1998} {"<<endln;
	s << "    set thisStep [expr $thisStep+1]"<<endln;
	s << "    set success [analyze 1 $dT [expr $dT/2.0] [expr $dT*2.0] 1]"<<endln;
	s << "    if {$success == 0} {;# success"<<endln;
	s << "        puts \"Analysis Finished at step: $thisStep\""<<endln;
	s << "    } else {"<<endln;
	s << "        puts \"Analysis Failed at step: $thisStep ----------------------------------------------!!!\""<<endln;
	s << "    }"<<endln;
	s << "}"<<endln<<endln; 
	s << "wipe"<<endln;
	s << "exit"<<endln<< endln <<endln;
	*/




	s << "puts \"Start analysis\"" << endln;
	s << "set startT [clock seconds]" << endln;
	s << "while {$success != -10} {" << endln;
	s << "	set subStep 0" << endln;
	s << "	set success [analyze $remStep  $dT]" << endln;
	s << "	if {$success == 0} {" << endln;
	s << "		puts \"Analysis Finished\"" << endln;
	s << "		break" << endln;
	s << "	} else {" << endln;
	s << "		set curTime  [getTime]" << endln;
	s << "		puts \"Analysis failed at $curTime . Try substepping.\"" << endln;
	s << "		set success  [subStepAnalyze [expr $dT/2.0] [incr subStep]]" << endln;
	s << "		set curStep  [expr int($curTime/$dT + 1)]" << endln;
	s << "		set remStep  [expr int($nSteps-$curStep)]" << endln;
	s << "		puts \"Current step: $curStep , Remaining steps: $remStep\"" << endln;
	s << "	}" << endln;
	s << "}" << endln << endln;
	s << "set endT [clock seconds]" << endln << endln;
	s << "puts \"loading analysis execution time: [expr $endT-$startT] seconds.\"" << endln << endln;
	s << "puts \"Finished with dynamic analysis...\"" << endln << endln;

	//s << "print -file out/Domain_tcl.out "<< endln<<endln;
	
	s << "wipe" << endln;
	s << "\n" << endln;


	s.close();
	

	OPS_Stream *theOutputStreamAll;
	theOutputStreamAll = new DataFileStream("out/Domain.out", OVERWRITE, 2, 0, false, 6, false);
	theDomain->Print(*theOutputStreamAll);
	opserr << theOutputStreamAll;


	double totalTime = dT * nSteps;
	int success = 0;
	


	
	opserr << "Analysis started:" << endln;
	std::stringstream progressBar;
	while (success != -10) {
		double subStep = 0;
		int success = theTransientAnalysis->analyze(remStep, dT);// 0 = success
		if (success>-1.e-7 & success<1.e-7) {
			int currentStep = nSteps - remStep;
			opserr << "Analysis Finished at time " << theDomain->getCurrentTime() << endln;

			break;
		} else {
			double currentTime = theDomain->getCurrentTime();
			opserr << "Analysis Failed at time " << currentTime << endln;
			success = subStepAnalyze(int(dT/2), subStep +1, success, remStep, theTransientAnalysis);
			int curStep = int(currentTime/dT+1);
			remStep = int(nSteps-curStep);
			opserr << "Current step: "<<curStep<<" , Remaining steps: " << remStep << endln;

			progressBar << "\r[";
			for (int ii = 0; ii < ((int)(20 * curStep / numSteps)-1); ii++)
				progressBar << "-";
			progressBar << " 🚌  ";
			for (int ii = (int)(20 * curStep / numSteps)+1; ii < 20; ii++)
				progressBar << ".";

			progressBar << "]  " << (int)(100 * curStep / numSteps) << "%";
			opsout << progressBar.str().c_str();
			opsout.flush();
		}

	}

	opserr << "Site response analysis done..." << endln;
	progressBar << "\r[";
	for (int ii = 0; ii < 20; ii++)
		progressBar << "-";

	progressBar << "]  🚌   100%\n";
	opsout << progressBar.str().c_str();
	opsout.flush();
	opsout << endln;
	


	
	/*
	opserr << "Analysis started:" << endln;
	std::stringstream progressBar;
	for (int analysisCount = 0; analysisCount < remStep; ++analysisCount)
	{
		//int converged = theAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		double stepDT = dt[analysisCount];
		//int converged = theTransientAnalysis->analyze(1, stepDT, stepDT / 2.0, stepDT * 2.0, 1); // *
		//int converged = theTransientAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		int converged = theTransientAnalysis->analyze(1, dT);
		if (!converged)
		{
			opserr << "Converged at time " << theDomain->getCurrentTime() << endln;

			if (analysisCount % (int)(remStep / 20) == 0)
			{
				progressBar << "\r[";
				for (int ii = 0; ii < ((int)(20 * analysisCount / remStep)-1); ii++)
					progressBar << "-";
				progressBar << " 🚌  ";
				for (int ii = (int)(20 * analysisCount / remStep)+1; ii < 20; ii++)
					progressBar << ".";

				progressBar << "]  " << (int)(100 * analysisCount / remStep) << "%";
				opsout << progressBar.str().c_str();
				opsout.flush();
			}
		}
		else
		{
			opserr << "Site response analysis did not converge." << endln;
			exit(-1);
		}
	}
	opserr << "Site response analysis done..." << endln;
	progressBar << "\r[";
	for (int ii = 0; ii < 20; ii++)
		progressBar << "-";

	progressBar << "]  🚌   100%\n";
	opsout << progressBar.str().c_str();
	opsout.flush();
	opsout << endln;
	*/

	

	//if (PRINTDEBUG)
	//{
	//	Information info;
	//	theEle = theDomain->getElement(1);
	//	theEle->getResponse(1, info);
	//	opserr << "Stress = " << info.getData();
	//	theEle->getResponse(2, info);
	//	opserr << "Strain = " << info.getData();
	//}

	//int count = 0;
	//NodeIter& theNodeIter = theDomain->getNodes();
	//Node * thisNode;
	//while ((thisNode = theNodeIter()) != 0)
	//{
	//	count++;
	//	opserr << "Node " << thisNode->getTag() << " = " << thisNode->getCrds() << endln;
	//}

	//int count = 0;
	//ElementIter& theEleIter = theDomain->getElements();
	//Element * thisEle;
	//while ((thisEle = theEleIter()) != 0)
	//{
	//	count++;
	//	opserr << "Element " << thisEle->getTag() << " = " << thisEle->getExternalNodes() << endln;
	//}



	return 0;



}




int SiteResponseModel::subStepAnalyze(double dT, int subStep, int success, int remStep, DirectIntegrationAnalysis* theTransientAnalysis)
{
	if (subStep > 10)
		return -10;
	for (int i; i < 3; i++)
	{
		opserr << "Try dT = " << dT << endln;
		success = theTransientAnalysis->analyze(remStep, dT);// 0 means success
		//success = subStepAnalyze(int(dT/2), subStep +1, success);
	}
	
	return 0;

}
/*
proc subStepAnalyze {dT subStep} {
	if {$subStep > 10} {
		return -10
	}
	for {set i 1} {$i < 3} {incr i} {
		puts "Try dT = $dT"
		set success [analyze 1 $dT]
		if {$success != 0} {
			set success [subStepAnalyze [expr $dT/2.0] [expr $subStep+1]]
			if {$success == -10} {
				puts "Did not converge."
				return $success
			}
		} else {
			if {$i==1} {
				puts "Substep $subStep : Left side converged with dT = $dT"
			} else {
				puts "Substep $subStep : Right side converged with dT = $dT"
			}
		}
	}
	return $success
}

*/


int SiteResponseModel::runTestModel()
{
	Vector zeroVec(3);
	zeroVec.Zero();

	Node *theNode;

	theNode = new Node(1, 3, 0.0, 0.0, 0.0);
	theDomain->addNode(theNode);
	theNode = new Node(2, 3, 1.0, 0.0, 0.0);
	theDomain->addNode(theNode);
	theNode = new Node(3, 3, 1.0, 1.0, 0.0);
	theDomain->addNode(theNode);
	theNode = new Node(4, 3, 0.0, 1.0, 0.0);
	theDomain->addNode(theNode);
	theNode = new Node(5, 3, 0.0, 0.0, 1.0);
	theDomain->addNode(theNode);
	theNode = new Node(6, 3, 1.0, 0.0, 1.0);
	theDomain->addNode(theNode);
	theNode = new Node(7, 3, 1.0, 1.0, 1.0);
	theDomain->addNode(theNode);
	theNode = new Node(8, 3, 0.0, 1.0, 1.0);
	theDomain->addNode(theNode);

	SP_Constraint *theSP;
	for (int counter = 0; counter < 3; ++counter)
	{
		theSP = new SP_Constraint(1, counter, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(2, counter, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(3, counter, 0.0, true);
		theDomain->addSP_Constraint(theSP);
		theSP = new SP_Constraint(4, counter, 0.0, true);
		theDomain->addSP_Constraint(theSP);
	}

	MP_Constraint *theMP;
	Matrix Ccr(3, 3);
	Ccr(0, 0) = 1.0;
	Ccr(1, 1) = 1.0;
	Ccr(2, 2) = 1.0;
	ID rcDOF(3);
	rcDOF(0) = 0;
	rcDOF(1) = 1;
	rcDOF(2) = 2;
	theMP = new MP_Constraint(5, 6, Ccr, rcDOF, rcDOF);
	theDomain->addMP_Constraint(theMP);
	theMP = new MP_Constraint(5, 7, Ccr, rcDOF, rcDOF);
	theDomain->addMP_Constraint(theMP);
	theMP = new MP_Constraint(5, 8, Ccr, rcDOF, rcDOF);
	theDomain->addMP_Constraint(theMP);

	NDMaterial *theMat;
	theMat = new J2CyclicBoundingSurface(1, 20000.0, 25000.0, 100.0, 0.0, 20000.0, 1.0, 0.0, 0.5);
	OPS_addNDMaterial(theMat);

	Element *theEle;
	theMat = OPS_getNDMaterial(1);
	theEle = new SSPbrick(1, 1, 2, 3, 4, 5, 6, 7, 8, *theMat, 0.0, 0.0, 0.0);
	theDomain->addElement(theEle);
	//theEle = new Brick(1, 1, 2, 3, 4, 5, 6, 7, 8, *theMat, 0.0, 0.0, 0.0); theDomain->addElement(theEle);

	//LinearSeries* theTS_disp;
	//theTS_disp = new LinearSeries(1, 1.0);

	Vector theTime(3);
	theTime(0) = 0.0;
	theTime(1) = 1.0;
	theTime(2) = 100.0;

	Vector theValue_Disp(3);
	theValue_Disp(0) = 0.0;
	theValue_Disp(1) = 1.0;
	theValue_Disp(2) = 1.0;

	Vector theValue_Vel(3);
	theValue_Vel(0) = 1.0;
	theValue_Vel(1) = 1.0;
	theValue_Vel(2) = 1.0;

	Vector theValue_Acc(3);
	theValue_Acc(0) = 0.0;
	theValue_Acc(1) = 0.0;
	theValue_Acc(2) = 0.0;
	PathTimeSeries *theTS_disp = new PathTimeSeries(1, theValue_Disp, theTime, 1.0, true);
	//PathTimeSeries* theTS_disp = NULL;
	PathTimeSeries *theTS_vel = new PathTimeSeries(1, theValue_Vel, theTime, 1.0, true);
	//PathTimeSeries* theTS_vel  = NULL;
	PathTimeSeries *theTS_acc = new PathTimeSeries(1, theValue_Acc, theTime, 1.0, true);
	//PathTimeSeries* theTS_acc = NULL;

	//LoadPattern* theLP;
	//theLP = new LoadPattern(1);
	//theLP->setTimeSeries(theTS_disp);

	MultiSupportPattern *theLP = new MultiSupportPattern(1);
	//theLP->setTimeSeries(theTS_disp);

	//NodalLoad* theLoad;
	//Vector load(3);
	//load(0) = 1.0;
	//load(1) = 0.0;
	//load(2) = 0.0;
	//theLoad = new NodalLoad(1, 5, load, false); theLP->addNodalLoad(theLoad);
	//theLoad = new NodalLoad(2, 6, load, false); theLP->addNodalLoad(theLoad);
	//theLoad = new NodalLoad(3, 7, load, false); theLP->addNodalLoad(theLoad);
	//theLoad = new NodalLoad(4, 8, load, false); theLP->addNodalLoad(theLoad);
	GroundMotion *theMotion = new GroundMotion(theTS_disp, theTS_vel, theTS_acc);
	theLP->addMotion(*theMotion, 1);

	theLP->addSP_Constraint(new ImposedMotionSP(5, 0, 1, 1));
	theLP->addSP_Constraint(new ImposedMotionSP(5, 0, 1, 1));
	theLP->addSP_Constraint(new ImposedMotionSP(5, 0, 1, 1));
	theLP->addSP_Constraint(new ImposedMotionSP(5, 0, 1, 1));

	//theLP->addSP_Constraint(new SP_Constraint(5, 0, 1.0, false));
	//theLP->addSP_Constraint(new SP_Constraint(6, 0, 1.0, false));
	//theLP->addSP_Constraint(new SP_Constraint(7, 0, 1.0, false));
	//theLP->addSP_Constraint(new SP_Constraint(8, 0, 1.0, false));
	theDomain->addLoadPattern(theLP);

	AnalysisModel *theModel = new AnalysisModel();
	CTestNormDispIncr *theTest = new CTestNormDispIncr(1.0e-7, 30, 1);
	EquiSolnAlgo *theSolnAlgo = new NewtonRaphson(*theTest);
	//StaticIntegrator* theIntegrator    = new LoadControl(0.05, 1, 0.05, 1.0);
	TransientIntegrator *theIntegrator = new Newmark(0.5, 0.25);
	ConstraintHandler *theHandler = new PenaltyConstraintHandler(1.0e15, 1.0e15);
	RCM *theRCM = new RCM();
	DOF_Numberer *theNumberer = new DOF_Numberer(*theRCM);
	BandGenLinSolver *theSolver = new BandGenLinLapackSolver();
	LinearSOE *theSOE = new BandGenLinSOE(*theSolver);

	//DirectIntegrationAnalysis* theAnalysis;
	//theAnalysis = new DirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	VariableTimeStepDirectIntegrationAnalysis *theAnalysis;
	theAnalysis = new VariableTimeStepDirectIntegrationAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator, theTest);

	//StaticAnalysis *theAnalysis;
	//theAnalysis = new StaticAnalysis(*theDomain, *theHandler, *theNumberer, *theModel, *theSolnAlgo, *theSOE, *theIntegrator);
	//theAnalysis->setConvergenceTest(*theTest);

	for (int analysisCount = 0; analysisCount < 15; ++analysisCount)
	{
		int converged = theAnalysis->analyze(1, 0.01, 0.005, 0.02, 1);
		if (!converged)
		{

			opserr << "Converged at time " << theDomain->getCurrentTime() << endln;

			opserr << "Disp = " << theDomain->getNode(5)->getDisp()(0);
			opserr << ", Vel = " << theDomain->getNode(5)->getTrialVel()(0);
			opserr << ", acc = " << theDomain->getNode(5)->getTrialAccel()(0) << endln;

			opserr << "From the ground motion: " << endln;
			opserr << "Disp = " << theMotion->getDisp(theDomain->getCurrentTime());
			opserr << ", Vel = " << theMotion->getVel(theDomain->getCurrentTime());
			opserr << ", acc = " << theMotion->getAccel(theDomain->getCurrentTime()) << endln;
		}
	}

	Information info;
	theEle->getResponse(1, info);
	opserr << "Stress = " << info.getData();
	theEle->getResponse(2, info);
	opserr << "Strain = " << info.getData();

	return 0;
}
