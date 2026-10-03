#include <windows.h>
#include <shlobj.h>
#include <atlbase.h>
void MarkShortcutRunAs(LPCWSTR pszShortcut)
{
 CComPtr<IPersistFile> sppf;
 if (FAILED(sppf.CoCreateInstance(CLSID_ShellLink))) return;
 if (FAILED(sppf->Load(pszShortcut, STGM_READWRITE))) return;
 CComQIPtr<IShellLinkDataList> spdl(sppf);
 if (!spdl) return;
 DWORD dwFlags;
 if (FAILED(spdl->GetFlags(&dwFlags))) return;
 dwFlags |= SLDF_RUNAS_USER;
 if (FAILED(spdl->SetFlags(dwFlags))) return;
 if (FAILED(sppf->Save(NULL, TRUE))) return;
 wprintf(L"Succeeded\n");
}
int __cdecl wmain(int argc, wchar_t *argv[])
{
 if (argc == 2 && SUCCEEDED(CoInitialize(NULL))) {
  MarkShortcutRunAs(argv[1]);
  CoUninitialize();
 }
 return 0;
}