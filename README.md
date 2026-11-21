# ExtendedExplorer

탭과 분할 창을 갖춘 Windows 10/11용 파일 탐색기. 바탕화면이나 다른 앱에서 연 폴더를
기본 파일 탐색기 대신 엽니다.

- **탭** (Win11 형태) — 탭마다 독립된 분할 레이아웃
- **파일 목록 분할** — 좌우/상하로 원하는 만큼 나누고 경계를 끌어 크기 조절
- **Win10 형태의 리본과 네비게이션 바**, 사이드바(탐색 창), 상태 표시줄
- **미리보기 창** (텍스트·이미지·PDF·Office 등, 별도 스레드) / **세부 정보 창**
- 어두운 테마, Ctrl+휠로 아이콘·썸네일 크기 조절, 마우스 엄지 버튼 뒤로/앞으로
- 세션 자동 저장·복원 (탭, 분할, 경로)

파일 목록은 OS의 `IExplorerBrowser`를 그대로 쓰므로 컨텍스트 메뉴, 드래그&드롭, 썸네일,
검색 등은 기본 탐색기와 같습니다. 자세한 명세는 [SPEC.md](SPEC.md), 수동 테스트 항목은
[TESTS.md](TESTS.md)를 보세요.

## 단축키

| 기능 | 키 |
|---|---|
| 좌우 / 상하 분할 | Ctrl+R / Ctrl+Shift+R |
| 현재 Pane 닫기 | Ctrl+W (마지막 Pane이면 탭이 닫힘) |
| 다음 / 이전 Pane | F6 / Shift+F6 |
| 방향으로 Pane 이동 | Ctrl+Alt+방향키 |
| 새 탭 / 탭 닫기 / 탭 순환 | Ctrl+T / Ctrl+F4 / Ctrl+Tab |
| 뒤로 / 앞으로 / 위로 | Alt+← / Alt+→ / Alt+↑ |

그 밖의 단축키는 기본 탐색기와 같습니다.

## 빌드

Visual Studio 2022 (C++ 데스크톱 개발, Windows SDK)가 필요합니다.

```bat
build.bat          :: build\ExtendedExplorer.exe
build.bat test     :: 앱 + selftest 빌드 후 selftest 실행
```

## 기본 파일 관리자로 등록

관리자 권한 없이 HKCU에만 씁니다. 해제하면 등록 전 상태로 되돌립니다.

```powershell
powershell -ExecutionPolicy Bypass -File register.ps1             # 등록
powershell -ExecutionPolicy Bypass -File register.ps1 -Unregister # 해제
```

- 등록하면 exe를 `%LOCALAPPDATA%\Programs\ExtendedExplorer`에 복사하고, 폴더·드라이브의
  기본 동작(더블클릭)을 ExtendedExplorer로 바꿉니다. 새로 빌드한 뒤에는 다시 실행해야
  설치본이 갱신됩니다.
- **바탕화면이나 다른 앱**에서 연 폴더 → ExtendedExplorer
- **파일 탐색기 창 안**에서 연 폴더 → 그 탐색기 창에서 그대로 열림
- 내 PC·제어판 같은 **가상 폴더**와, 앱이 띄우지 못한 폴더 → 파일 탐색기로 넘김
- `Folder\shell\open`은 건드리지 않습니다. 이를 바꾸면 파일 탐색기 창이 "작업 중..."에서 멈춥니다.

## 설정

`%APPDATA%\ExtendedExplorer\settings.ini` (세션, 창 위치, 테마 등).
