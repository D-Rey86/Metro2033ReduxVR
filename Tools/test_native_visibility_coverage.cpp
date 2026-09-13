#include <stdio.h>
#include <stdlib.h>
#include "../ThirdParty/3Dmigoto/DirectX11/NativeVisibilityCoverage.h"
static unsigned checks;
static void Check(bool ok) { ++checks; if (!ok) { fprintf(stderr,"FAIL %u\n",checks); exit(1); } }
int main() {
    float v[16]={1,0,0,0,0,1,0,0,0,0,1,0,12,-3,7,1};
    float p[16]={1.173614f,0,0,0,0,2.114322f,0,0,0,0,1.002f,1,0,0,-.1002f,0};
    float vp[16]={}, out[16]={}, eyes[2][16]={};
    for (unsigned e=0;e<2;++e) {
        eyes[e][0]=2/(1.4013f+.8354f); eyes[e][2]=(e?-.5659f:.5659f)/(1.4013f+.8354f);
        eyes[e][5]=1/1.3065f;
    }
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        for(unsigned k=0;k<4;++k)vp[r*4+c]+=v[r*4+k]*p[k*4+c];
    Check(NativeVisibilityCoverage::Widen(v,p,vp,eyes,out));
    for(unsigned r=0;r<4;++r)for(unsigned c=2;c<4;++c)Check(out[r*4+c]==vp[r*4+c]);
    Check(out[0]<vp[0] && out[5]<vp[5]);
    for(int degrees=0;degrees<360;++degrees) {
        float a=degrees*.01745329252f;
        for(unsigned e=0;e<2;++e)for(int x=-1;x<=1;x+=2)for(int y=-1;y<=1;y+=2) {
            float tx=(x-eyes[e][2])/eyes[e][0],ty=(y-eyes[e][6])/eyes[e][5];
            float rx=cosf(a)*tx-sinf(a)*ty,ry=sinf(a)*tx+cosf(a)*ty;
            // Camera-space unit-depth corner transformed through widened scales.
            Check(fabsf(rx*out[0])<=1.00001f && fabsf(ry*out[5])<=1.00001f);
        }
    }
    vp[0]+=.01f; Check(!NativeVisibilityCoverage::Widen(v,p,vp,eyes,out)); vp[0]-=.01f;
    p[11]=0; Check(!NativeVisibilityCoverage::Widen(v,p,vp,eyes,out)); p[11]=1;
    eyes[0][0]=0; Check(!NativeVisibilityCoverage::Widen(v,p,vp,eyes,out));
    printf("PASS: %u visibility coverage checks (offline; no engine hook)\n",checks);
}
