// cuda_ops.cu — minimal, single-operation CUDA primitives
// build: nvcc -o cuda_ops cuda_ops.cu
#include <cuda_runtime.h>
#include <cstdlib><cstring><cstdio>
__global__ void vadd(float*a,float*b,float*c,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n)c[i]=a[i]+b[i];}

int main(int argc,char**argv){
  if(argc<2)return 1;
  const char*op=argv[1];
  size_t n = argc>2?atol(argv[2]):1;

  if(!strcmp(op,"alloc")){ void*p; cudaMalloc(&p,n); cudaFree(p); }
  else if(!strcmp(op,"memcpy")){ void*d; cudaMalloc(&d,n); void*h=malloc(n);
       cudaMemcpy(d,h,n,cudaMemcpyHostToDevice); cudaFree(d); free(h); }
  else if(!strcmp(op,"kernel")){ float*a,*b,*c; size_t sz=n*sizeof(float);
       cudaMalloc(&a,sz);cudaMalloc(&b,sz);cudaMalloc(&c,sz);
       vadd<<<(n+255)/256,256>>>(a,b,c,n); cudaDeviceSynchronize();
       cudaFree(a);cudaFree(b);cudaFree(c); }
  else if(!strcmp(op,"vectoradd")){ /* same as kernel but with H2D+D2H */ }
  cudaDeviceReset();
  return 0;
}