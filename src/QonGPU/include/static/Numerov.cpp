#include <vector>
#include <iostream>
#include <omp.h>
#include "Numerov.hpp"
#include <cmath>

static void HandleError( cudaError_t err,
                         const char *file,
                         int line ) {
    if (err != cudaSuccess) {
        printf( "%s in %s at line %d\n", cudaGetErrorString( err ),
                file, line );
        exit( EXIT_FAILURE );
    }
}
#define HANDLE_ERROR( err ) (HandleError( err, __FILE__, __LINE__ ))

Numerov::Numerov():nx(0),ne(0),xmax(0),xmin(0){}

Numerov::Numerov(Params1D *pa): param(pa),
                                chunk(pa->getnx()*CHUNKSIZE),
                                cache(pa->getnx()*CHUNKSIZE),
                                nx(pa->getnx()),
                                ne(pa->getne()),
                                z(pa->getz()),
                                xmax(pa->getxmax()),
                                xmin(pa->getxmin())
                                 {

    // initialize the cache, with the inital values
    for(auto it = cache.begin(); it != cache.end(); it += nx) {
        // the first entry always has to be zero, the second
        // is an arbitrary small value
        *(it) = 0;
        *(it+1) = 1e-7;
    }
    //get the minimal energy E_n is in [V(0,0,z),0]
    //We'll define it as positive
    //Kernel code then uses negaive values!
    E = V(0,0,z);
}

Numerov::~Numerov(){}

void Numerov::solve(){
    // This Loop is used to create
    for(auto j = 1; j < 2; j++) {
        
        z = j;
        DEBUG2("Solving for Z ="<<z);
        double *dev_ptr;
	
        int dev_ne = CHUNKSIZE;
        // Make use of some local variables
        int index = 0;
        double dE =  V(0, 0, z) / (double) ne;
        // This will be the starting energy for each chunk calculation
        double En = 0.0;
        int numlvl = 1;
		
		HANDLE_ERROR(cudaMalloc((void **) &dev_ptr, sizeof(double) * nx * dev_ne));
		
		cudaHostRegister(chunk.data(),  sizeof(double) * nx * dev_ne, cudaHostRegisterMapped);
		cudaHostRegister(cache.data(),  sizeof(double) * nx * dev_ne, cudaHostRegisterMapped);
        
        cudaEvent_t start, stop;
		cudaEventCreate(&start);
		cudaEventCreate(&stop);
        float t_el = 0;
        
        while (index < ne) {
            
            //copy initals on device
            dev_ne = CHUNKSIZE;
            
            if (index + CHUNKSIZE > ne) {
				
				
                DEBUG2("Changing Device memory");
                dev_ne = ne - index;
                cudaFree( dev_ptr);
                cudaMalloc( (void **) &dev_ptr, sizeof(double) * nx * dev_ne);
            }
            
            DEBUG2("Calculating chunk: " << index / CHUNKSIZE);
            
            HANDLE_ERROR(cudaMemcpy(dev_ptr,
                                    cache.data(),
                                    sizeof(double) * nx * dev_ne,
                                    cudaMemcpyHostToDevice));
            
            
            En = dE * (double) index -0.2748;
            DEBUG2("Calculating with Energy: "<<En);
            iter1 <<< 1024, 8 >>> (dev_ptr, nx, dev_ne, xmax, xmin, z, En, dE);
			
            HANDLE_ERROR(cudaMemcpy(chunk.data(), dev_ptr, sizeof(double) * nx * dev_ne, cudaMemcpyDeviceToHost));
            
            
            
            bisect(En, numlvl) index = ne;

			
            index += CHUNKSIZE;

        }
        cudaFree(dev_ptr);
    }
    // After all the calculations done we can save our energy levels!
    prepstates();
    savelevels();

}

void Numerov::savelevels(){
    // Function to provide saving functionality of the energy Levels
    // First Allocate an appropiate vector

    DEBUG2(res.size()/nx);

    hid_t file_id;
    vector<double> buffer2(eval.size());

    for(auto it = 0u; it < eval.size(); it++) {
        // We do the analog thing for the enegy
        // It's a lot simpler!
        buffer2[it] = eval.at(it);
        
    }
    // Create a new HDF5 file

    file_id = H5Fcreate("EnergyLevels.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

    hsize_t dims = res.size();

    // Create a HDF5 Data set and write buffer1
    H5LTmake_dataset(file_id, "/numres", 1, &dims, H5T_NATIVE_DOUBLE, res.data());
    
    // Analog for buffer2
    dims = buffer2.size();
    H5LTmake_dataset(file_id, "/evals", 1, &dims, H5T_NATIVE_DOUBLE, buffer2.data());
    
    //Save some necessary parameters
    vector<double> p(3);
    p[0] = nx;
    p[1] = ne;
    p[2] = xmax;
    
	// Call the library
    dims = 3 ;
    H5LTmake_dataset( file_id, "/params", 1, &dims, H5T_NATIVE_DOUBLE, p.data());
    
    // close the file
    H5Fclose(file_id);
    
    
}


bool Numerov::sign(double s){
    return std::signbit(s);
}



int Numerov::bisect(double j, int& numelvl) {

    // Iterate through chunk data
    // create local variable for the offset
    // off is the index of the last Element
    const int off = nx;
    auto it = chunk.begin();
    vector<double> temp( nx);
    double dE =  V(0,0,z)/ne;
    double En = 0;

//#pragma omp parallel for
    for( auto i = 2 * off - 1 ; i < chunk.size(); i += nx){

        //DEBUG2("1. chunk[i] = "<< chunk[i]);
        //DEBUG2("2. chunk[i-nx] = " << chunk[i - nx]);
        if(sign( chunk[ i ]) != sign( chunk[ i - nx])){

            if( (fabs( chunk[ i]) < fabs( chunk[i - nx])) /*&& (abs(chunk[i])<1e-3)*/) {

                res.resize(res.size()+nx);
                auto iter = res.end()-nx;
                std::copy(it+i,it+i+nx,iter);
                En = ( j + i/nx * dE);
                std::cout << "Energy level found" << std::endl;
                std::cout << "Detected energy level: "<< En << std::endl;
                eval.push_back(En);
                DEBUG2(eval.size());
                DEBUG2("Checked element: "<< chunk[i]);
                if(numelvl == 1)
                    return 1;
                numelvl += 1;

            }
            else {

                // if(abs(chunk[i-nx])<1e-3) {
                    res.resize(res.size()+nx);
                    auto iter = res.end()-nx+1;
                    std::copy(it+i,it+i+nx,iter);
                    En = (j + (i/nx) * dE);

                    std::cout << "Energy level found" << std::endl;
                    std::cout << "Detected energy level: "<< En << std::endl;
                    DEBUG2("Checked element: "<< chunk[i-nx]);
                    
                    eval.push_back(En);
                    DEBUG2(eval.size());
                    if(numelvl == 1)
                        return 1;
                    numelvl += 1;

               // }

            }

        }

    }
    return 0;
}


double Numerov::trapez(int first, int last) {

    double h = (xmax - xmin) / (double) nx;
    auto temp = 0.0;

    for(auto i = first; i < last; ++i){

        temp += res[i]*res[i];

    }

    temp  *= 2.0;
    temp -= (res[first]*res[first] + res[last]*res[last]);
    temp *= h/2;
    return 1/sqrt(temp);

}


void Numerov::mult_const(int first, int last, double c) {

    for( auto i = first; i < last; ++i) {

        res[i] *= c;

    }
}


void Numerov::norm_corr( double* psi) {
	
	double h = (xmax - xmin) / (double) nx;
    auto temp = 0.0;

    for(auto i = 0u; i < nx; ++i){

        temp += psi[i]*psi[i];

    }

    temp *= 2.0;
    temp -= (psi[0]*psi[0] + psi[nx] * psi[nx]);
    temp *= h / 2.0;
    
    temp = 1/sqrt(temp);
    
    for( auto i = 0u; i < nx; i++) {
		
		psi[i] *= temp;
		
	}
    
}


void Numerov::prepstates() {

    // This function normalizes the
    // static Solutions of the Numerov solution and
    // Writes them to the

    double c_temp = 0;

    for(auto i = 0u; i < res.size(); i += nx+1) {

        c_temp = trapez(i, i+nx-1);
        mult_const( i, i+nx, c_temp);

    }

}


void Numerov::copystate(int ind, thrust::host_vector<cuDoubleComplex>& v) {

	// get index inside the results array
    int o = nx*ind;

	// Define vectors to copy values in, since casting to cuDoubleComplex
	// is not that trivial!
    thrust::host_vector<double> real(nx);
    thrust::host_vector<double> imag(nx);

	// copy results to real part
    thrust::copy(res.begin()+o, res.begin() + o + nx, real.begin());

	
	double E = eval.at( ind);
    double tmax = param->gettmax();
    double tmin = param->gettmin();
    int nt = param->getnt();
    double dt = (tmax - tmin)/nt;

    double t0 = tmin + 1e4*dt;
    

    real[0] = 0;
    for(auto i = 0u; i < real.size(); i++) {
        //real[i] = cos(E * t0) * real[i];

        imag[i] = 0;//-sin(E * t0) * real[i];
    }
    for(auto i = 0u; i < v.size(); i++) {

        v[i] = make_cuDoubleComplex(real[i], imag[i]);

    }
    
    double h = (xmax - xmin)/nx;
    double loc = xmax - 100*h;
    v[1] = make_cuDoubleComplex(0, 0);
    
    thrust::host_vector<double> corr(nx);
    corr[ nx-1] = 0; 
    
    double pre = real[nx-2]/abs(real[nx-2]);
    
    corr[ nx-2] = pre * 1e-7;
    
    double* psi = thrust::raw_pointer_cast( corr.data());
    
    corr_wf(psi, nx, xmax, xmin, eval[ind], z);  
    
    
    
	
    // The next thing is to find a suitable index to 
    // unify both solutions
    
    norm_corr(psi);
    
    hid_t file = H5Fcreate("corr_wf.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims = nx;
    H5LTmake_dataset( file, "/numres", 1, &dims, H5T_NATIVE_DOUBLE,
                      psi);
    
   
    int unclose = 1; 
    int cindex = 0;
    int count = nx-1; 
    
    
    while( unclose) {
		
		if( fabs(corr[count] - real[count]) < 1e-8 ) {
			
			cindex  = count;
			unclose = 0;
		}
		
		count -= 1;
	 	
	}
    
    
    for( int i = cindex;  i < nx; i++) {
		
		real[i] = corr[ i];
		
	}
    
    norm_corr(thrust::raw_pointer_cast( real.data()));
	
	for( int i = 0; i < real.size(); i++) {
		
		v[i] = make_cuDoubleComplex( real[i], 0);
		
	}
    
    
    
    
    param->seten( eval[ind]);

}
