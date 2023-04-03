/**
* This file is part of DSO.
* 
* Copyright 2016 Technical University of Munich and Intel.
* Developed by Jakob Engel <engelj at in dot tum dot de>,
* for more information see <http://vision.in.tum.de/dso>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* DSO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* DSO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with DSO. If not, see <http://www.gnu.org/licenses/>.
*/


 
#include "FullSystem/HessianBlocks.h"
#include "util/FrameShell.h"
#include "FullSystem/ImmaturePoint.h"
#include "OptimizationBackend/EnergyFunctionalStructs.h"
// 2022.08.19 czq
#include "utility"
#include "iostream"
#include "opencv2/highgui/highgui.hpp"

using namespace std;
// 2022.08.19 czq

namespace dso
{


PointHessian::PointHessian(const ImmaturePoint* const rawPoint, CalibHessian* Hcalib)
{
	instanceCounter++;
	host = rawPoint->host;
	hasDepthPrior=false;

	idepth_hessian=0;
	maxRelBaseline=0;
	numGoodResiduals=0;

	// set static values & initialization.
	u = rawPoint->u;
	v = rawPoint->v;
	assert(std::isfinite(rawPoint->idepth_max));
	//idepth_init = rawPoint->idepth_GT;

	my_type = rawPoint->my_type;

	setIdepthScaled((rawPoint->idepth_max + rawPoint->idepth_min)*0.5);
	setPointStatus(PointHessian::INACTIVE);

	int n = patternNum;
	memcpy(color, rawPoint->color, sizeof(float)*n);
	memcpy(weights, rawPoint->weights, sizeof(float)*n);
	energyTH = rawPoint->energyTH;

	efPoint=0;


}


void PointHessian::release()
{
	for(unsigned int i=0;i<residuals.size();i++) delete residuals[i];
	residuals.clear();
}


void FrameHessian::setStateZero(const Vec10 &state_zero)
{
	assert(state_zero.head<6>().squaredNorm() < 1e-20);

	this->state_zero = state_zero;


	for(int i=0;i<6;i++)
	{
		Vec6 eps; eps.setZero(); eps[i] = 1e-3;
		SE3 EepsP = Sophus::SE3::exp(eps);
		SE3 EepsM = Sophus::SE3::exp(-eps);
		SE3 w2c_leftEps_P_x0 = (get_worldToCam_evalPT() * EepsP) * get_worldToCam_evalPT().inverse();
		SE3 w2c_leftEps_M_x0 = (get_worldToCam_evalPT() * EepsM) * get_worldToCam_evalPT().inverse();
		nullspaces_pose.col(i) = (w2c_leftEps_P_x0.log() - w2c_leftEps_M_x0.log())/(2e-3);
	}
	//nullspaces_pose.topRows<3>() *= SCALE_XI_TRANS_INVERSE;
	//nullspaces_pose.bottomRows<3>() *= SCALE_XI_ROT_INVERSE;

	// scale change
	SE3 w2c_leftEps_P_x0 = (get_worldToCam_evalPT());
	w2c_leftEps_P_x0.translation() *= 1.00001;
	w2c_leftEps_P_x0 = w2c_leftEps_P_x0 * get_worldToCam_evalPT().inverse();
	SE3 w2c_leftEps_M_x0 = (get_worldToCam_evalPT());
	w2c_leftEps_M_x0.translation() /= 1.00001;
	w2c_leftEps_M_x0 = w2c_leftEps_M_x0 * get_worldToCam_evalPT().inverse();
	nullspaces_scale = (w2c_leftEps_P_x0.log() - w2c_leftEps_M_x0.log())/(2e-3);


	nullspaces_affine.setZero();
	nullspaces_affine.topLeftCorner<2,1>()  = Vec2(1,0);
	assert(ab_exposure > 0);
	nullspaces_affine.topRightCorner<2,1>() = Vec2(0, expf(aff_g2l_0().a)*ab_exposure);
};



void FrameHessian::release()
{
	// DELETE POINT
	// DELETE RESIDUAL
	for(unsigned int i=0;i<pointHessians.size();i++) delete pointHessians[i];
	for(unsigned int i=0;i<pointHessiansMarginalized.size();i++) delete pointHessiansMarginalized[i];
	for(unsigned int i=0;i<pointHessiansOut.size();i++) delete pointHessiansOut[i];
	for(unsigned int i=0;i<immaturePoints.size();i++) delete immaturePoints[i];


	pointHessians.clear();
	pointHessiansMarginalized.clear();
	pointHessiansOut.clear();
	immaturePoints.clear();
}


void FrameHessian::makeImages(float* color, CalibHessian* HCalib)
{

	for(int i=0;i<pyrLevelsUsed;i++)
	{
		dIp[i] = new Eigen::Vector3f[wG[i]*hG[i]];
		absSquaredGrad[i] = new float[wG[i]*hG[i]];
	}
	dI = dIp[0];


	// make d0
	int w=wG[0];
	int h=hG[0];
	for(int i=0;i<w*h;i++)
		dI[i][0] = color[i];

	for(int lvl=0; lvl<pyrLevelsUsed; lvl++)
	{
		int wl = wG[lvl], hl = hG[lvl];
		Eigen::Vector3f* dI_l = dIp[lvl];

		float* dabs_l = absSquaredGrad[lvl];
		if(lvl>0)
		{
			int lvlm1 = lvl-1;
			int wlm1 = wG[lvlm1];
			Eigen::Vector3f* dI_lm = dIp[lvlm1];



			for(int y=0;y<hl;y++)
				for(int x=0;x<wl;x++)
				{
					dI_l[x + y*wl][0] = 0.25f * (dI_lm[2*x   + 2*y*wlm1][0] +
												dI_lm[2*x+1 + 2*y*wlm1][0] +
												dI_lm[2*x   + 2*y*wlm1+wlm1][0] +
												dI_lm[2*x+1 + 2*y*wlm1+wlm1][0]);
				}
		}

		for(int idx=wl;idx < wl*(hl-1);idx++)
		{
			float dx = 0.5f*(dI_l[idx+1][0] - dI_l[idx-1][0]);
			float dy = 0.5f*(dI_l[idx+wl][0] - dI_l[idx-wl][0]);


			if(!std::isfinite(dx)) dx=0;
			if(!std::isfinite(dy)) dy=0;

			dI_l[idx][1] = dx;
			dI_l[idx][2] = dy;


			dabs_l[idx] = dx*dx+dy*dy;

			if(setting_gammaWeightsPixelSelect==1 && HCalib!=0)
			{
				float gw = HCalib->getBGradOnly((float)(dI_l[idx][0]));
				dabs_l[idx] *= gw*gw;	// convert to gradient of original color space (before removing response).
			}
		}
	}
}

//2022.09.20 czq
bool cmp1(const pair<float, float> a, const pair<float, float> b) {
    return a.first<b.first;//自定义的比较函数
}

void FrameHessian::makeDepthImages(float* depthImage)
{
	// 每一层创建图像值, 和图像梯度的存储空间
	for(int i=0;i<pyrLevelsUsed;i++)
	{
		dDepth[i] = new float[wG[i]*hG[i]];
	}

	// make d0
	int w=wG[0]; // 零层weight
	int h=hG[0]; // 零层height
	for(int i=0;i<w*h;i++)
		dDepth[0][i] = depthImage[i] / 10000.0;

	for(int lvl=0; lvl<pyrLevelsUsed; lvl++)
	{
		int wl = wG[lvl], hl = hG[lvl]; // 该层图像大小
		float* dDepth_l = dDepth[lvl];

		if(lvl>0)
		{
			int lvlm1 = lvl-1;
			int wlm1 = wG[lvlm1]; // 列数
			float* dDepth_lm = dDepth[lvlm1];

			for(int y=0;y<hl;y++)
				for(int x=0;x<wl;x++)
				{
					vector<float> vNeighboursValue;

			        if(dDepth_lm[2 * x + 2 * y * wlm1] > 1e-5)
			        {
				        vNeighboursValue.push_back(dDepth_lm[2 * x + 2 * y * wlm1]);
			        }
			        if(dDepth_lm[2 * x + 1 + 2 * y * wlm1] > 1e-5)
			        {
				        vNeighboursValue.push_back(dDepth_lm[2 * x + 1 + 2 * y * wlm1]);
			        }
			        if(dDepth_lm[2 * x + 2 * y * wlm1 + wlm1] > 1e-5)
			        {
				        vNeighboursValue.push_back(dDepth_lm[2 * x + 2 * y * wlm1 + wlm1]);
			        }
			        if(dDepth_lm[2 * x + 1 + 2 * y * wlm1 + wlm1] > 1e-5)
			        {
				        vNeighboursValue.push_back(dDepth_lm[2 * x + 1 + 2 * y * wlm1 + wlm1]);
			        }

			        if(vNeighboursValue.size() == 0)
				        dDepth_l[x + y * wl] = 0;
			        else
			        {
				        float sum = 0.0;

				        for(int j = 0; j < vNeighboursValue.size(); j++)
				        {
					        sum = sum + vNeighboursValue[j];
				        }

				        float average = sum / vNeighboursValue.size();

				        vector<pair<float, float> > vNeighbours;

				        for(int j = 0; j < vNeighboursValue.size(); j++)
				        {
					        vNeighbours.push_back(make_pair(fabs(vNeighboursValue[j] - average) ,vNeighboursValue[j]));
				        }

				        sort(vNeighbours.begin(), vNeighbours.end(), cmp1);

                        //std::cout << "vNeighbours[0].second = " << vNeighbours[0].second << std::endl;
				        dDepth_l[x + y * wl] = vNeighbours[0].second;
			        }
				}
		}
	}
}

void FrameHessian::showDepthImages()
{
	// 每一层创建图像值, 和图像梯度的存储空间
	for(int i=0;i<pyrLevelsUsed;i++)
	{
		cv::Mat m(hG[i], wG[i], CV_32FC1);
		memcpy(m.data, dDepth[i], sizeof(float)*wG[i]*hG[i]);
		m.convertTo(m, CV_16UC1, 5000.0);
		char str[20];
		sprintf(str, "depthImage-%d", i);
		cv::imshow(string(str), m);
	}
}
//2022.09.20 czq

void FrameFramePrecalc::set(FrameHessian* host, FrameHessian* target, CalibHessian* HCalib )
{
	this->host = host;
	this->target = target;

	SE3 leftToLeft_0 = target->get_worldToCam_evalPT() * host->get_worldToCam_evalPT().inverse();
	PRE_RTll_0 = (leftToLeft_0.rotationMatrix()).cast<float>();
	PRE_tTll_0 = (leftToLeft_0.translation()).cast<float>();



	SE3 leftToLeft = target->PRE_worldToCam * host->PRE_camToWorld;
	PRE_RTll = (leftToLeft.rotationMatrix()).cast<float>();
	PRE_tTll = (leftToLeft.translation()).cast<float>();
	distanceLL = leftToLeft.translation().norm();


	Mat33f K = Mat33f::Zero();
	K(0,0) = HCalib->fxl();
	K(1,1) = HCalib->fyl();
	K(0,2) = HCalib->cxl();
	K(1,2) = HCalib->cyl();
	K(2,2) = 1;
	PRE_KRKiTll = K * PRE_RTll * K.inverse();
	PRE_RKiTll = PRE_RTll * K.inverse();
	PRE_KtTll = K * PRE_tTll;


	PRE_aff_mode = AffLight::fromToVecExposure(host->ab_exposure, target->ab_exposure, host->aff_g2l(), target->aff_g2l()).cast<float>();
	PRE_b0_mode = host->aff_g2l_0().b;
}

}

