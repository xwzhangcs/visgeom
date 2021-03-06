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

/*
Cost functions for localization based on photometric data and mutual information
*/

#pragma once

#include "std.h"
#include "eigen.h"
#include "ocv.h"
#include "ceres.h"

#include "geometry/geometry.h"
#include "projection/generic_camera.h"

// to store the data for the photometric optimization
struct PhotometricPack
{
    vector<double> valVec;
    vector<Vector3d> cloud;
    vector<int> idxVec;
    int scaleIdx;
};

/*
A cost function with analytic jacobian
works faster than autodiff version and works with any ICamera
*/
struct PhotometricCostFunction : ceres::CostFunction
{

    PhotometricCostFunction(const ICamera * camera, const PhotometricPack & dataPack,
            const Mat32f & img2, double scale) :
            _camera(camera->clone()),
            _dataPack(dataPack),
            _imageGrid(img2.cols, img2.rows, (float*)(img2.data)),
            _scale(scale) 
    {
        mutable_parameter_block_sizes()->clear();
        mutable_parameter_block_sizes()->push_back(6);
        set_num_residuals(_dataPack.cloud.size());
    }
    
    virtual ~PhotometricCostFunction()  
    {
        delete _camera;
        _camera = NULL;
    }
    
    virtual bool Evaluate(double const * const * parameters, double * residual, double ** jacobian) const;

    ICamera * _camera;
    const PhotometricPack & _dataPack;
    const Grid2D _imageGrid;
    const double _scale;
};


/*
This struct is ment for the automatic differentiation by ceres-solver.
class Projector must be known at the compilation time.
*/
template<template<typename> class Projector>
struct PhotometricError 
{
    PhotometricError(const vector<double> & projectionParams, const PhotometricPack & dataPack,
            const Mat32f & img2, double scale) :
            _projectionParams(projectionParams),
            _dataPack(dataPack),
            _imageGrid(img2.cols, img2.rows, (float*)(img2.data)),
            _scale(scale) {}
            
    template <typename T>
    bool operator()(const T * const* params,
                    T* residual) const 
    {
        Transformation<T> T12(params[0]);
        vector<Vector3<T>> transformedPoints;
        transformedPoints.reserve(_dataPack.cloud.size());
        for (auto & point : _dataPack.cloud)
        {
            transformedPoints.push_back(point.template cast<T>());
        }
        T12.inverseTransform(transformedPoints, transformedPoints);
        Projector<T> projector;
        ceres::BiCubicInterpolator<Grid2D> imageInterpolator(_imageGrid);
        vector<T> projectionParamsT;
        projectionParamsT.reserve(_projectionParams.size());
        for (auto & x : _projectionParams)
        {
            projectionParamsT.push_back(T(x));
        }
        for (int i = 0; i < transformedPoints.size(); i++)
        {
            Vector2<T> pt;
            if (projector(projectionParamsT.data(), transformedPoints[i].data(), pt.data())) 
            {
                T res;
                imageInterpolator.Evaluate(pt[1] / T(_scale), pt[0] / T(_scale), &res);
                residual[i] = res - T(_dataPack.valVec[i]);
            }
            else
            {
                residual[i] = T(0.);
            }
        }
        return true;
    }
    
    const vector<double> & _projectionParams;
    const PhotometricPack & _dataPack;
    const Grid2D _imageGrid;
    const double _scale; 
};

/*
Mutual Information cost function 
//TODO complete the gradient computation explanation
*/
struct MutualInformation : public FirstOrderFunction
{

    MutualInformation(const ICamera * camera, const PhotometricPack & dataPack,
            const Mat32f & img2, double scale, int numBins, double valMax = 1.) :
            _camera(camera->clone()),
            _dataPack(dataPack),
            _imageGrid(img2.cols, img2.rows, (float*)img2.data),
            _scale(scale),
            _numBins(numBins),
            _histStep(valMax / (numBins - 1)),
            _increment(1./dataPack.cloud.size()),
            _hist1(computeHist(dataPack.valVec))
    { }
    
    virtual int NumParameters() const { return 6; }
    
    virtual ~MutualInformation()  
    {
        delete _camera;
        _camera = NULL;
    }
    
    virtual bool Evaluate(double const * parameters, double * cost, double * gradient) const;
    
    void computeShareDerivative(double val, int & idx1, int & idx2, double & der) const;
    
    void computeShares(double val, int & idx1, int & idx2, double & share) const;
    
    vector<double> computeHist(const vector<double> & valVec) const;
    
    //the first vector corresponds to the first image
    //to efficiently compute the histogram for the second image
    vector<double> computeHist2d(const vector<double> & valVec1, const vector<double> & valVec2) const;
    
    vector<double> reduceHist(const vector<double> & hist2d) const;
    
    ICamera * _camera;
    const PhotometricPack & _dataPack;
    const Grid2D _imageGrid;
    const double _scale;
    
    //histogram params
    const int _numBins;
    const double _histStep;
    
    //variable must be initialize for MutualInformation::computeShares
    double _increment;
    
    vector<double> _hist1;
};

// excluding the point reconstruction reduces the number of unknowns,
// might be faster TODO check it
// but statisically not optimal
struct EssentialCost : ceres::SizedCostFunction<6, 6>
{
    EssentialCost(const Vector3d x1, const Vector3d x2);

    virtual ~EssentialCost() { }
    
    virtual bool Evaluate(double const * const * params,
            double * residual, double ** jacobian) const;
    
    vector<double> _aVec;
};

//5-point algorithm
//a simpler solution, more variables TODO compare performance
//TODO change from 5 to any number
struct MonoReprojectCost : ceres::SizedCostFunction<10, 6, 5>
{
    MonoReprojectCost(const ICamera * camera,
            const Vector3dVec & xVec1, const Vector2dVec & pVec2,
            const Transf xiBaseCam):
    _pVec2(pVec2), _xVec1(xVec1), _camera(camera->clone()), _xiBaseCam(xiBaseCam) 
    {
        assert(_xVec1.size() == 5);
        assert(_pVec2.size() == 5);
    }

    virtual ~MonoReprojectCost() { delete _camera; }
    
    virtual bool Evaluate(double const * const * params,
            double * residual, double ** jacobian) const;
    
    const ICamera * _camera;
    const Vector3dVec _xVec1;
    const Vector2dVec _pVec2;
    Transf _xiBaseCam;
    vector<double> _aVec;
};


struct SparseReprojectCost : ceres::CostFunction
{
    SparseReprojectCost(const ICamera * camera,
            const Vector3dVec & xVec1, const Vector3dVec & xVec2,
            const Vector2dVec & pVec2, const Transf xiBaseCam):
    _pVec2(pVec2), _xVec1(xVec1), _xVec2(xVec2),  _camera(camera->clone()), _xiBaseCam(xiBaseCam) 
    {
        assert(_pVec2.size() == _xVec1.size());
        assert(_pVec2.size() == _xVec2.size());
        set_num_residuals(_pVec2.size() * 2);           // projection error
        mutable_parameter_block_sizes()->push_back(6);  // 6dof transformation
    }

    virtual ~SparseReprojectCost() { delete _camera; }
    
    virtual bool Evaluate(double const * const * params,
            double * residual, double ** jacobian) const;
    
    const ICamera * _camera;
    const Vector3dVec _xVec1;
    const Vector3dVec _xVec2;
    const Vector2dVec _pVec2;
    Transf _xiBaseCam;
    vector<double> _aVec;
};

struct OdometryPrior : ceres::SizedCostFunction<6, 6>
{
    OdometryPrior(const double errV, const double errW, const double lambdaT, const double lambdaR,
        const Transf xiOdom);

    virtual ~OdometryPrior() { }
    
    virtual bool Evaluate(double const * const * params,
            double * residual, double ** jacobian) const;
    
    
    Vector6d _dxiPrior;
    Matrix6d _A;
};

