/* Mercenaries2Remastered - Copyright (C) 2026 the Mercenaries2Remastered authors (see AUTHORS).
 * Licensed under the GNU GPL version 3 or later with an additional permission (see LICENSE, LICENSE-EXCEPTION). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
HRESULT WINAPI DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
static unsigned char ref(unsigned long long i){ return (unsigned char)((i*2654435761u)>>13); }
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){ fails++; printf("FAIL: " __VA_ARGS__); printf("\n");} }while(0)
static void chk(const unsigned char*b, unsigned long long off, DWORD n, const char*what){
  for(DWORD i=0;i<n;i++) if(b[i]!=ref(off+i)){ CHECK(0,"%s data mismatch at %llu",what,off+i); return; } }
int main(int argc, char**argv){ if(argc>99) DirectInput8Create(0,0,0,0,0);

  FILETIME ft; GetSystemTimeAsFileTime(&ft);
  const unsigned long long S0=1000003, S1=500000, T=S0+S1;
  unsigned char *buf=malloc(T+4096);
  HANDLE h=CreateFileA("data\\vz.wad",GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
  CHECK(h!=INVALID_HANDLE_VALUE,"open");
  LARGE_INTEGER sz; GetFileSizeEx(h,&sz); CHECK((unsigned long long)sz.QuadPart==T,"GetFileSizeEx %lld",sz.QuadPart);
  WIN32_FIND_DATAA fd; HANDLE f=FindFirstFileA("data\\vz.wad",&fd); FindClose(f);
  CHECK(fd.nFileSizeLow==T,"FindFirstFile size %lu",fd.nFileSizeLow);
  /* overlapped-offset reads like the game streamer */
  unsigned long long offs[]={0, S0-10, S0-2048, S0, S0+5, T-100, 999424};
  DWORD lens[]={4096, 20, 4096, 1000, 2048, 100, 2048};
  for(int k=0;k<7;k++){ OVERLAPPED ov={0}; ov.Offset=(DWORD)offs[k]; ov.hEvent=CreateEventA(0,1,0,0); DWORD r=0;
    BOOL ok=ReadFile(h,buf,lens[k],&r,&ov); CHECK(ok&&r==lens[k],"ov read %d ok=%d r=%lu",k,ok,r); chk(buf,offs[k],r,"ov");
    CHECK(WaitForSingleObject(ov.hEvent,0)==WAIT_OBJECT_0,"event %d",k);
    DWORD r2=0; CHECK(GetOverlappedResult(h,&ov,&r2,FALSE)&&r2==r,"GOR %d r2=%lu",k,r2); CloseHandle(ov.hEvent);}
  /* short read across EOF, then read at EOF */
  { OVERLAPPED ov={0}; ov.Offset=(DWORD)(T-50); DWORD r=0; BOOL ok=ReadFile(h,buf,4096,&r,&ov); CHECK(ok&&r==50,"eof short ok=%d r=%lu",ok,r);
    ov.Offset=(DWORD)T; ok=ReadFile(h,buf,4096,&r,&ov); CHECK(!ok&&GetLastError()==ERROR_HANDLE_EOF&&r==0,"at eof ok=%d err=%lu",ok,GetLastError()); }
  /* sync SetFilePointer + ReadFile */
  DWORD p=SetFilePointer(h,(LONG)(S0-7),NULL,FILE_BEGIN); CHECK(p==S0-7,"sfp");
  DWORD r=0; ReadFile(h,buf,100,&r,NULL); CHECK(r==100,"sync r=%lu",r); chk(buf,S0-7,100,"sync");
  ReadFile(h,buf,100,&r,NULL); chk(buf,S0+93,100,"sync2");
  p=SetFilePointer(h,0,NULL,FILE_CURRENT); CHECK(p==S0+193,"cur %lu",p);
  p=SetFilePointer(h,-10,NULL,FILE_END); CHECK(p==T-10,"end %lu",p);
  /* full sequential read in 1MB chunks */
  SetFilePointer(h,0,NULL,FILE_BEGIN); unsigned long long tot=0;
  for(;;){ ReadFile(h,buf+tot,1<<20,&r,NULL); if(!r)break; tot+=r; } CHECK(tot==T,"seq tot %llu",tot); chk(buf,0,(DWORD)tot,"seq");
  CloseHandle(h);
  /* unbuffered handle */
  h=CreateFileA("data\\vz.wad",GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING,0);
  unsigned char*al=(unsigned char*)(((ULONG_PTR)buf+4095)&~4095);
  { OVERLAPPED ov={0}; ov.Offset=(DWORD)(S0&~2047); BOOL ok=ReadFile(h,al,4096,&r,&ov); CHECK(ok&&r==4096,"nobuf ok=%d r=%lu",ok,r); chk(al,S0&~2047,r,"nobuf"); }
  CloseHandle(h);
  /* non-split file untouched */
  h=CreateFileA("data\\other.bin",GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0); GetFileSizeEx(h,&sz); CHECK(sz.QuadPart==S0,"other size"); CloseHandle(h);
  printf("plugin marker: %s\n", GetEnvironmentVariableA("M2R_TEST_PLUGIN",(char*)buf,64)?(char*)buf:"(none)");
  printf(fails?"TESTS FAILED (%d)\n":"ALL TESTS PASSED\n",fails);
  return fails;
}
