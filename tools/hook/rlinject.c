// rlinject.exe <dll> <exe> [args...]  - start the game suspended, load our DLL, resume, wait.
// Leaves the game install untouched (no proxy DLLs on disk).
// Build: x86_64-w64-mingw32-gcc -O1 -municode -o rlinject.exe rlinject.c
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv) {
  if (argc < 3) {
    fwprintf(stderr, L"usage: rlinject <dll> <exe> [args...]\n");
    return 2;
  }
  wchar_t dll[MAX_PATH];
  GetFullPathNameW(argv[1], MAX_PATH, dll, NULL);
  static wchar_t cmd[32768];
  int pos = swprintf(cmd, 32768, L"\"%ls\"", argv[2]);
  for (int i = 3; i < argc; i++) pos += swprintf(cmd + pos, 32768 - pos, L" %ls", argv[i]);
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(argv[2], cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
    fwprintf(stderr, L"CreateProcess failed %lu\n", GetLastError());
    return 3;
  }
  SIZE_T n = (wcslen(dll) + 1) * sizeof(wchar_t);
  void *mem = VirtualAllocEx(pi.hProcess, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  WriteProcessMemory(pi.hProcess, mem, dll, n, NULL);
  HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), mem, 0, NULL);
  if (!th) {
    fwprintf(stderr, L"CreateRemoteThread failed %lu\n", GetLastError());
    TerminateProcess(pi.hProcess, 1);
    return 4;
  }
  WaitForSingleObject(th, 30000);
  DWORD code = 0;
  GetExitCodeThread(th, &code);
  fwprintf(stderr, L"rlinject: LoadLibrary returned %lx (0 = failed)\n", code);
  ResumeThread(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  GetExitCodeProcess(pi.hProcess, &code);
  return (int)code;
}
