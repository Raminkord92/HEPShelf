Unicode true
!include "MUI2.nsh"

!ifndef APP_VERSION
  !error "APP_VERSION must be supplied"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR must be supplied"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE must be supplied"
!endif

Name "HEPShelf ${APP_VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\HEPShelf"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install HEPShelf" MainSection
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\HEPShelf"
  CreateShortcut "$SMPROGRAMS\HEPShelf\HEPShelf.lnk" "$INSTDIR\hepshelf.exe"
  CreateShortcut "$SMPROGRAMS\HEPShelf\Uninstall HEPShelf.lnk" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "DisplayName" "HEPShelf"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "Publisher" "HEPShelf contributors"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\HEPShelf\HEPShelf.lnk"
  Delete "$SMPROGRAMS\HEPShelf\Uninstall HEPShelf.lnk"
  RMDir "$SMPROGRAMS\HEPShelf"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\HEPShelf"
  RMDir /r "$INSTDIR"
SectionEnd
