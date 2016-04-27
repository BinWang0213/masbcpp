/*
Copyright (c) 2016 Ravi Peters

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// #define VERBOSEPRINT 1;
// #define WITH_OPENMP 1;

#include <iostream>
#include <fstream>
#include <string>
#include <limits>

// OpenMP
#ifdef WITH_OPENMP
    #include <omp.h>
#endif

// Vrui
#include <vrui/Geometry/ComponentArray.h>
#include <vrui/Math/Math.h>
#ifndef __MINGW32__
#include <vrui/Misc/Timer.h>
#endif
// kdtree2
#include <kdtree2/kdtree2.hpp>
// cnpy
#include <cnpy/cnpy.h>
// tclap
#include <tclap/CmdLine.h>

// typedefs
#include "types.h"

// globals
Scalar initial_radius;
bool nan_for_initr;
double denoise_preserve;
double denoise_planar;
const Scalar delta_convergance = 1E-5;
const unsigned int iteration_limit = 30;
const Point nanPoint( std::numeric_limits<Scalar>::quiet_NaN() );

inline Scalar compute_radius(Point &p, Vector &n, Point &q)
{
    // this is basic goniometry
    double d = Geometry::mag(p-q);
    Scalar cos_theta = ( n * (p-q) ) / d;
    return d/(2*cos_theta);
}

inline Scalar cos_angle(Vector p, Vector q)
{
    // Calculate the cosine of angle between vector p and q, see http://en.wikipedia.org/wiki/Law_of_cosines#Vector_formulation
    Scalar result = p*q / ( Geometry::mag(p) * Geometry::mag(q) );
    if( result > 1 ) return 1;
    else if( result < -1 ) return -1;
    return result;
}

ma_result sb_point(Point &p, Vector &n, kdtree2::KDTree* kd_tree)
{
    unsigned int j=0;
    Scalar r, r_previous = 0;
    Point q, c_next;
    int qidx = -1, qidx_next;
    Point c = p - n * initial_radius;

    while (1) 
    {
        #ifdef VERBOSEPRINT
        std::cout << "\nloop iteration: " << j << ", p = (" << p[0] << "," << p[1] << "," << p[2] << ", n = (" << n[0] << "," << n[1] << "," << n[2] << ") \n";

        std::cout << "c = (" << c[0] << "," << c[1] << "," << c[2] << ")\n";
        #endif

        // find closest point to c
        kdtree2::KDTreeResultVector result;
        kd_tree->n_nearest(c,2,result);
        
        qidx_next = result[0].idx;
        q = kd_tree->the_data[ qidx_next ];

        #ifdef VERBOSEPRINT
        std::cout << "q = (" << q[0] << "," << q[1] << "," << q[2] << ")\n";
        #endif

        // handle case when q==p
        if( q == p )
        {
            // 1) if r_previous==SuperR, apparantly no other points on the halfspace spanned by -n => that's an infinite ball
            if( r_previous == initial_radius )
            {
                r = initial_radius;
                c = nan_for_initr ? nanPoint : p - n * r;
                break;
            // 2) otherwise just pick the second closest point
            } else {
                qidx_next = result[1].idx;
                q = kd_tree->the_data[ qidx_next ];
            }
        }

        // compute radius
        r = compute_radius(p,n,q);

        #ifdef VERBOSEPRINT
        std::cout << "r = " << r << "\n";
        #endif

        // if r < 0 closest point was on the wrong side of plane with normal n => start over with SuperRadius on the right side of that plane
        if( r < 0 )
            r = initial_radius;
        // if r > SuperR, stop now because otherwise in case of planar surface point configuration, we end up in an infinite loop
        else if( r > initial_radius )
        {
            r = initial_radius;
            c = nan_for_initr ? nanPoint : p - n * r;
            break;
        }

        // compute next ball center
        c_next = p - n * r;

        // denoising
        if( denoise_preserve or denoise_planar )
        {
            Scalar a = cos_angle(p-c_next, q-c_next);
            Scalar separation_angle = Math::acos(a);

            if( denoise_preserve && ( separation_angle < denoise_preserve && j>0 && r > Geometry::mag(q-p) ) )
            {
                // keep previous radius:
                r = r_previous;
                // qidx = qidx_next;
                break;
            }
            if( denoise_planar && ( separation_angle < denoise_planar && j==0 ) )
            {
                r = initial_radius;
                c = nan_for_initr ? nanPoint : p - n * r;
                // qidx = qidx_next;
                break;
            }
        }

        // stop iteration if r has converged
        if( Math::abs(r_previous-r) < delta_convergance )
            break;

        // stop iteration if this looks like an infinite loop:
        if( j > iteration_limit )
            break;

        r_previous = r;
        c = c_next;
        qidx = qidx_next;
        j++;
    }
        
    return {c, qidx};
}

void sb_points(PointList &points, VectorList &normals, kdtree2::KDTree* kd_tree, PointList &ma_coords, int* ma_qidx, bool inner=1)
{
    Point p;
    Vector n;

    #pragma omp parallel for private(p, n)
    for( int i=0; i<points.size(); i++ )
    {
        p = points[i];
        if( inner )
            n = normals[i];
        else
            n = -normals[i];
        ma_result r = sb_point(p, n, kd_tree);
        ma_coords[i] = r.c;
        ma_qidx[i] = r.qidx;
    }
    // return ma_coords;
}


int main(int argc, char **argv)
{
    // parse command line arguments
    try {
        TCLAP::CmdLine cmd("Computes a MAT point approximation, see also https://github.com/tudelft3d/masbcpp", ' ', "0.1");

        TCLAP::UnlabeledValueArg<std::string> inputArg( "input", "path to directory with inside it a 'coords.npy' and a 'normals.npy' file. Both should be Nx3 float arrays where N is the number of input points.", true, "", "input dir", cmd);
        TCLAP::UnlabeledValueArg<std::string> outputArg( "output", "path to output directory", false, "", "output dir", cmd);

        TCLAP::ValueArg<double> denoise_preserveArg("d","preserve","denoise preserve threshold",false,20,"double", cmd);
        TCLAP::ValueArg<double> denoise_planarArg("p","planar","denoise planar threshold",false,32,"double", cmd);
        TCLAP::ValueArg<double> initial_radiusArg("r","radius","initial ball radius",false,200,"double", cmd);
        
        TCLAP::SwitchArg nan_for_initrSwitch("a","nan","write nan for points with radius equal to initial radius", cmd, false);
        TCLAP::SwitchArg reorder_kdtreeSwitch("N","no-kdtree-reorder","Don't reorder kd-tree points: slower computation but lower memory use", cmd, true);

        cmd.parse(argc,argv);
        
        initial_radius = initial_radiusArg.getValue();
        denoise_preserve = (M_PI/180) * denoise_preserveArg.getValue();
        denoise_planar = (M_PI/180) * denoise_planarArg.getValue();
        
        nan_for_initr = nan_for_initrSwitch.getValue();
        bool kd_tree_reorder = reorder_kdtreeSwitch.getValue();

        std::string output_path = inputArg.getValue();
        if(outputArg.isSet())
            output_path = outputArg.getValue();
        std::replace(output_path.begin(), output_path.end(), '\\', '/');

        // check for proper in-output arguments and set in and output filepath strings
        std::string input_coords_path = inputArg.getValue()+"/coords.npy";
        std::replace(input_coords_path.begin(), input_coords_path.end(), '\\', '/');
        std::string input_normals_path = inputArg.getValue()+"/normals.npy";
        std::replace(input_normals_path.begin(), input_normals_path.end(), '\\', '/');
        std::string output_path_ma_in = output_path+"/ma_coords_in.npy";
        std::string output_path_ma_out = output_path+"/ma_coords_out.npy";
        std::string output_path_ma_q_in = output_path+"/ma_qidx_in.npy";
        std::string output_path_ma_q_out = output_path+"/ma_qidx_out.npy";
        {
            std::ifstream infile(input_coords_path.c_str());
            if(!infile)
                throw TCLAP::ArgParseException("invalid filepath", inputArg.getValue());
        }
        {
            std::ifstream infile(input_normals_path.c_str());
            if(!infile)
                throw TCLAP::ArgParseException("invalid filepath", inputArg.getValue());
        }
        {
            std::ofstream outfile(output_path_ma_in.c_str());    
            if(!outfile)
                throw TCLAP::ArgParseException("invalid filepath", output_path);
        }

	   	std::cout << "Parameters: denoise_preserve="<<denoise_preserveArg.getValue()<<", denoise_planar="<<denoise_planarArg.getValue()<<", initial_radius="<<initial_radius<<"\n";
	    
	    cnpy::NpyArray coords_npy = cnpy::npy_load( input_coords_path.c_str() );
	    float* coords_carray = reinterpret_cast<float*>(coords_npy.data);

	    unsigned int num_points = coords_npy.shape[0];
	    unsigned int dim = coords_npy.shape[1];
	    PointList coords(num_points);
	    for ( unsigned int i=0; i<num_points; i++) coords[i] = Point(&coords_carray[i*3]);
	    coords_npy.destruct();

	    cnpy::NpyArray normals_npy = cnpy::npy_load( input_normals_path.c_str() );
	    float* normals_carray = reinterpret_cast<float*>(normals_npy.data);
	    VectorList normals(normals_npy.shape[0]);
	    for ( unsigned int i=0; i<num_points; i++) normals[i] = Vector(&normals_carray[i*3]);
	    normals_npy.destruct();
	    
        #ifndef __MINGW32__
	    Misc::Timer t0;
        #endif
	    kdtree2::KDTree* kd_tree;
	    kd_tree = new kdtree2::KDTree(coords,kd_tree_reorder);
	    kd_tree->sort_results = true;
        #ifndef __MINGW32__
	    t0.elapse();
	    std::cout<<"Constructed kd-tree in "<<t0.getTime()*1000.0<<" ms"<<std::endl;
        #endif

	    // omp_set_num_threads(4);

	    {
            PointList ma_coords_in(coords.size());
            int* ma_qidx_in = new int[num_points];

	        sb_points(coords, normals, kd_tree, ma_coords_in, ma_qidx_in, 1);
            #ifndef __MINGW32__
	        t0.elapse();
	        std::cout<<"Done shrinking interior balls, took "<<t0.getTime()*1000.0<<" ms"<<std::endl;
            #endif
	    
	        Scalar* ma_coords_in_carray = new Scalar[num_points*3];   
	        for (int i=0; i<ma_coords_in.size(); i++)
	            for (int j=0; j<3; j++)
	                ma_coords_in_carray[i*3+j] = ma_coords_in[i][j];
	    
	        const unsigned int c_size = ma_coords_in.size();
	        const unsigned int shape[] = {c_size,3};
            cnpy::npy_save(output_path_ma_in.c_str(), ma_coords_in_carray, shape, 2, "w");
            const unsigned int shape_[] = {c_size};
	        cnpy::npy_save(output_path_ma_q_in.c_str(), ma_qidx_in, shape_, 1, "w");
	    }

	    {
            PointList ma_coords_out(coords.size());
            int* ma_qidx_out = new int[num_points];
            sb_points(coords, normals, kd_tree, ma_coords_out, ma_qidx_out, 0);
            #ifndef __MINGW32__
	        t0.elapse();
	        std::cout<<"Done shrinking exterior balls, took "<<t0.getTime()*1000.0<<" ms"<<std::endl;
	        #endif
            
	        Scalar* ma_coords_out_carray = new Scalar[num_points*3];
	        for (int i=0; i<ma_coords_out.size(); i++)
	            for (int j=0; j<3; j++)
	                ma_coords_out_carray[i*3+j] = ma_coords_out[i][j];

	        const unsigned int c_size = ma_coords_out.size();
	        const unsigned int shape[] = {c_size,3};
	        cnpy::npy_save(output_path_ma_out.c_str(), ma_coords_out_carray, shape, 2, "w");
            const unsigned int shape_[] = {c_size};
            cnpy::npy_save(output_path_ma_q_out.c_str(), ma_qidx_out, shape_, 1, "w");
	    }

	} catch (TCLAP::ArgException &e) { std::cerr << "Error: " << e.error() << " for " << e.argId() << std::endl; }

    return 0;
}
