/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                        Intel License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of Intel Corporation may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

// Vendored from OpenCV 4.14.0, modules/video/src/ecc.cpp
// (https://github.com/opencv/opencv/blob/4.14.0/modules/video/src/ecc.cpp). The OpenCV
// project is Apache-2.0; this file keeps the upstream license header above verbatim.
//
// Changes from upstream (sr, 2026-11-16):
//  1. "precomp.hpp" replaced by public headers; code wrapped in namespace sr::vendor.
//  2. Only the homography path is kept: image_jacobian_{euclidean,affine,translation}_ECC,
//     computeECC and the non-homography branches of update_warping_matrix_ECC are removed.
//  3. Masks removed (callers pass none): templateMask/inputMask branches are gone; the
//     all-ones input mask and its per-iteration warp are kept, as upstream does without masks.
//  4. Single channel only, so the cv::merge of the coordinate grids (a copy for 1 channel)
//     is removed.
//  5. findTransformECCWithMask becomes eccHomography: argument checks return nullopt instead
//     of CV_Assert/CV_Error, the two CV_Error(StsNoConv) (rho NaN, lambda_d <= 0) return
//     nullopt, and a non-finite warp after an update returns nullopt so it never reaches
//     warpPerspective.
//  6. CV_Assert calls inside the helpers are removed; eccHomography's checks make them hold.

#include "Ecc.h"

#include <opencv2/imgproc.hpp>

#include <cmath>

namespace sr::vendor {
namespace {

using namespace cv;

void image_jacobian_homo_ECC(const Mat& src1, const Mat& src2, const Mat& src3, const Mat& src4, const Mat& src5,
                             Mat& dst) {
    const float* hptr = src5.ptr<float>(0);

    const float h0_ = hptr[0];
    const float h1_ = hptr[3];
    const float h2_ = hptr[6];
    const float h3_ = hptr[1];
    const float h4_ = hptr[4];
    const float h5_ = hptr[7];
    const float h6_ = hptr[2];
    const float h7_ = hptr[5];

    const int w = src1.cols;

    // create denominator for all points as a block
    Mat den_;
    addWeighted(src3, h2_, src4, h5_, 1.0, den_);

    // create projected points
    Mat hatX_, hatY_;
    addWeighted(src3, h0_, src4, h3_, 0.0, hatX_);
    hatX_ += h6_;

    addWeighted(src3, h1_, src4, h4_, 0.0, hatY_);
    hatY_ += h7_;

    divide(-hatY_, den_, hatY_);
    divide(-hatX_, den_, hatX_);

    // instead of dividing each block with den,
    // just pre-divide the block of gradients (it's more efficient)

    Mat src1Divided_;
    Mat src2Divided_;

    divide(src1, den_, src1Divided_);
    divide(src2, den_, src2Divided_);

    // compute Jacobian blocks (8 blocks)

    dst.colRange(0, w) = src1Divided_.mul(src3);  // 1

    dst.colRange(w, 2 * w) = src2Divided_.mul(src3);  // 2

    Mat temp_ = (hatX_.mul(src1Divided_) + hatY_.mul(src2Divided_));
    dst.colRange(2 * w, 3 * w) = temp_.mul(src3);  // 3

    hatX_.release();
    hatY_.release();

    dst.colRange(3 * w, 4 * w) = src1Divided_.mul(src4);  // 4

    dst.colRange(4 * w, 5 * w) = src2Divided_.mul(src4);  // 5

    dst.colRange(5 * w, 6 * w) = temp_.mul(src4);  // 6

    src1Divided_.copyTo(dst.colRange(6 * w, 7 * w));  // 7

    src2Divided_.copyTo(dst.colRange(7 * w, 8 * w));  // 8
}

void project_onto_jacobian_ECC(const Mat& src1, const Mat& src2, Mat& dst) {
    /* this functions is used for two types of projections. If src1.cols ==src.cols
    it does a blockwise multiplication (like in the outer product of vectors)
    of the blocks in matrices src1 and src2 and dst
    has size (number_of_blcks x number_of_blocks), otherwise dst is a vector of size
    (number_of_blocks x 1) since src2 is "multiplied"(dot) with each block of src1.

    The number_of_blocks is equal to the number of parameters we are lloking for
    (i.e. rtanslation:2, euclidean: 3, affine: 6, homography: 8)

    */
    int w;

    float* dstPtr = dst.ptr<float>(0);

    if (src1.cols != src2.cols) {  // dst.cols==1
        w = src2.cols;
        for (int i = 0; i < dst.rows; i++) {
            dstPtr[i] = (float)src2.dot(src1.colRange(i * w, (i + 1) * w));
        }
    }

    else {
        w = src2.cols / dst.cols;
        Mat mat;
        for (int i = 0; i < dst.rows; i++) {
            mat = Mat(src1.colRange(i * w, (i + 1) * w));
            dstPtr[i * (dst.rows + 1)] = (float)std::pow(norm(mat), 2);  // diagonal elements

            for (int j = i + 1; j < dst.cols; j++) {  // j starts from i+1
                dstPtr[i * dst.cols + j] = (float)mat.dot(src2.colRange(j * w, (j + 1) * w));
                dstPtr[j * dst.cols + i] = dstPtr[i * dst.cols + j];  // due to symmetry
            }
        }
    }
}

void update_warping_matrix_ECC(Mat& map_matrix, const Mat& update) {
    float* mapPtr = map_matrix.ptr<float>(0);
    const float* updatePtr = update.ptr<float>(0);

    mapPtr[0] += updatePtr[0];
    mapPtr[3] += updatePtr[1];
    mapPtr[6] += updatePtr[2];
    mapPtr[1] += updatePtr[3];
    mapPtr[4] += updatePtr[4];
    mapPtr[7] += updatePtr[5];
    mapPtr[2] += updatePtr[6];
    mapPtr[5] += updatePtr[7];
}

}  // namespace

std::optional<double> eccHomography(const Mat& templ, const Mat& input, Mat& warp,
                                    int maxIter, double eps, int gaussFiltSize) {
    // sr: upstream CV_Assert/CV_Error argument checks, as failures (AD-004).
    if (templ.empty() || input.empty() || templ.type() != CV_32FC1 || input.type() != CV_32FC1) {
        return std::nullopt;
    }
    if (warp.rows != 3 || warp.cols != 3 || warp.type() != CV_32FC1 || !warp.isContinuous()) {
        return std::nullopt;
    }
    if (maxIter <= 0 || gaussFiltSize <= 0 || gaussFiltSize % 2 == 0) return std::nullopt;

    Mat src = templ;   // template image
    Mat dst = input;   // input image (to be warped)
    Mat& map = warp;   // warp (transformation)

    const int numberOfIterations = maxIter;
    const double termination_eps = eps;

    const int numberOfParameters = 8;

    const int ws = src.cols;
    const int hs = src.rows;
    const int wd = dst.cols;
    const int hd = dst.rows;

    Mat Xcoord = Mat(1, ws, CV_32F);
    Mat Ycoord = Mat(hs, 1, CV_32F);
    Mat Xgrid = Mat(hs, ws, CV_32F);
    Mat Ygrid = Mat(hs, ws, CV_32F);

    float* XcoPtr = Xcoord.ptr<float>(0);
    float* YcoPtr = Ycoord.ptr<float>(0);
    int j;
    for (j = 0; j < ws; j++) XcoPtr[j] = (float)j;
    for (j = 0; j < hs; j++) YcoPtr[j] = (float)j;

    repeat(Xcoord, hs, 1, Xgrid);
    repeat(Ycoord, 1, ws, Ygrid);

    Xcoord.release();
    Ycoord.release();

    const int type = CV_32FC1;

    Mat templateZM = Mat(hs, ws, type);     // to store the (smoothed)zero-mean version of template
    Mat templateFloat = Mat(hs, ws, type);  // to store the (smoothed) template
    Mat imageFloat = Mat(hd, wd, type);     // to store the (smoothed) input image
    Mat imageWarped = Mat(hs, ws, type);    // to store the warped zero-mean input image
    Mat imageMask = Mat(hs, ws, CV_8U);     // to store the final mask

    // Gaussian filtering is optional
    src.convertTo(templateFloat, templateFloat.type());
    GaussianBlur(templateFloat, templateFloat, Size(gaussFiltSize, gaussFiltSize), 0, 0);

    dst.convertTo(imageFloat, imageFloat.type());
    GaussianBlur(imageFloat, imageFloat, Size(gaussFiltSize, gaussFiltSize), 0, 0);

    // needed matrices for gradients and warped gradients
    Mat gradientX = Mat::zeros(hd, wd, type);
    Mat gradientY = Mat::zeros(hd, wd, type);
    Mat gradientXWarped = Mat(hs, ws, type);
    Mat gradientYWarped = Mat(hs, ws, type);

    // calculate first order image derivatives
    Matx13f dx(-0.5f, 0.0f, 0.5f);

    filter2D(imageFloat, gradientX, -1, dx);
    filter2D(imageFloat, gradientY, -1, dx.t());

    //to use it for mask warping
    Mat preMask = Mat::ones(hd, wd, CV_8U);

    // matrices needed for solving linear equation system for maximizing ECC
    Mat jacobian = Mat(hs, ws * numberOfParameters, type);
    Mat hessian = Mat(numberOfParameters, numberOfParameters, CV_32F);
    Mat hessianInv = Mat(numberOfParameters, numberOfParameters, CV_32F);
    Mat imageProjection = Mat(numberOfParameters, 1, CV_32F);
    Mat templateProjection = Mat(numberOfParameters, 1, CV_32F);
    Mat imageProjectionHessian = Mat(numberOfParameters, 1, CV_32F);
    Mat errorProjection = Mat(numberOfParameters, 1, CV_32F);

    Mat deltaP = Mat(numberOfParameters, 1, CV_32F);  // transformation parameter correction
    Mat error = Mat(hs, ws, CV_32F);                  // error as 2D matrix

    const int imageFlags = INTER_LINEAR + WARP_INVERSE_MAP;
    const int maskFlags = INTER_NEAREST + WARP_INVERSE_MAP;

    // iteratively update map_matrix
    double rho = -1;
    double last_rho = -termination_eps;
    for (int i = 1; (i <= numberOfIterations) && (fabs(rho - last_rho) >= termination_eps); i++) {
        // warp-back portion of the inputImage and gradients to the coordinate space of the templateImage
        warpPerspective(imageFloat, imageWarped, map, imageWarped.size(), imageFlags);
        warpPerspective(gradientX, gradientXWarped, map, gradientXWarped.size(), imageFlags);
        warpPerspective(gradientY, gradientYWarped, map, gradientYWarped.size(), imageFlags);
        warpPerspective(preMask, imageMask, map, imageMask.size(), maskFlags);

        Scalar imgMean, imgStd, tmpMean, tmpStd;
        meanStdDev(imageWarped, imgMean, imgStd, imageMask);
        meanStdDev(templateFloat, tmpMean, tmpStd, imageMask);

        subtract(imageWarped, imgMean, imageWarped, imageMask);  // zero-mean input
        templateZM = Mat::zeros(templateZM.rows, templateZM.cols, templateZM.type());
        subtract(templateFloat, tmpMean, templateZM, imageMask);  // zero-mean template

        int validPixels = countNonZero(imageMask);
        double tmpNorm = std::sqrt(validPixels * cv::norm(tmpStd, cv::NORM_L2SQR));
        double imgNorm = std::sqrt(validPixels * cv::norm(imgStd, cv::NORM_L2SQR));

        // calculate jacobian of image wrt parameters
        image_jacobian_homo_ECC(gradientXWarped, gradientYWarped, Xgrid, Ygrid, map, jacobian);

        // calculate Hessian and its inverse
        project_onto_jacobian_ECC(jacobian, jacobian, hessian);

        hessianInv = hessian.inv();

        const double correlation = templateZM.dot(imageWarped);

        // calculate enhanced correlation coefficient (ECC)->rho
        last_rho = rho;
        rho = correlation / (imgNorm * tmpNorm);
        if (cvIsNaN(rho)) {
            return std::nullopt;  // sr: was CV_Error(StsNoConv, "NaN encountered.")
        }

        // project images into jacobian
        project_onto_jacobian_ECC(jacobian, imageWarped, imageProjection);
        project_onto_jacobian_ECC(jacobian, templateZM, templateProjection);

        // calculate the parameter lambda to account for illumination variation
        imageProjectionHessian = hessianInv * imageProjection;
        const double lambda_n = (imgNorm * imgNorm) - imageProjection.dot(imageProjectionHessian);
        const double lambda_d = correlation - templateProjection.dot(imageProjectionHessian);
        if (lambda_d <= 0.0) {
            // sr: was CV_Error(StsNoConv, "The algorithm stopped before its convergence. ...")
            return std::nullopt;
        }
        const double lambda = (lambda_n / lambda_d);

        // estimate the update step delta_p
        error = lambda * templateZM - imageWarped;
        project_onto_jacobian_ECC(jacobian, error, errorProjection);
        deltaP = hessianInv * errorProjection;

        // update warping matrix
        update_warping_matrix_ECC(map, deltaP);

        // sr: a diverging warp must not reach warpPerspective (AD-004).
        if (!checkRange(map)) return std::nullopt;
    }

    // return final correlation coefficient
    return rho;
}

}  // namespace sr::vendor
