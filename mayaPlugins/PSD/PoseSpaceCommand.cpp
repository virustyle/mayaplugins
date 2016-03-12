#include "PoseSpaceCommand.h"
#include "PoseSpaceDeformer.h"
#include "utils.h"

#include <map>

#include <maya/MGlobal.h>
#include <maya/MMatrix.h>
#include <maya/MFnMatrixData.h>
#include <maya/MFnIntArrayData.h>
#include <maya/MFnVectorArrayData.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnMesh.h>
#include <maya/MFnSkinCluster.h>
#include <maya/MFnGeometryFilter.h>
#include <maya/MDagPath.h>
#include <maya/MArgDatabase.h>
#include <maya/MItDependencyGraph.h>
#include <maya/MIntArray.h>
#include <maya/MPointArray.h>


#define SFLAG_CREATE     "c"
#define LFLAG_CREATE     "create"
#define SFLAG_ADDPOSE    "ap"
#define LFLAG_ADDPOSE    "addPose"


#define MATCHARG(str, shortName, longName) \
        (((str)==(shortName))||((str)==(longName)))



const char * PoseSpaceCommand::name(PluginNames::PoseSpaceCommand);


typedef std::map<int, MMatrix>      JointMatrixMap;


void* PoseSpaceCommand::creator()
{
    return new PoseSpaceCommand;
}

MSyntax PoseSpaceCommand::cmdSyntax()
{
    MStatus stat;
    MSyntax syntax;

    syntax.addFlag(SFLAG_CREATE, LFLAG_CREATE, MSyntax::kString);
    syntax.addFlag(SFLAG_ADDPOSE, LFLAG_ADDPOSE, MSyntax::kString);
    syntax.makeFlagMultiUse(SFLAG_ADDPOSE);

    syntax.enableQuery(false);
    syntax.useSelectionAsDefault(true);
    syntax.setObjectType(MSyntax::kSelectionList);

    return syntax;
}

MStatus PoseSpaceCommand::parseArgs( const MArgList& args )
{
    MStatus stat;

    MArgDatabase argDB(syntax(), args, &stat);
    MCheckStatus(stat, ErrorStr::FailedToParseArgs);

    // Get selection
    stat = argDB.getObjects( _selList );
    MCheckStatus(stat, ErrorStr::FailedToParseArgs);


    if (argDB.isFlagSet(LFLAG_CREATE))
    {
        _operation = LFLAG_CREATE;

        stat = argDB.getFlagArgument(LFLAG_CREATE, 0, _poseSpaceDeformer);
        MCheckStatus(stat, ErrorStr::FailedToParseArgs);
    }
    else if (argDB.isFlagSet(LFLAG_ADDPOSE))
    {
        _operation = LFLAG_ADDPOSE;

        for (uint i = 0; i < argDB.numberOfFlagUses(LFLAG_ADDPOSE); i++)
        {
            MArgList argList;
            stat = argDB.getFlagArgumentList(LFLAG_ADDPOSE, i, argList);
            MCheckStatus(stat, ErrorStr::FailedToParseArgs);

            MString name = argList.asString(0, &stat);
            
            // Get PSD node and jointss
            MSelectionList sel;
            sel.add(name);
            MObject obj;
            sel.getDependNode(0, obj);
            MFnDependencyNode fnDep(obj);
            if (fnDep.typeId() == PoseSpaceDeformer::id)
                _poseSpaceDeformer = name;
            else if (obj.apiType() == MFn::kJoint)
                _joints.append(name);
        }
        return MS::kSuccess;
    }

    return MS::kSuccess;
}

MStatus PoseSpaceCommand::doIt( const MArgList& args )
{
    MStatus stat;
    MString msg;

    stat = parseArgs(args);
    MCheckStatus(stat, "");

    if (_operation == LFLAG_CREATE)
    {
        stat = create();
    }
    else if (_operation == LFLAG_ADDPOSE)
    {
        stat = addPose();        
    }

    return stat;
}

// Get mesh name from selection list
MStatus PoseSpaceCommand::getMeshFromSelList(MObject& meshObj)
{
    MStatus stat;

    for( int i=0; i < _selList.length(); ++i )
    {
        MObject obj;
        stat = _selList.getDependNode(i, obj);
        MFnDependencyNode fnDep(obj, &stat);

        if (obj.hasFn(MFn::kMesh))
        {
            meshObj = obj;
            return MS::kSuccess;
        }
        else if (obj.hasFn(MFn::kTransform))
        {
            MFnDagNode fnDag(obj);

            MDagPath dag;
            fnDag.getPath(dag);

            dag.extendToShape();
            meshObj = dag.node();
            return MS::kSuccess;
        }
    }

    MReturnFailure(ErrorStr::PSDMeshNotFound);
}


// Get poseDeformer obj
MStatus PoseSpaceCommand::getDeformerObj(MObject& defObj)
{
    MSelectionList sel;
    sel.add(_poseSpaceDeformer);

    MObject obj;
    sel.getDependNode(0, obj);

    MFnDependencyNode fnDep(obj);
    if (fnDep.typeName() == PoseSpaceDeformer::name)
    {
        defObj = obj;
        return MS::kSuccess;
    }

    MReturnFailure(ErrorStr::PSDeformerNotFound);
}


MStatus PoseSpaceCommand::create()
{
    MStatus stat;
    MString msg;
    MObject obj;

    // Get mesh fn
    getMeshFromSelList(obj);
    MCheckStatus(stat, "");
    MFnMesh fnMesh(obj);


    // Get skinCluster
    MFnSkinCluster fnSkinCluster;
    {
        MPlug plug = fnMesh.findPlug("inMesh");

        MItDependencyGraph iter(
            plug, 
            MFn::kSkinClusterFilter, 
            MItDependencyGraph::kUpstream,
            MItDependencyGraph::kDepthFirst,
            MItDependencyGraph::kNodeLevel, 
            &stat);
        MCheckStatus(stat, ErrorStr::PSDSCNotFound);

        if (iter.isDone())            
            MReturnFailure(ErrorStr::PSDSCNotFound);

        stat = fnSkinCluster.setObject(iter.currentItem());
        MCheckStatus(stat, ErrorStr::PSDSCNotFound);
    }


    // Create deformer
    MString cmd = "deformer -type ";
    cmd += PoseSpaceDeformer::name;
    cmd += " -name ";
    cmd += _poseSpaceDeformer;
    MStringArray result;
    stat = MGlobal::executeCommand(cmd, result);
    MCheckStatus(stat, ErrorStr::PSDCreateFailed);

    _poseSpaceDeformer = result[0];
    stat = getDeformerObj(obj);
    MCheckStatus(stat, "");
    MFnDependencyNode fnDeformer(obj);


    // Connect skinCluster weights
    MPlug srcPlug = fnSkinCluster.findPlug("weightList");
    MPlug dstPlug = fnDeformer.findPlug(PoseSpaceDeformer::aSkinClusterWeightList);

    MDGModifier dgMod;
    stat = dgMod.connect(srcPlug, dstPlug);
    stat = dgMod.doIt();
    MCheckStatus(stat, ErrorStr::PSDCreateFailed);

    setResult(_poseSpaceDeformer);

    return MS::kSuccess;
}


MStatus PoseSpaceCommand::addPose()
{
    MStatus stat;
    MString msg;

    // Get deformer
    MObject obj;
    stat = getDeformerObj(obj);
    MCheckStatus(stat, "");
    MFnGeometryFilter fnDeformer(obj);

    // Check joints
    if (_joints.length() == 0)
        MReturnFailure("No joints provided to be added to pose");


    // Get output mesh from deformer
    MDagPath srcPath;
    fnDeformer.getPathAtIndex(0, srcPath);
    obj = fnDeformer.outputShapeAtIndex(0);
    MFnMesh fnSrcMesh(obj);


    // Get pose/target mesh from selection list
    getMeshFromSelList(obj);
    MCheckStatus(stat, "");
    MFnMesh fnTgtMesh(obj);



    // Get skinCluster weightsPlug and joint matrices
    MPlug scWtsPlug;
    JointMatrixMap jtMatrices;
    {
        // Get skinCluster
        MFnSkinCluster fnSkinCluster;
        {
            MPlug plug = fnDeformer.findPlug("input");
            plug = plug[0];
            plug = plug.child(0);

            MItDependencyGraph iter(
                plug,
                MFn::kSkinClusterFilter,
                MItDependencyGraph::kUpstream,
                MItDependencyGraph::kDepthFirst,
                MItDependencyGraph::kNodeLevel,
                &stat);
            MCheckStatus(stat, ErrorStr::PSDSCNotFound);

            if (iter.isDone())
                MReturnFailure(ErrorStr::PSDSCNotFound);

            stat = fnSkinCluster.setObject(iter.currentItem());
            MCheckStatus(stat, ErrorStr::PSDSCNotFound);
#if 0
            MPlug plug = fnDeformer.findPlug(PoseSpaceDeformer::aSkinClusterWeightList);
            MPlugArray plugArr;
            plug.connectedTo(plugArr, 1, 0);
            obj = plugArr[0].node();
            fnSkinCluster.setObject(obj);
#endif
        }
        
        scWtsPlug = fnSkinCluster.findPlug("weightList");
        MPlug bindPlug = fnSkinCluster.findPlug("bindPreMatrix");
        MPlug jtMatPlug = fnSkinCluster.findPlug("matrix");
        MMatrix srcGeomWorldMat = srcPath.inclusiveMatrix();

        MFnMatrixData fnMatrix;

        for( int i=0; i < jtMatPlug.numElements(); ++i )
        {
            if ( jtMatPlug[i].logicalIndex() != bindPlug[i].logicalIndex() )
            {
                MReturnFailure("Matrix and bindPreMatrix plugs in skincluster doesnt match");
            }

            fnMatrix.setObject(bindPlug[i].asMObject());
            MMatrix bindInvMat = fnMatrix.matrix();

            fnMatrix.setObject(jtMatPlug[i].asMObject());
            MMatrix jtMat = fnMatrix.matrix();

            MMatrix scMat = srcGeomWorldMat * bindInvMat * jtMat * srcGeomWorldMat.inverse();
            jtMatrices[jtMatPlug[i].logicalIndex()] = scMat;
        }
    }



    // Get vertices that differ from src and target mesh
    MPointArray srcPositions;
    MPointArray tgtPositions;

    fnSrcMesh.getPoints(srcPositions);
    fnTgtMesh.getPoints(tgtPositions);

    if ( srcPositions.length() != tgtPositions.length() )
    {
        MReturnFailure("Posed mesh and mesh in poseSpaceDeformer differ in vertex count. Failed to add pose.");
    }



    // Get components with delta (in bind space)
    MIntArray components;
    MVectorArray deltas;
    for( int i=0; i < srcPositions.length(); ++i )
    {
        // Delta in skin space
        MVector delta = tgtPositions[i] - srcPositions[i];
        if ( delta.length() < FLOAT_TOLERANCE )
            continue;

        // Find skinMatrix
        MMatrix skinMatrix;        
        MPlug wtsPlug = scWtsPlug[i].child(0);
        for( int j=0; j < wtsPlug.numElements(); ++j )
        {
            int jtIdx = wtsPlug[j].logicalIndex();
            MMatrix mat = wtsPlug[j].asDouble() * jtMatrices[jtIdx];

            if (j)
                skinMatrix += mat;
            else
                skinMatrix = mat;
        }

        // Convert delta from skin to bind space
        delta = delta * skinMatrix.inverse();

        components.append(i);
        deltas.append(delta);
    }

    if (components.length() == 0)
    {
        MReturnFailure("Posed mesh is similar to mesh in poseSpaceDeformer. Failed to add pose.");
    }



    // Get next pose index
    int poseIndex = 0;
    MPlug pPose = fnDeformer.findPlug(PoseSpaceDeformer::aPose);
    if (pPose.numElements())
        poseIndex = pPose[pPose.numElements()-1].logicalIndex() + 1;
    pPose = pPose.elementByLogicalIndex(poseIndex);        

    MPlug pPoseComp = pPose.child(PoseSpaceDeformer::aPoseComponents);
    MPlug pPoseDelta = pPose.child(PoseSpaceDeformer::aPoseDelta);

    MFnIntArrayData fnIntArrData;
    obj = fnIntArrData.create();
    fnIntArrData.set(components);
    pPoseComp.setValue(obj);

    MFnVectorArrayData fnVectorArrData;
    obj = fnVectorArrData.create();
    fnVectorArrData.set(deltas);
    pPoseDelta.setValue(obj);


    // Add joints to pose
    for (int i = 0; i < _joints.length(); ++i)
    {
        MFnTransform fnJnt(_joints[i]);

        // Check if joint index exists in deformer
        int jtIndex = -1;
        MPlug rotPlug = fnJnt.findPlug('rotation');
        MPlugArray conns;
        rotPlug.connectedTo(conns, 0, 1);
        for (int j = 0; j < conns.length(); ++j)
        {
            if (conns[j].node() == fnDeformer.object() && conns[j] == PoseSpaceDeformer::aJoint)
            {
                jtIndex = conns[j].logicalIndex();
            }
        }

        // Connect joint
        if (jtIndex < 0)
        {
            MPlug pjtPlug = fnDeformer.findPlug(PoseSpaceDeformer::aJoint);
            int numJts = pjtPlug.numElements();
            if (numJts)
                jtIndex = pjtPlug[numJts - 1].logicalIndex() + 1;
            else
                jtIndex = 0;

            pjtPlug = pjtPlug.elementByLogicalIndex(jtIndex);

            MDGModifier dgMod;
            stat = dgMod.connect(rotPlug, pjtPlug);
            stat = dgMod.doIt();
            char buf[1024];
            sprintf(buf, ErrorStr::PSDJointConnectionFailed, _joints[i].asChar());
            MCheckStatus(stat, buf);
        }

        // Set poseJoint
        MPlug pPoseJoint = pPose.child(PoseSpaceDeformer::aPoseJoints);
        pPoseJoint = pPoseJoint.elementByLogicalIndex(jtIndex);
        MPlug pRotation = pPoseJoint.child(PoseSpaceDeformer::aPoseJointRotation);

        // Set joint rotation data
        MEulerRotation rot;
        fnJnt.getRotation(rot);
        MFnNumericData fnData;
        MObject obj = fnData.create(MFnNumericData::k3Double);
        fnData.setData(rot.x, rot.y, rot.z);
        pRotation.setValue(obj);
    }



    setResult(poseIndex);

    return MS::kSuccess;
}