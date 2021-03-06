/*
This file is part of visgeom.

visgeom is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

visgeom is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with visgeom.  If not, see <http://www.gnu.org/licenses/>.
*/ 

#include "calibration/unified_calibration.h"

#include "std.h"
#include "io.h"
#include "ocv.h"
#include "eigen.h"
#include "ceres.h"
#include "json.h"

#include <glog/logging.h>

#include "calibration/calib_cost_functions.h"
#include "projection/generic_camera.h"
#include "projection/eucm.h"
    
bool GenericCameraCalibration::compute()
{
    //run the solver
    Solver::Options options;
//        options.check_gradients = true;
    options.gradient_check_relative_precision = 1e-2;
    options.max_num_iterations = 1000;
    options.function_tolerance = 1e-10;
    options.gradient_tolerance = 1e-10;
    options.parameter_tolerance = 1e-10;
    options.logging_type = ceres::SILENT;
//        options.minimizer_progress_to_stdout = true;
    Solver::Summary summary;
    Solve(options, &globalProblem, &summary);
    cout << summary.BriefReport() << endl;
    
    cout << "Intrinsic parameters :" << endl;
    for (auto & x : intrinsicMap)
    {
        cout << x.first << " : ";
        for (int i = 0; i < cameraMap[x.first]->numParams(); i++) //FIXME store the intrinsic size
        {
            cout << x.second[i] << "  ";
        }
        cout << endl;
        
    }
    
    cout << "Local extrinsic parameters :" << endl;
    for (auto & sequenceMap : sequenceTransformMap)
    {
        cout << "Sequence : " << sequenceMap.first << endl;
        int i = 0;
        for (auto & x : sequenceMap.second)
        {
            cout << i++ << " : " << Transformation<double>(x.data()) << endl;
        }
    }
    
    cout << "Global extrinsic parameters :" << endl;
    for (auto & x : globalTransformMap)
    {
        cout << x.first << " : " << Transformation<double>(x.second.data()) << endl;
    }
    
}

void GenericCameraCalibration::parseTransforms()
{
    for (auto & transInfo : root.get_child("transformations"))
    {
        const string name = transInfo.second.get<string>("name");
        transformInfoMap[name] = TransformInfo();
        auto & info = transformInfoMap[name];
        info.global = transInfo.second.get<bool>("global");
        info.prior = transInfo.second.get<bool>("prior");
        info.constant = transInfo.second.get<bool>("constant");
        assert(not info.constant or info.prior);
        info.initialized = false;
        
        if (info.global)
        {
            globalTransformMap[name] = Array6d();
            if (info.prior)
            {
                auto transf = readTransform(transInfo.second.get_child("value"));
                transf.toArray(globalTransformMap[name].data());
            }
        }
        else 
        {
            sequenceTransformMap[name] = vector<Array6d>();
            if (info.prior)
            {
                auto & valVec = sequenceTransformMap[name];
                for (auto & val : transInfo.second.get_child("value"))
                {
                    auto transf = readTransform(val.second);
                    valVec.emplace_back(transf.toArray());
                }
            }
        }
    }
}

void GenericCameraCalibration::parseCameras()
{
    for (auto & cameraInfo : root.get_child("cameras"))
    {
        const string name = cameraInfo.second.get<string>("name");
        intrinsicMap[name] = vector<double>();
        auto & intrinsicVec = intrinsicMap[name];
        const string cameraType = cameraInfo.second.get<string>("type");
        for (auto & x : cameraInfo.second.get_child("value"))
        {
            intrinsicVec.push_back(x.second.get_value<double>());
        }
        if (cameraType == "eucm")
        {
            cout << "Model : EUCM" << endl;
            assert(intrinsicVec.size() == 6);
            cameraMap[name] = new EnhancedCamera(intrinsicVec.data());
        }
        else
        {
            cout << "ERROR : invalid camera model name" << endl;
            assert(false);
        }
    }
}

void GenericCameraCalibration::initTransformChainInfo(const ptree & node)
{
    transNameVec.clear();
    transStatusVec.clear();
    cameraName = node.get<string>("camera");
    cout <<"Camera : " <<  cameraName << endl;
    cout <<"Transformations : ";
    for (auto & transInfo : node.get_child("transform_chain"))
    {
        transNameVec.push_back(transInfo.second.get<string>("name"));
        cout << transNameVec.back();
        if (transInfo.second.get<bool>("direct")) 
        {
            transStatusVec.push_back(TRANSFORM_DIRECT);
        }
        else
        {
            transStatusVec.push_back(TRANSFORM_INVERSE);
            cout << "_inv";
        }
        cout <<  "   ";
    }
    cout << endl;
}

void GenericCameraCalibration::initGrid(const ptree & node)
{
    Nx = node.get<int>("object.cols");
    Ny = node.get<int>("object.rows");
    double sqSize = node.get<double>("object.size");
    grid.clear();
    grid.reserve(Nx*Ny);
    for (int i = 0; i < Ny; i++)
    {
        for (int j = 0; j < Nx; j++)
        {
           grid.emplace_back(sqSize * j, sqSize * i, 0); 
        }
    }
    //fill up gridExtractionVec which stores all the extracted grids
    const string prefix = node.get<string>("images.prefix");
    bool checkExtraction = node.get<bool>("parameters.check_extraction");
    gridExtractionVec.clear();
    for (auto & x : node.get_child("images.names"))
    {
        gridExtractionVec.emplace_back();
        const string filename = x.second.get_value<string>();
        cout << "." << flush;
        if (not extractGridProjection(prefix + filename, gridExtractionVec.back(), checkExtraction))
        {
            cout << endl << "WARNING : GRID NOT EXTRACTED" << endl;
        }
    }
    cout << endl;
}

void GenericCameraCalibration::getInitTransform(Transformation<double> & xi,
            const string & initName, int gridIdx)
{
    for (int i = 0; i < transNameVec.size(); i++)
    {
        const string & name = transNameVec[i];
        if (name == initName) break;
        else if (transStatusVec[i] == TRANSFORM_DIRECT)
        {
            xi = getTransform(name, gridIdx).inverseCompose(xi);
        }
        else if (transStatusVec[i] == TRANSFORM_INVERSE)
        {
            xi = getTransform(name, gridIdx).compose(xi);
        }
    }
    for (int i = transNameVec.size() - 1; i >= 0; i--)
    {
        const string & name = transNameVec[i];
        if (name == initName)
        {
            if (transStatusVec[i] == TRANSFORM_INVERSE)
            {
                xi = xi.inverse();
            }
            break;
        }
        else if (transStatusVec[i] == TRANSFORM_DIRECT)
        {
            xi = xi.composeInverse(getTransform(name, gridIdx));
        }
        else if (transStatusVec[i] == TRANSFORM_INVERSE)
        {
            xi = xi.compose(getTransform(name, gridIdx));
        }
    }
}

void GenericCameraCalibration::initGlobalTransform(const string & name)
{
    Problem problem;
    for (int gridIdx = 0; gridIdx < gridExtractionVec.size(); gridIdx++)
    {
        if (gridExtractionVec[gridIdx].empty()) continue;
        
        vector<double*> ptrVec;
        for (int i = 0; i < transNameVec.size(); i++)
        {
            const string & name = transNameVec[i];
            ptrVec.push_back(getTransformData(name, gridIdx).data());
        }

        //add a residual
        GenericProjectionJac * costFunction = new GenericProjectionJac(gridExtractionVec[gridIdx],
                    grid, cameraMap[cameraName], transStatusVec);

        switch (ptrVec.size())
        {
        case 0:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    intrinsicMap[cameraName].data());
            break;
        case 1:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], intrinsicMap[cameraName].data());
            break;
        case 2:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], intrinsicMap[cameraName].data());
            break;
        case 3:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2], intrinsicMap[cameraName].data());
            break;
        case 4:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2],
                    ptrVec[3], intrinsicMap[cameraName].data());
            break;
        case 5:
            problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2],
                    ptrVec[3], ptrVec[4], intrinsicMap[cameraName].data());
            break;
        }
        
        //set everything to constant except for the transform to be initialize
        for (int i = 0; i < transNameVec.size(); i++)
        {
            if (transNameVec[i] != name)
            {
                problem.SetParameterBlockConstant(ptrVec[i]);
            }
        }
    }
    //initrinsics are constant as well
    problem.SetParameterBlockConstant(intrinsicMap[cameraName].data());
    
    //solve
    Solver::Options options;
//    options.check_gradients = true;
    options.max_num_iterations = 500;
    Solver::Summary summary;
    Solve(options, &problem, &summary);
//    cout << summary.FullReport() << endl;
}

void GenericCameraCalibration::initTransforms(const ptree & node)
{
    const string initName = node.get<string>("init");
    if (initName != "none")
    {
        //there is a transform to initialize
        assert(transformInfoMap.find(initName) != transformInfoMap.end());
        
        //and it belongs to the transformation chain
        auto nameIter = find(transNameVec.begin(), transNameVec.end(), initName);
        assert(nameIter != transNameVec.end());
        
        //it is not allowed to initialize a transformation with a prior
        if (not (transformInfoMap[initName].prior or transformInfoMap[initName].initialized))
        {
            transformInfoMap[initName].initialized = true;
            
            //make sure that the data is not initializad
            if (not transformInfoMap[initName].global)
            {
                assert(sequenceTransformMap[initName].size() == 0);
            }
            
            //do the initialization
            if (not transformInfoMap[initName].global)
            {
                for (int gridIdx = 0; gridIdx < gridExtractionVec.size(); gridIdx++)
                {
                    if (gridExtractionVec[gridIdx].empty())
                    {
                        cout << "WARNING : " << initName << " " << gridIdx
                             << "is not initialized, no board extracted" << endl;
                        sequenceTransformMap[initName].push_back(Array6d{0, 0, 1, 0, 0, 0});      
                    }
                    auto xi = estimateInitialGrid(cameraName, gridExtractionVec[gridIdx], grid);
                    getInitTransform(xi, initName, gridIdx);
                    sequenceTransformMap[initName].push_back(xi.toArray());
                }
            }
            else
            {
                int i = 0;
                while (gridExtractionVec[i].empty()) i++;
                assert(i < gridExtractionVec.size());
                auto xi = estimateInitialGrid(cameraName, gridExtractionVec[i], grid);
                getInitTransform(xi, initName, i);
                xi.toArray(globalTransformMap[initName].data());
                
                if (gridExtractionVec.size() > 1) initGlobalTransform(initName);
            }
        }
    }
    
    //to make sure that all the other transformations are initialized
    for (auto & x : transNameVec)
    {
        assert(transformInfoMap[x].prior xor transformInfoMap[x].initialized);
    }
}

void GenericCameraCalibration::addGridResidualBlocks()
{
    for (int gridIdx = 0; gridIdx < gridExtractionVec.size(); gridIdx++)
    {
        if (gridExtractionVec[gridIdx].empty()) continue;
        
        // make the vector of pointers to the transformation data
        vector<double*> ptrVec;
        for (int i = 0; i < transNameVec.size(); i++)
        {
            const string & name = transNameVec[i];
            ptrVec.push_back(getTransformData(name, gridIdx).data());
        }

        //add a residual
        GenericProjectionJac * costFunction = new GenericProjectionJac(gridExtractionVec[gridIdx],
                    grid, cameraMap[cameraName], transStatusVec);

        switch (ptrVec.size())
        {
        case 0:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                 intrinsicMap[cameraName].data());
            break;
        case 1:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], intrinsicMap[cameraName].data());
            break;
        case 2:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], intrinsicMap[cameraName].data());
            break;
        case 3:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2], intrinsicMap[cameraName].data());
            break;
        case 4:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2],
                    ptrVec[3], intrinsicMap[cameraName].data());
            break;
        case 5:
            globalProblem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
                    ptrVec[0], ptrVec[1], ptrVec[2],
                    ptrVec[3], ptrVec[4], intrinsicMap[cameraName].data());
            break;
        }
        
        //set constant transformations
        for (int i = 0; i < transNameVec.size(); i++)
        {
            const string & name = transNameVec[i];
            if (transformInfoMap[name].constant)
            {
                globalProblem.SetParameterBlockConstant(ptrVec[i]);
            }
            ptrVec.push_back(getTransformData(name, gridIdx).data());
        }
        
        //set the limits on intrinsics
        for (int i = 0; i < intrinsicMap[cameraName].size(); i++)
        {
            globalProblem.SetParameterLowerBound(intrinsicMap[cameraName].data(), i,
                        cameraMap[cameraName]->lowerBound(i));
            globalProblem.SetParameterUpperBound(intrinsicMap[cameraName].data(), i,
                        cameraMap[cameraName]->upperBound(i));
        }
    }
}
    
void GenericCameraCalibration::parseData()
{
    for (auto & dataInfo : root.get_child("data"))
    {
        const string dataType = dataInfo.second.get<string>("type");
        if (dataType == "images")
        {
            initTransformChainInfo(dataInfo.second);
            
            initGrid(dataInfo.second);
            
            initTransforms(dataInfo.second);
            
            addGridResidualBlocks();
        }
        else if (dataType == "odometry")
        {
            const string transformName = dataInfo.second.get<string>("transform");
            assert(transformInfoMap.find(transformName) != transformInfoMap.end());
            assert(not transformInfoMap[transformName].global); //works only fo sequences
            
            const double errV = dataInfo.second.get<double>("err_v"); //relative error in speed
            const double errW = dataInfo.second.get<double>("err_w"); //relative error in rotation
            const double lambda = dataInfo.second.get<double>("lambda"); //relative error in rotation
            //read out the transformations
            vector<Transformation<double>> odometryVec;
            for (auto & odomItem : dataInfo.second.get_child("value"))
            {
                odometryVec.emplace_back(readTransform(odomItem.second));
            }
            
            //use the odometry as initial values
            if (dataInfo.second.get<bool>("init"))
            {
                assert(sequenceTransformMap[transformName].size() == 0);
                transformInfoMap[transformName].initialized = true;
                for (auto & xi : odometryVec)
                {
                    sequenceTransformMap[transformName].emplace_back(xi.toArray());
                }
            }
            
            //add the cost functions
            for (int i = 0; i < odometryVec.size() - 1; i++)
            {
                OdometryPrior * costFunction = new OdometryPrior(errV, errW, lambda,
                                odometryVec[i], odometryVec[i + 1]);
                                
                globalProblem.AddResidualBlock(costFunction, NULL,
                    sequenceTransformMap[transformName][i].data(),
                    sequenceTransformMap[transformName][i + 1].data()); 
                
            }
            if (dataInfo.second.get<bool>("anchor"))
            {
                globalProblem.SetParameterBlockConstant(sequenceTransformMap[transformName][0].data());
            }
        }
        else if (dataType == "transformation_prior")
        {
            const string transformName = dataInfo.second.get<string>("transform");
            
            assert(globalTransformMap.find(transformName) != globalTransformMap.end());
            assert(transformInfoMap[transformName].prior);
            vector<double> stiffnessVec;
            for (auto & x : dataInfo.second.get_child("stiffness"))
            {
                stiffnessVec.push_back(x.second.get_value<double>());
            }
            
            double * transformData = getTransformData(transformName).data();
            TransformationPrior * costFunction = new TransformationPrior(stiffnessVec.data(), transformData);
            globalProblem.AddResidualBlock(costFunction, NULL, transformData); 
        }
    }
}

bool GenericCameraCalibration::extractGridProjection(const string & fileName, Vector2dVec & projection, bool checkExtraction)
{
    Size patternSize(Nx, Ny);
    Mat frame = imread(fileName, 0);
    if (frame.empty())
    {
        cout << fileName << " : ERROR, file not found" << endl;
        return false;
    }
    vector<cv::Point2f> centers;
    bool patternIsFound = findChessboardCorners(frame, patternSize, centers, CV_CALIB_CB_ADAPTIVE_THRESH);
    if (not patternIsFound)
    {
        cout << fileName << " : ERROR, pattern not found" << endl;
        return false;
    }

    if (checkExtraction)
    {
        drawChessboardCorners(frame, patternSize, Mat(centers), patternIsFound);
        imshow("corners", frame);
        char key = waitKey();
        if (key == 'n' or key == 'N')
        {
            cout << fileName << " : ERROR, pattern not accepted" << endl;
            return false;
        }
    }
    
    projection.resize(Nx * Ny);
    for (int i = 0; i < Nx * Ny; i++)
    {
        projection[i] = Vector2d(centers[i].x, centers[i].y);
    }
    return true;
}

Transformation<double> GenericCameraCalibration::estimateInitialGrid(const string & cameraName,
        const Vector2dVec & projection, const Vector3dVec & grid)
{
    Problem problem;
    GenericProjectionJac * costFunction = new GenericProjectionJac(projection, grid,
            cameraMap[cameraName], {TRANSFORM_DIRECT});
    array<double, 6> xi{0, 0, 1, 0, 0, 0};
    
    Vector2d v = projection[1] - projection[0];
    xi[5] = atan2(v[1], v[0]);
    
    problem.AddResidualBlock(costFunction, new SoftLOneLoss(1),
            xi.data(), intrinsicMap[cameraName].data());
    problem.SetParameterBlockConstant(intrinsicMap[cameraName].data());
    Solver::Options options;
    options.max_num_iterations = 500;
    Solver::Summary summary;
    Solve(options, &problem, &summary);
//    cout << Transformation<double>(xi.data()) << endl;
    return Transformation<double>(xi.data());
}
