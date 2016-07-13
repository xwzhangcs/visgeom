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
Scale space for multiscale optimization
*/

#pragma once

#include "std.h"
#include "ocv.h"

class BinaryScalSpace
{
public:
    BinaryScalSpace(int numScales) : activeScale(1), activeScaleIdx(0)
    {
        imgVec.resize(numScales);
    }
    
    BinaryScalSpace() : activeScale(1), activeScaleIdx(0)
    {
        imgVec.resize(1);
    }
    
    void setNumberScales(int numScales)
    {
        assert(numScales > 0);
        imgVec.resize(numScales);
    }
    
//    virtual ~ScalSpace();
    
    void generate(const Mat8u & img)
    {
        cvtColor(img, imgVec[0], CV_32F);
        propagate();
    }

    void generate(const Mat32f & img)
    {
        img.copyTo(imgVec[0]);
        propagate();
    }

    void propagate()
    {
        for (int i = 1; i < imgVec.size(); i++)
        {
            Mat32f blured;
            GaussianBlur(imgVec[i - 1], blured, Size(3, 3), 0, 0);
            resize(blured, imgVec[i], Size(blured.cols/2, blured.rows/2));
        }
    }
    
    const Mat32f & get() const { return imgVec[activeScaleIdx]; }
    
    int size() const { return imgVec.size(); }
    
    int scale(int idx) const { return (1 << idx); }
    
    void setActiveScale(int idx) 
    { 
        activeScale = scale(idx);
        activeScaleIdx = idx;
    }
    
    int getActiveScale() 
    { 
        return activeScale;
    }
    
    int getActiveIdx()
    {
        return activeScaleIdx;
    }
    
    double uBase(double u) const { return u * activeScale; }
    double vBase(double v) const { return v * activeScale; }
    
    double uScaled(double u) const { return u / activeScale; }
    double vScaled(double v) const { return v / activeScale; }
    
private:
    std::vector<Mat32f> imgVec;
    int activeScale;
    int activeScaleIdx;
};
