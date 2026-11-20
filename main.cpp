// ExtendedExplorer
// Phase 1: 창 + 탭 스트립(Win11) + 분할 트리(Pane = IExplorerBrowser) + 단축키 + Pane별 히스토리
// Phase 2: 네비게이션 바(Win10) + 상태 표시줄 + 사이드바(INameSpaceTreeControl)
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <shdispid.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <structuredquery.h>
#include <uiribbon.h>
#include <uiribbonpropertyhelpers.h>
#include <propvarutil.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include "ribbon_ids.h"     // build\ — uicc가 ribbon.xml에서 생성
#include <initguid.h>
#include <propkey.h>
#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "msimg32.lib")   // AlphaBlend (썸네일)
#pragma comment(lib, "dwmapi.lib")    // 어두운 제목 표시줄
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")  // RegGetValueW (시스템 앱 모드)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- 상수 ------------------------------------------------------------------
enum { DIR_V = 0, DIR_H = 1 };   // V: 좌/우 분할, H: 상/하 분할 (SPEC §4)
static const int GAP    = 2;     // Pane 간격 2px
static const int HITPAD = 3;     // 분할선 히트 영역 확장
static const UINT WM_APP_ENDADDR = WM_APP + 1;
static const UINT WM_APP_RIBBON  = WM_APP + 2;   // 리본 명령은 콜백 밖에서 실행 (모달 대화상자·창 닫기 안전)
static const UINT WM_APP_ENDTAB  = WM_APP + 3;   // 탭 이름 편집 상자가 포커스를 잃음
static const UINT WM_APP_NAV     = WM_APP + 4;   // wp=Node*, lp=PIDL(받는 쪽이 해제). 셸 콜백 밖에서 이동
static const ULONG_PTR COPY_OPEN = 0x4545;       // WM_COPYDATA: 다른 프로세스가 폴더를 새 탭으로 열어 달라고 함

// 색 토큰. SPEC §12: 시스템 라이트/다크를 따라 ApplyPalette가 바꾼다.
struct Palette { COLORREF strip, active, hover, text, dim, sep, gap, box, border, hot, hot2; };
static const Palette kLight = {
    RGB(232, 232, 232), RGB(249, 249, 249), RGB(240, 240, 240), RGB(26, 26, 26), RGB(160, 160, 160),
    RGB(200, 200, 200), RGB(225, 225, 225), RGB(255, 255, 255), RGB(217, 217, 217),
    RGB(229, 243, 255), RGB(204, 232, 255),   // Win10 탐색기 hover
};
static const Palette kDark = {
    RGB(32, 32, 32), RGB(43, 43, 43), RGB(50, 50, 50), RGB(255, 255, 255), RGB(120, 120, 120),
    RGB(70, 70, 70), RGB(20, 20, 20), RGB(25, 25, 25), RGB(70, 70, 70),
    RGB(65, 65, 65), RGB(85, 85, 85),
};
static COLORREF C_STRIP, C_ACTIVE, C_HOVER, C_TEXT, C_DIM, C_SEP, C_GAP, C_BOX, C_BORDER, C_HOT, C_HOT2;
static bool g_dark = false;
static void ApplyPalette(const Palette& p) {
    C_STRIP = p.strip; C_ACTIVE = p.active; C_HOVER = p.hover; C_TEXT = p.text; C_DIM = p.dim; C_SEP = p.sep;
    C_GAP = p.gap; C_BOX = p.box; C_BORDER = p.border; C_HOT = p.hot; C_HOT2 = p.hot2;
}
static struct PaletteInit { PaletteInit() { ApplyPalette(kLight); } } g_paletteInit;

// Segoe MDL2 Assets 글리프
#define G_BACK    L"\xE72B"
#define G_FWD     L"\xE72A"
#define G_UP      L"\xE74A"
#define G_DOWN    L"\xE96E"
#define G_RIGHT   L"\xE970"
#define G_REFRESH L"\xE72C"
#define G_SEARCH  L"\xE721"

// ---- 트리 ------------------------------------------------------------------
struct Tab;
struct Events;

struct Node {
    bool  isPane = true;
    Node* parent = nullptr;
    RECT  rc = {};
    // Pane
    Tab*  tab = nullptr;
    HWND  host = nullptr;            // 테두리를 그리는 컨테이너. IExplorerBrowser는 이 안에 산다.
    IExplorerBrowser* eb = nullptr;
    Events* ev = nullptr;
    DWORD cookie = 0;
    IConnectionPoint* cp = nullptr;  // 셸 뷰의 선택/내용 변경 이벤트
    DWORD cpCookie = 0;
    std::vector<PIDLIST_ABSOLUTE> hist;   // SPEC §14: travel log를 직접 관리
    int   hi = -1;                   // 현재 위치
    int   pend = -1;                 // 뒤로/앞으로로 가는 중인 인덱스
    std::wstring query;              // 검색 상자 내용 (Pane별)
    bool  searching = false;         // 검색 결과로 가는 중
    PIDLIST_ABSOLUTE scope = nullptr;   // 검색 범위 (결과 안에서 다시 검색해도 원래 폴더 기준)
    // Split
    int   dir = DIR_V;
    float ratio = 0.5f;
    Node* a = nullptr;
    Node* b = nullptr;
    RECT  sp = {};
};

struct Tab {
    Node* root = nullptr;
    Node* focus = nullptr;           // 항상 leaf
    std::wstring name;
    bool  fixedName = false;         // SPEC §5: 사용자가 이름을 정하면 자동으로 바뀌지 않는다
    HICON icon = nullptr;
};

static HINSTANCE g_inst;
static HWND  g_main = nullptr;
static HFONT g_font = nullptr, g_glyph = nullptr, g_glyphSm = nullptr;
static int   g_fontH = 16;
static UINT  g_dpi = 96;
static ATOM  g_paneAtom = 0;
static std::vector<Tab*> g_tabs;
static int   g_cur = -1;
static Node* g_drag = nullptr;       // 드래그 중인 분할선
static int   g_tabDrag = -1;         // 드래그 중인 탭
static bool  g_sideDrag = false;     // 사이드바 경계 드래그 중
static int   g_hoverTab = -1, g_hoverPart = -1, g_hoverNav = 0;
static HWND  g_lastFocus = nullptr;
static HWND  g_addrEdit = nullptr, g_searchEdit = nullptr;
static bool  g_addrEditing = false;
static int   g_menuArrow = -1;       // 드롭다운이 열려 있는 브레드크럼 화살표
static std::wstring g_status1, g_status2;
// 사이드바 (창 전역, SPEC §4)
static INameSpaceTreeControl* g_tree = nullptr;
static HWND  g_treeWnd = nullptr;
static DWORD g_treeCookie = 0;
static bool  g_sideOn = true;
static int   g_sideW96 = 220;        // 96dpi 기준 폭
static DWORD g_treeInputTick = 0;    // 사용자가 사이드바를 마지막으로 클릭/키 입력한 시각
// 미리 보기 / 세부 정보 창 (SPEC §3: 창 전역 1개, 분할 영역 오른쪽, 내용은 포커스 Pane 선택)
enum { INFO_NONE = 0, INFO_PREVIEW = 1, INFO_DETAILS = 2 };
static int   g_infoMode = INFO_NONE;
static int   g_infoW96 = 300;
static bool  g_infoDrag = false;
// 실시간 미리 보기 (미리 보기 창 안)
enum { PV_NONE = 0, PV_HANDLER, PV_TEXT };
static int   g_pvKind = PV_NONE;
static HWND  g_pvHost = nullptr;     // 처리기 창/텍스트 상자가 들어가는 자식 창 (정보 창 자리)
static HWND  g_pvText = nullptr;     // 처리기가 없는 텍스트 파일용 읽기 전용 상자
// 리본 (SPEC §9). 프레임워크는 최상위 창에만 붙으므로 맨 위에 온다.
static IUIFramework* g_fw = nullptr;
static int   g_ribbonH = 0;
static IContextMenu3* g_cm3 = nullptr;   // 셸 메뉴가 떠 있는 동안 메뉴 메시지 전달 (보내기, 새로 만들기 …)
static HWND  g_tabEdit = nullptr;        // 탭 이름 바꾸기
static Tab*  g_tabEditing = nullptr;
static HWND  g_tip = nullptr;            // 탭 툴팁 (포커스 Pane 전체 경로)
// 설정 (SPEC §11): %APPDATA%\ExtendedExplorer\settings.ini (+ ribbon.dat)
static std::wstring g_dir, g_ini;
static bool  g_restore = true;           // 종료 시 탭·분할·경로 저장, 다음 실행 때 복원
static bool  g_openInTab = true;         // 폴더 열기 = 실행 중인 창의 새 탭 (아니면 새 창)
static void IniSetInt(const wchar_t* sec, const wchar_t* key, int v);

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static Tab* Cur() { return (g_cur >= 0 && g_cur < (int)g_tabs.size()) ? g_tabs[g_cur] : nullptr; }
static Node* FocusNode() { Tab* t = Cur(); return t ? t->focus : nullptr; }

static void UpdateTabInfo(Tab* t);
static void UpdateStatus();
static void RibbonRefresh();
static const UINT_PTR TIMER_INFO = 1;
static void ScheduleInfo() { if (g_main && g_infoMode) SetTimer(g_main, TIMER_INFO, 120, nullptr); }
static void ComputeInfo();
static void PaintInfo(HDC dc, RECT r);
static void ToggleInfo(int mode);
static void PlacePreview();
static void StopPreview();
static void ThemeWindowTree(HWND root);
static int g_themePref = 0;   // settings.ini Theme: 0 시스템(기본), 1 라이트, 2 다크
static void HookView(Node* n, IShellView* sv);
static bool OpenSelectionInTabs(Node* n);
static bool OpenFolderInPlace(Node* n);
static void OpenInNewTab(PCIDLIST_ABSOLUTE p);

// ---- 탐색 이벤트 -----------------------------------------------------------
static void OnPaneNavigated(Node* n, PCIDLIST_ABSOLUTE p);

// IExplorerBrowserEvents + 셸 뷰 이벤트 싱크(DShellFolderViewEvents)
// + 브라우저 사이트(IServiceProvider → ICommDlgBrowser): 기본 동작(더블클릭/Enter)을 가로챈다
struct Events : IExplorerBrowserEvents, IDispatch, IServiceProvider, ICommDlgBrowser {
    LONG  ref = 1;
    Node* pane;
    explicit Events(Node* n) : pane(n) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_IExplorerBrowserEvents)
            *ppv = static_cast<IExplorerBrowserEvents*>(this);
        else if (riid == IID_IDispatch || riid == DIID_DShellFolderViewEvents)
            *ppv = static_cast<IDispatch*>(this);
        else if (riid == IID_IServiceProvider)
            *ppv = static_cast<IServiceProvider*>(this);
        else if (riid == IID_ICommDlgBrowser)
            *ppv = static_cast<ICommDlgBrowser*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref); }
    IFACEMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    IFACEMETHODIMP OnNavigationPending(PCIDLIST_ABSOLUTE) { return S_OK; }
    IFACEMETHODIMP OnViewCreated(IShellView* sv) { if (pane) HookView(pane, sv); return S_OK; }
    IFACEMETHODIMP OnNavigationComplete(PCIDLIST_ABSOLUTE p) {
        if (pane) OnPaneNavigated(pane, p);
        return S_OK;
    }
    IFACEMETHODIMP OnNavigationFailed(PCIDLIST_ABSOLUTE) {
        if (pane) { pane->pend = -1; pane->searching = false; }
        return S_OK;
    }
    // IDispatch
    IFACEMETHODIMP GetTypeInfoCount(UINT* n) { *n = 0; return S_OK; }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) { return E_NOTIMPL; }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) { return E_NOTIMPL; }
    IFACEMETHODIMP Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) {
        if (pane && pane == FocusNode() &&
            (id == DISPID_SELECTIONCHANGED || id == DISPID_CONTENTSCHANGED || id == DISPID_FILELISTENUMDONE ||
             id == DISPID_VIEWMODECHANGED))
            UpdateStatus();
        return S_OK;
    }
    // IServiceProvider: ExplorerBrowser가 사이트에서 ICommDlgBrowser를 찾는다
    IFACEMETHODIMP QueryService(REFGUID svc, REFIID riid, void** ppv) {
        if (svc == SID_SExplorerBrowserFrame && riid == IID_ICommDlgBrowser) return QueryInterface(riid, ppv);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    // ICommDlgBrowser. SPEC §5: Ctrl+더블클릭(또는 Ctrl+Enter) = 폴더를 새 탭에서
    IFACEMETHODIMP OnDefaultCommand(IShellView*) {
        if (!pane) return S_FALSE;
        if (GetKeyState(VK_CONTROL) < 0) return OpenSelectionInTabs(pane) ? S_OK : S_FALSE;
        // SPEC §10: 폴더 "열기"를 우리 exe로 등록해도 Pane 안에서는 제자리 이동
        return OpenFolderInPlace(pane) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP OnStateChange(IShellView*, ULONG) { return S_OK; }
    IFACEMETHODIMP IncludeObject(IShellView*, PCUITEMID_CHILD) { return S_OK; }
};

static LRESULT CALLBACK ViewSubProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

static PCIDLIST_ABSOLUTE CurPidl(Node* n) { return (n && n->hi >= 0) ? n->hist[n->hi] : nullptr; }

// 히스토리: 뒤로/앞으로로 도착했으면 인덱스만 옮기고, 아니면 앞쪽을 잘라내고 쌓는다.
// 같은 위치로의 이동(새로 고침 등)은 쌓지 않는다.
static void OnPaneNavigated(Node* n, PCIDLIST_ABSOLUTE p) {
    if (n->pend >= 0 && n->pend < (int)n->hist.size() && ILIsEqual(n->hist[n->pend], p)) {
        n->hi = n->pend;
    } else if (n->hi < 0 || !ILIsEqual(n->hist[n->hi], p)) {
        for (size_t i = (size_t)(n->hi + 1); i < n->hist.size(); ++i) ILFree(n->hist[i]);
        n->hist.resize((size_t)(n->hi + 1));
        n->hist.push_back(ILCloneFull(p));
        n->hi = (int)n->hist.size() - 1;
    }
    n->pend = -1;
    if (n->searching) n->searching = false;
    else n->query.clear();              // 검색 결과를 벗어나면 검색어도 지운다 (Win10)
    if (n->tab && n->tab->focus == n) UpdateTabInfo(n->tab);
}

static void HookView(Node* n, IShellView* sv) {
    if (n->cp) { n->cp->Unadvise(n->cpCookie); n->cp->Release(); n->cp = nullptr; }
    if (!sv) return;
    // 컨텍스트 메뉴를 우리가 띄운다 (새 탭에서 열기, 분할 명령 추가). 창이 파괴되면 subclass도 풀린다.
    HWND vw = nullptr;
    if (SUCCEEDED(sv->GetWindow(&vw)) && vw) {
        SetWindowSubclass(vw, ViewSubProc, 1, (DWORD_PTR)n);
        ThemeWindowTree(vw);   // 새 뷰는 이동할 때마다 만들어진다
    }
    IDispatch* d = nullptr;
    if (FAILED(sv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&d)))) return;
    IConnectionPointContainer* cpc = nullptr;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&cpc)))) {
        if (SUCCEEDED(cpc->FindConnectionPoint(DIID_DShellFolderViewEvents, &n->cp)) &&
            FAILED(n->cp->Advise(static_cast<IDispatch*>(n->ev), &n->cpCookie))) {
            n->cp->Release();
            n->cp = nullptr;
        }
        cpc->Release();
    }
    d->Release();
}

// ---- 트리 조작 (순수) ------------------------------------------------------
static Node* FirstLeaf(Node* n) { while (!n->isPane) n = n->a; return n; }

// SPEC §4: 선순위 순회 = 좌→우, 위→아래
static void CollectLeaves(Node* n, std::vector<Node*>& out) {
    if (n->isPane) { out.push_back(n); return; }
    CollectLeaves(n->a, out);
    CollectLeaves(n->b, out);
}

static int LeafCount(Node* n) { return n->isPane ? 1 : LeafCount(n->a) + LeafCount(n->b); }

// p를 Split{a=p, b=np}로 치환
static void InsertSplit(Tab* t, Node* p, Node* np, int dir) {
    Node* s = new Node;
    s->isPane = false;
    s->dir = dir;
    s->parent = p->parent;
    s->a = p; s->b = np;
    if (!p->parent) t->root = s;
    else if (p->parent->a == p) p->parent->a = s;
    else p->parent->b = s;
    p->parent = s; np->parent = s;
}

// p를 떼어내고 부모 Split을 형제로 치환한다. p는 해제하지 않는다. 형제를 돌려준다.
static Node* RemovePane(Tab* t, Node* p) {
    Node* par = p->parent;
    Node* sib = (par->a == p) ? par->b : par->a;
    Node* gp  = par->parent;
    if (!gp) { t->root = sib; sib->parent = nullptr; }
    else {
        if (gp->a == par) gp->a = sib; else gp->b = sib;
        sib->parent = gp;
    }
    delete par;
    p->parent = nullptr;
    return sib;
}

// SPEC §6: Ctrl+Alt+방향키. (dx,dy) 방향으로 붙어 있는 Pane 중 가장 가까운 것.
static Node* Neighbor(Node* root, Node* f, int dx, int dy) {
    std::vector<Node*> v;
    CollectLeaves(root, v);
    RECT a = f->rc;
    int cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
    Node* best = nullptr;
    long long bestScore = 0;
    for (Node* c : v) {
        if (c == f) continue;
        RECT b = c->rc;
        bool ovY = min(a.bottom, b.bottom) > max(a.top, b.top);
        bool ovX = min(a.right, b.right) > max(a.left, b.left);
        long long d, side;
        if (dx < 0)      { if (b.right > a.left || !ovY) continue;  d = a.left - b.right; }
        else if (dx > 0) { if (b.left < a.right || !ovY) continue;  d = b.left - a.right; }
        else if (dy < 0) { if (b.bottom > a.top || !ovX) continue;  d = a.top - b.bottom; }
        else             { if (b.top < a.bottom || !ovX) continue;  d = b.top - a.bottom; }
        // 같은 거리면 내 중심선에 걸친 쪽
        if (dx) side = cy < b.top ? b.top - cy : (cy > b.bottom ? cy - b.bottom : 0);
        else    side = cx < b.left ? b.left - cx : (cx > b.right ? cx - b.right : 0);
        long long score = d * 1000000 + side;
        if (!best || score < bestScore) { best = c; bestScore = score; }
    }
    return best;
}

// ---- 창 영역 ---------------------------------------------------------------
static int StripH()  { return S(40); }
static int NavH()    { return S(36); }
static int StatusH() { return S(24); }
static int SideW()   { return S(g_sideW96); }
static int SideGap() { return S(4); }

static RECT Client() { RECT rc = {}; if (g_main) GetClientRect(g_main, &rc); return rc; }

// 세로 배치: 리본 → 탭 스트립 → 네비게이션 바 → 본문 → 상태 표시줄
static int StripTop() { return g_ribbonH; }
static int NavTop()   { return StripTop() + StripH(); }

static RECT BodyRc() {
    RECT rc = Client();
    rc.top = min(rc.bottom, (LONG)(NavTop() + NavH()));
    rc.bottom = max(rc.top, rc.bottom - StatusH());
    return rc;
}
// 사이드바를 Win10 탐색 창과 같은 위치로: 루트 아이콘이 왼쪽에서 35px, 한 단계 들여쓰기 7px (100% 기준 실측)
static int SidePad()    { return S(8); }
static int TreeIndent() { return S(7); }
static RECT SideRc() {
    RECT r = BodyRc();
    r.right = g_sideOn ? min(r.right, (LONG)SideW()) : 0;
    return r;
}
static RECT SideSplitRc() {
    RECT r = BodyRc();
    r.left = SideRc().right;
    r.right = g_sideOn ? r.left + SideGap() : r.left;
    return r;
}
// 오른쪽: [분할 영역][경계][정보 창]. 정보 창이 본문 절반을 넘지 않게.
static RECT InfoRc() {
    RECT r = BodyRc();
    int w = g_infoMode ? min(S(g_infoW96), (int)(r.right - SideSplitRc().right) / 2) : 0;
    r.left = r.right - max(0, w);
    return r;
}
static RECT InfoSplitRc() {
    RECT r = InfoRc();
    r.right = r.left;
    if (g_infoMode) r.left -= SideGap();
    return r;
}
static RECT ContentRect() {
    RECT r = BodyRc();
    r.left = min(r.right, SideSplitRc().right);
    r.right = max(r.left, InfoSplitRc().left);
    return r;
}

// 네비게이션 바 (Win10 배치)
static RECT NavBtnRc(int k) {   // 0 뒤로, 1 앞으로, 2 최근 위치, 3 위로
    static const int x0[] = { 6, 34, 62, 82 }, x1[] = { 34, 62, 78, 110 };
    RECT r = { S(x0[k]), NavTop() + S(5), S(x1[k]), NavTop() + NavH() - S(5) };
    return r;
}
static RECT SearchRc() {
    RECT c = Client();
    int w = min(S(240), max(S(120), (int)c.right / 4));
    RECT r = { c.right - S(8) - w, NavTop() + S(5), c.right - S(8), NavTop() + NavH() - S(5) };
    return r;
}
static RECT AddrRc() {
    RECT r = { S(116), NavTop() + S(5), max(S(116), (int)SearchRc().left - S(8)), NavTop() + NavH() - S(5) };
    return r;
}
static RECT RefreshRc() {
    RECT r = AddrRc();
    r.left = max(r.left, r.right - S(28));
    return r;
}
static RECT StatusRc() {
    RECT r = Client();
    r.top = max(r.top, r.bottom - StatusH());
    return r;
}

// ---- 배치 ------------------------------------------------------------------
static void Layout(Node* n, RECT r, HDWP& d) {
    n->rc = r;
    if (n->isPane) {
        if (n->host) {
            int w = r.right - r.left, h = r.bottom - r.top;
            // NOCOPYBITS: 옛 화면을 새 위치로 복사하면 셸 뷰가 다시 그릴 때까지 잔상이 남는다
            UINT f = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
            HDWP nd = d ? DeferWindowPos(d, n->host, nullptr, r.left, r.top, w, h, f) : nullptr;
            if (nd) d = nd;
            else SetWindowPos(n->host, nullptr, r.left, r.top, w, h, f);
        }
        return;
    }
    RECT ra = r, rb = r;
    if (n->dir == DIR_V) {
        int avail = max(0, (int)(r.right - r.left) - GAP);
        int x = r.left + (int)(avail * n->ratio + 0.5f);
        SetRect(&n->sp, x, r.top, x + GAP, r.bottom);
        ra.right = x; rb.left = min(x + GAP, (int)r.right);
    } else {
        int avail = max(0, (int)(r.bottom - r.top) - GAP);
        int y = r.top + (int)(avail * n->ratio + 0.5f);
        SetRect(&n->sp, r.left, y, r.right, y + GAP);
        ra.bottom = y; rb.top = min(y + GAP, (int)r.bottom);
    }
    Layout(n->a, ra, d);
    Layout(n->b, rb, d);
}

static void PlaceEdit(HWND e, RECT box, int padL, int padR) {
    int h = g_fontH + 2;
    int y = (box.top + box.bottom - h) / 2;
    SetWindowPos(e, nullptr, box.left + padL, y, max(0, (int)(box.right - box.left) - padL - padR), h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void Relayout() {
    if (!g_main) return;
    if (g_searchEdit) PlaceEdit(g_searchEdit, SearchRc(), S(6), S(28));
    if (g_addrEdit)   PlaceEdit(g_addrEdit, AddrRc(), S(6), S(30));
    if (g_treeWnd) {
        RECT s = SideRc();
        s.left = min(s.right, s.left + SidePad());   // 왼쪽 여백 (WM_PAINT가 트리 배경색으로 칠함)
        SetWindowPos(g_treeWnd, nullptr, s.left, s.top, s.right - s.left, s.bottom - s.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | (g_sideOn ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    PlacePreview();
    Tab* t = Cur();
    if (t) {
        HDWP d = BeginDeferWindowPos(LeafCount(t->root));
        Layout(t->root, ContentRect(), d);
        if (d) EndDeferWindowPos(d);
    }
    InvalidateRect(g_main, nullptr, FALSE);
}

// 드래그 중에는 다음 마우스 이동 전에 모두 다시 그려야 옛 화면이 남지 않는다
static void DragRelayout() {
    Relayout();
    RECT body = BodyRc();
    RedrawWindow(g_main, &body, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static Node* HitSplitter(Node* n, POINT pt) {
    if (!n || n->isPane || !PtInRect(&n->rc, pt)) return nullptr;
    RECT h = n->sp;
    InflateRect(&h, n->dir == DIR_V ? HITPAD : 0, n->dir == DIR_H ? HITPAD : 0);
    if (PtInRect(&h, pt)) return n;
    Node* r = HitSplitter(n->a, pt);
    return r ? r : HitSplitter(n->b, pt);
}

static bool HitSideSplit(POINT pt) {
    if (!g_sideOn) return false;
    RECT r = SideSplitRc();
    InflateRect(&r, HITPAD, 0);
    return PtInRect(&r, pt) != FALSE;
}

static bool HitInfoSplit(POINT pt) {
    if (!g_infoMode) return false;
    RECT r = InfoSplitRc();
    InflateRect(&r, HITPAD, 0);
    return PtInRect(&r, pt) != FALSE;
}

// ---- Pane 생성/해제 ---------------------------------------------------------
static const wchar_t* const kPaneClass = L"EEPane";

static Node* NewPane(Tab* t, PCIDLIST_ABSOLUTE pidl) {
    Node* n = new Node;
    n->tab = t;
    if (!g_main) return n;   // selftest: 창 없이 트리만
    n->host = CreateWindowExW(0, kPaneClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, g_main, nullptr, g_inst, n);
    if (FAILED(CoCreateInstance(CLSID_ExplorerBrowser, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&n->eb))))
        return n;
    FOLDERSETTINGS fs = { FVM_DETAILS, 0 };
    RECT rc = {};
    if (FAILED(n->eb->Initialize(n->host, &rc, &fs))) { n->eb->Release(); n->eb = nullptr; return n; }
    n->eb->SetOptions(EBO_NOBORDER);
    n->ev = new Events(n);
    n->eb->Advise(n->ev, &n->cookie);
    IUnknown_SetSite(n->eb, static_cast<IServiceProvider*>(n->ev));   // 뷰가 만들어지기 전에
    if (pidl) n->eb->BrowseToIDList(pidl, SBSP_ABSOLUTE);
    return n;
}

static void FreePane(Node* n) {
    HookView(n, nullptr);
    if (n->ev) { n->ev->pane = nullptr; if (n->eb) n->eb->Unadvise(n->cookie); n->ev->Release(); }
    if (n->eb) { IUnknown_SetSite(n->eb, nullptr); n->eb->Destroy(); n->eb->Release(); }
    if (n->host) DestroyWindow(n->host);
    for (PIDLIST_ABSOLUTE p : n->hist) ILFree(p);
    ILFree(n->scope);
    delete n;
}

static void FreeSubtree(Node* n) {
    if (!n) return;
    if (n->isPane) { FreePane(n); return; }
    FreeSubtree(n->a);
    FreeSubtree(n->b);
    delete n;
}

static void ShowTree(Node* n, bool show) {
    if (n->isPane) { if (n->host) ShowWindow(n->host, show ? SW_SHOWNA : SW_HIDE); return; }
    ShowTree(n->a, show);
    ShowTree(n->b, show);
}

static IShellView* PaneView(Node* n) {
    IShellView* sv = nullptr;
    if (n && n->eb) n->eb->GetCurrentView(IID_PPV_ARGS(&sv));
    return sv;
}

// 분할돼 있을 때만 1px 강조 테두리 (SPEC §4). 하나뿐이면 식별할 필요가 없다.
static int PaneInset(Node* n) { return (n->tab && !n->tab->root->isPane) ? 1 : 0; }

static LRESULT CALLBACK PaneProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    Node* n = (Node*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_SIZE:
        if (n && n->eb) {
            RECT rc; GetClientRect(h, &rc);
            int in = PaneInset(n);
            InflateRect(&rc, -in, -in);
            n->eb->SetRect(nullptr, rc);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        bool hot = n && n->tab && n->tab->focus == n && PaneInset(n);
        SetDCBrushColor(dc, hot ? GetSysColor(COLOR_HIGHLIGHT) : C_GAP);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---- 브레드크럼 (SPEC §8) ---------------------------------------------------
struct Crumb {
    PIDLIST_ABSOLUTE pidl = nullptr;
    std::wstring name;
    int  w = 0;                 // 차지하는 폭 (텍스트+화살표)
    RECT rText = {}, rArrow = {};
};
static std::vector<Crumb> g_crumbs;   // [0] = 바탕 화면 (화살표만)
static int g_crumbFirst = 1;          // 보이는 첫 조각. 앞쪽은 « 드롭다운으로 접힌다.

// w[0]은 항상 보인다. 넘치면 앞쪽(1..)부터 숨긴다. 마지막 조각은 남긴다.
static int FitCrumbs(const std::vector<int>& w, int avail) {
    int n = (int)w.size(), total = 0;
    for (int i = 1; i < n; ++i) total += w[i];
    avail -= n ? w[0] : 0;
    int first = 1;
    while (first < n - 1 && total > avail) total -= w[first++];
    return first;
}

static void FreeCrumbs() {
    for (Crumb& c : g_crumbs) ILFree(c.pidl);
    g_crumbs.clear();
}

static void BuildCrumbs(PCIDLIST_ABSOLUTE p) {
    FreeCrumbs();
    if (!p) return;
    PIDLIST_ABSOLUTE cur = ILCloneFull(p);
    for (;;) {
        Crumb c;
        c.pidl = ILCloneFull(cur);
        g_crumbs.push_back(c);
        if (ILIsEmpty(cur)) break;
        ILRemoveLastID(cur);
    }
    ILFree(cur);
    std::reverse(g_crumbs.begin(), g_crumbs.end());
    HDC dc = GetDC(nullptr);
    HGDIOBJ of = g_font ? SelectObject(dc, g_font) : nullptr;
    for (size_t i = 0; i < g_crumbs.size(); ++i) {
        Crumb& c = g_crumbs[i];
        if (i == 0) { c.w = S(18); continue; }
        PWSTR s = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(c.pidl, SIGDN_NORMALDISPLAY, &s))) { c.name = s; CoTaskMemFree(s); }
        SIZE sz = {};
        GetTextExtentPoint32W(dc, c.name.c_str(), (int)c.name.size(), &sz);
        c.w = sz.cx + S(12) + S(16);
    }
    if (of) SelectObject(dc, of);
    ReleaseDC(nullptr, dc);
}

static void LayoutCrumbs() {
    RECT box = AddrRc();
    int x = box.left + S(26);                       // 아이콘 자리
    int maxX = RefreshRc().left - S(24);            // 빈 곳을 눌러 편집할 여지
    std::vector<int> w;
    for (Crumb& c : g_crumbs) w.push_back(c.w);
    g_crumbFirst = FitCrumbs(w, maxX - x);
    for (size_t i = 0; i < g_crumbs.size(); ++i) {
        Crumb& c = g_crumbs[i];
        c.rText = c.rArrow = RECT{};
        if (i != 0 && (int)i < g_crumbFirst) continue;
        int right = min(x + c.w, maxX);
        int arrowL = right - (i == 0 ? S(18) : S(16));
        c.rText  = RECT{ x, box.top + S(1), arrowL, box.bottom - S(1) };
        c.rArrow = RECT{ arrowL, box.top + S(1), right, box.bottom - S(1) };
        x = right;
    }
}

// ---- 포커스 ----------------------------------------------------------------
static void InvalidateRow(int top, int h) {
    if (!g_main) return;
    RECT rc = Client();
    rc.top = top; rc.bottom = top + h;
    InvalidateRect(g_main, &rc, FALSE);
}
static void InvalidateStrip() { InvalidateRow(StripTop(), StripH()); }
static void InvalidateNav()   { InvalidateRow(NavTop(), NavH()); }

// SPEC §3: 상태 표시줄은 포커스 Pane 기준
static void UpdateStatus() {
    g_status1.clear(); g_status2.clear();
    IShellView* sv = PaneView(FocusNode());
    IFolderView2* fv = nullptr;
    if (sv && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
        int all = 0, sel = 0;
        fv->ItemCount(SVGIO_ALLVIEW, &all);
        fv->ItemCount(SVGIO_SELECTION, &sel);
        wchar_t buf[128];
        swprintf_s(buf, L"%d개 항목", all);
        g_status1 = buf;
        if (sel > 0) {
            swprintf_s(buf, L"%d개 항목 선택함", sel);
            g_status2 = buf;
            // 파일만 선택했을 때 크기 합계 (Win10). ponytail: 5000개 넘으면 생략, 느린 속성 조회 방지.
            IShellItemArray* arr = nullptr;
            if (sel <= 5000 && SUCCEEDED(fv->GetSelection(FALSE, &arr))) {
                ULONGLONG total = 0;
                bool files = true;
                DWORD n = 0;
                arr->GetCount(&n);
                for (DWORD i = 0; i < n && files; ++i) {
                    IShellItem* si = nullptr;
                    if (FAILED(arr->GetItemAt(i, &si))) continue;
                    SFGAOF at = 0;
                    si->GetAttributes(SFGAO_FOLDER, &at);
                    if (at & SFGAO_FOLDER) files = false;
                    IShellItem2* si2 = nullptr;
                    if (files && SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&si2)))) {
                        ULONGLONG sz = 0;
                        if (SUCCEEDED(si2->GetUInt64(PKEY_Size, &sz))) total += sz;
                        si2->Release();
                    }
                    si->Release();
                }
                arr->Release();
                if (files) {
                    wchar_t sb[64];
                    StrFormatByteSizeW((LONGLONG)total, sb, 64);
                    g_status2 += L" ";
                    g_status2 += sb;
                }
            }
        }
        fv->Release();
    }
    if (sv) sv->Release();
    if (g_main) { RECT r = StatusRc(); InvalidateRect(g_main, &r, FALSE); }
    RibbonRefresh();   // 선택·위치·보기 모드가 바뀌면 리본 상태도 바뀐다
    ScheduleInfo();    // 미리 보기 / 세부 정보 창도
}

// 포커스 Pane이 바뀌거나 이동했을 때 창 전역 UI를 다시 맞춘다.
static void UpdateTabInfo(Tab* t) {
    if (!g_main || !t) return;
    PCIDLIST_ABSOLUTE p = CurPidl(t->focus);
    if (!p) return;
    PWSTR s = nullptr;
    if (!t->fixedName && SUCCEEDED(SHGetNameFromIDList(p, SIGDN_NORMALDISPLAY, &s))) { t->name = s; CoTaskMemFree(s); }
    SHFILEINFOW fi = {};
    if (SHGetFileInfoW((LPCWSTR)p, 0, &fi, sizeof(fi), SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON)) {
        if (t->icon) DestroyIcon(t->icon);
        t->icon = fi.hIcon;
    }
    InvalidateStrip();
    if (t != Cur()) return;
    SetWindowTextW(g_main, t->name.c_str());
    BuildCrumbs(p);
    LayoutCrumbs();
    InvalidateNav();
    if (g_searchEdit) {
        std::wstring cue = L"검색: " + t->name;
        SendMessageW(g_searchEdit, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
        if (GetFocus() != g_searchEdit) SetWindowTextW(g_searchEdit, t->focus->query.c_str());
    }
    UpdateStatus();
}

// 포커스 Pane 기록만 바꾼다 (키보드 포커스는 이미 거기 있음)
static void SetFocusPane(Node* n) {
    Tab* t = n->tab;
    if (t->focus == n) return;
    Node* old = t->focus;
    t->focus = n;
    if (old && old->host) InvalidateRect(old->host, nullptr, FALSE);
    if (n->host) InvalidateRect(n->host, nullptr, FALSE);
    UpdateTabInfo(t);
}

// 포커스 Pane으로 지정하고 키보드 포커스도 옮긴다
static void FocusPane(Node* n) {
    SetFocusPane(n);
    if (IShellView* sv = PaneView(n)) {
        HWND w = nullptr;
        if (SUCCEEDED(sv->GetWindow(&w)) && w) SetFocus(w);
        sv->Release();
    } else if (n->host) {
        SetFocus(n->host);
    }
}

// 마우스 클릭 등으로 키보드 포커스가 다른 Pane으로 가면 따라간다.
static void SyncFocus() {
    HWND f = GetFocus();
    if (f == g_lastFocus) return;
    g_lastFocus = f;
    for (HWND h = f; h; h = GetParent(h)) {
        if (GetClassLongPtrW(h, GCW_ATOM) == g_paneAtom) {
            Node* n = (Node*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (n && n->tab == Cur()) SetFocusPane(n);
            return;
        }
    }
}

// ---- 탐색 명령 (포커스 Pane 기준) ------------------------------------------
static void Navigate(Node* n, PCIDLIST_ABSOLUTE p) {
    if (n && n->eb && p) n->eb->BrowseToIDList(p, SBSP_ABSOLUTE);
}

static void GoHistTo(Node* n, int t) {
    if (!n || !n->eb || t < 0 || t >= (int)n->hist.size() || t == n->hi) return;
    n->pend = t;
    n->eb->BrowseToIDList(n->hist[t], SBSP_ABSOLUTE);
}
static void GoHist(Node* n, int d) { if (n) GoHistTo(n, n->hi + d); }

static bool CanUp(Node* n) { PCIDLIST_ABSOLUTE p = CurPidl(n); return p && !ILIsEmpty(p); }
static void GoUp(Node* n) { if (n && n->eb && CanUp(n)) n->eb->BrowseToIDList(nullptr, SBSP_PARENT); }

static void Refresh(Node* n) {
    if (IShellView* sv = PaneView(n)) { sv->Refresh(); sv->Release(); }
}

// ---- 사이드바 (SPEC §7: INameSpaceTreeControl) ------------------------------
static void TreeNavigate(IShellItem* si) {
    PIDLIST_ABSOLUTE p = nullptr;
    if (si && SUCCEEDED(SHGetIDListFromObject(si, &p))) {
        Node* f = FocusNode();
        if (f && !(CurPidl(f) && ILIsEqual(CurPidl(f), p))) Navigate(f, p);
        ILFree(p);
    }
}

struct TreeEvents : INameSpaceTreeControlEvents {
    LONG ref = 1;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_INameSpaceTreeControlEvents) {
            *ppv = static_cast<INameSpaceTreeControlEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&ref); }
    IFACEMETHODIMP_(ULONG) Release() { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    // 마우스 클릭은 메시지 루프의 TreeItemAt에서 처리한다 (이 알림은 실제로 오지 않았다)
    IFACEMETHODIMP OnItemClick(IShellItem*, NSTCEHITTEST, NSTCECLICKTYPE) { return S_FALSE; }
    // 키보드(방향키 등)로 선택을 옮길 때 이동. 트리가 처음 뜰 때 스스로 고르는 선택과
    // Ctrl+Shift+E의 선택은 무시해야 하므로 '방금 트리에서 키를 눌렀나'로 가린다.
    IFACEMETHODIMP OnSelectionChanged(IShellItemArray* arr) {
        if (GetTickCount() - g_treeInputTick > 1000) return S_OK;
        IShellItem* si = nullptr;
        if (arr && SUCCEEDED(arr->GetItemAt(0, &si))) { TreeNavigate(si); si->Release(); }
        return S_OK;
    }
    IFACEMETHODIMP OnPropertyItemCommit(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemStateChanging(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemStateChanged(IShellItem*, NSTCITEMSTATE, NSTCITEMSTATE) { return E_NOTIMPL; }
    IFACEMETHODIMP OnKeyboardInput(UINT, WPARAM, LPARAM) { return S_FALSE; }
    IFACEMETHODIMP OnBeforeExpand(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnAfterExpand(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeginLabelEdit(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnEndLabelEdit(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnGetToolTip(IShellItem*, LPWSTR, int) { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeItemDelete(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemAdded(IShellItem*, BOOL) { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemDeleted(IShellItem*, BOOL) { return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeContextMenu(IShellItem*, REFIID, void** ppv) { *ppv = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP OnAfterContextMenu(IShellItem*, IContextMenu*, REFIID, void** ppv) { *ppv = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP OnBeforeStateImageChange(IShellItem*) { return E_NOTIMPL; }
    IFACEMETHODIMP OnGetDefaultIconIndex(IShellItem*, int*, int*) { return E_NOTIMPL; }
};

// 폴더가 아닌 항목은 넣지 않는다. OneDrive를 설정하지 않은 PC에서는 FOLDERID_SkyDrive가
// 이름이 "::{CLSID}"이고 폴더 속성이 없는 항목으로 나와 사이드바에 빈 줄이 생긴다.
// (GetAttributes는 일부만 맞으면 S_FALSE라 성공으로 보면 안 되고 비트를 직접 본다.)
static void AddTreeRoot(IShellItem* si, NSTCROOTSTYLE st) {
    if (!si) return;
    PWSTR name = nullptr;
    SFGAOF at = 0;
    si->GetAttributes(SFGAO_FOLDER, &at);
    bool ok = (at & SFGAO_FOLDER) && SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name && *name &&
              wcsncmp(name, L"::", 2) != 0;
    CoTaskMemFree(name);
    if (ok) g_tree->AppendRoot(si, SHCONTF_FOLDERS, st, nullptr);
    si->Release();
}

static void CreateSidebar() {
    if (FAILED(CoCreateInstance(CLSID_NamespaceTreeControl, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_tree))))
        return;
    RECT rc = SideRc();
    // ROOTHASEXPANDO: 탐색기처럼 루트(즐겨찾기, 내 PC …)에도 펼침 화살표 칸이 있어 접을 수 있고 왼쪽이 띄워진다
    NSTCSTYLE st = NSTCS_HASEXPANDOS | NSTCS_ROOTHASEXPANDO | NSTCS_FADEINOUTEXPANDOS | NSTCS_SHOWSELECTIONALWAYS | NSTCS_FULLROWSELECT;
    if (FAILED(g_tree->Initialize(g_main, &rc, st))) { g_tree->Release(); g_tree = nullptr; return; }
    IOleWindow* ow = nullptr;
    if (SUCCEEDED(g_tree->QueryInterface(IID_PPV_ARGS(&ow)))) { ow->GetWindow(&g_treeWnd); ow->Release(); }
    TreeEvents* te = new TreeEvents;
    g_tree->TreeAdvise(static_cast<INameSpaceTreeControlEvents*>(te), &g_treeCookie);
    te->Release();
    // Win10 탐색 창 구성: 바로 가기, OneDrive, 내 PC, 네트워크
    IShellItem* si = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr, IID_PPV_ARGS(&si))))
        AddTreeRoot(si, NSTCRS_EXPANDED);
    si = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_SkyDrive, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&si))))
        AddTreeRoot(si, NSTCRS_VISIBLE);
    si = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&si))))
        AddTreeRoot(si, NSTCRS_EXPANDED);   // 탐색기처럼 내 PC는 펼친 채로 (문서·다운로드·드라이브가 바로 보임)
    si = nullptr;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_NetworkFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&si))))
        AddTreeRoot(si, NSTCRS_VISIBLE);
}

// SPEC §6: Ctrl+Shift+E = 탐색 창을 열린 폴더까지 펼친다.
// 바탕 화면부터 조상을 차례로 펼치고(트리에 없는 조상은 건너뜀) 마지막을 선택한다.
// 포커스는 Pane에 그대로 두므로 선택 변경이 이동을 일으키지 않는다.
static void ExpandTreeTo(PCIDLIST_ABSOLUTE p) {
    if (!g_tree || !p) return;
    if (!g_sideOn) { g_sideOn = true; Relayout(); }
    std::vector<PIDLIST_ABSOLUTE> chain;
    PIDLIST_ABSOLUTE cur = ILCloneFull(p);
    for (;;) {
        chain.push_back(ILCloneFull(cur));
        if (ILIsEmpty(cur)) break;
        ILRemoveLastID(cur);
    }
    ILFree(cur);
    for (size_t i = chain.size(); i-- > 0;) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromIDList(chain[i], IID_PPV_ARGS(&si)))) {
            if (i == 0) {
                g_tree->SetItemState(si, NSTCIS_SELECTED, NSTCIS_SELECTED);
                g_tree->EnsureItemVisible(si);
            } else {
                g_tree->SetItemState(si, NSTCIS_EXPANDED, NSTCIS_EXPANDED);
            }
            si->Release();
        }
        ILFree(chain[i]);
    }
}

static void DestroySidebar() {
    if (!g_tree) return;
    g_tree->TreeUnadvise(g_treeCookie);
    g_tree->Release();
    g_tree = nullptr;
}

// ---- 드롭다운 메뉴 ---------------------------------------------------------
// parent의 하위 폴더 목록. pre는 맨 앞에 붙는 항목(« 숨은 조각). bold는 굵게 표시할 항목.
static void ChildMenu(PCIDLIST_ABSOLUTE parent, POINT at, PCIDLIST_ABSOLUTE bold,
                      const std::vector<PCIDLIST_ABSOLUTE>& pre) {
    struct It { PIDLIST_ABSOLUTE p; std::wstring name; };
    std::vector<It> items;
    for (PCIDLIST_ABSOLUTE p : pre) {
        PWSTR s = nullptr;
        It it = { ILCloneFull(p), L"" };
        if (SUCCEEDED(SHGetNameFromIDList(p, SIGDN_NORMALDISPLAY, &s))) { it.name = s; CoTaskMemFree(s); }
        items.push_back(it);
    }
    size_t nPre = items.size();
    IShellFolder* sf = nullptr;
    if (ILIsEmpty(parent)) SHGetDesktopFolder(&sf);
    else SHBindToObject(nullptr, parent, nullptr, IID_PPV_ARGS(&sf));
    if (sf) {
        IEnumIDList* en = nullptr;
        if (sf->EnumObjects(g_main, SHCONTF_FOLDERS, &en) == S_OK && en) {
            PITEMID_CHILD c = nullptr;
            while (en->Next(1, &c, nullptr) == S_OK) {
                STRRET sr;
                wchar_t* s = nullptr;
                if (SUCCEEDED(sf->GetDisplayNameOf(c, SHGDN_NORMAL, &sr)) && SUCCEEDED(StrRetToStrW(&sr, c, &s))) {
                    items.push_back(It{ ILCombine(parent, c), s });
                    CoTaskMemFree(s);
                }
                CoTaskMemFree(c);
            }
            en->Release();
        }
        sf->Release();
    }
    std::sort(items.begin() + nPre, items.end(),
              [](const It& x, const It& y) { return StrCmpLogicalW(x.name.c_str(), y.name.c_str()) < 0; });
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size(); ++i) {
        if (i == nPre && nPre) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, i + 1, items[i].name.c_str());
        if (bold && ILIsEqual(items[i].p, bold)) SetMenuDefaultItem(m, (UINT)(i + 1), FALSE);
    }
    UINT cmd = items.empty() ? 0 : TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd) Navigate(FocusNode(), items[cmd - 1].p);
    for (It& it : items) ILFree(it.p);
}

static void CrumbMenu(int i) {
    if (i < 0 || i >= (int)g_crumbs.size()) return;
    POINT at = { g_crumbs[i].rArrow.left, AddrRc().bottom };
    ClientToScreen(g_main, &at);
    std::vector<PCIDLIST_ABSOLUTE> pre;
    if (i == 0) for (int k = g_crumbFirst - 1; k >= 1; --k) pre.push_back(g_crumbs[k].pidl);
    PCIDLIST_ABSOLUTE bold = (i + 1 < (int)g_crumbs.size()) ? g_crumbs[i + 1].pidl : nullptr;
    PIDLIST_ABSOLUTE parent = ILCloneFull(g_crumbs[i].pidl);   // 이동하면 g_crumbs가 다시 만들어진다
    g_menuArrow = i;
    InvalidateNav();
    ChildMenu(parent, at, bold, pre);
    ILFree(parent);
    g_menuArrow = -1;
    InvalidateNav();
}

// 최근 위치 (뒤로 드롭다운). 새 것이 위, 현재 위치에 체크.
static void HistMenu() {
    Node* n = FocusNode();
    if (!n || n->hist.empty()) return;
    HMENU m = CreatePopupMenu();
    int lo = max(0, n->hi - 8), hi = min((int)n->hist.size() - 1, n->hi + 8);
    for (int i = hi; i >= lo; --i) {
        PWSTR s = nullptr;
        std::wstring name;
        if (SUCCEEDED(SHGetNameFromIDList(n->hist[i], SIGDN_NORMALDISPLAY, &s))) { name = s; CoTaskMemFree(s); }
        AppendMenuW(m, MF_STRING | (i == n->hi ? MF_CHECKED : 0), i + 1, name.c_str());
    }
    RECT b = NavBtnRc(0);
    POINT at = { b.left, b.bottom };
    ClientToScreen(g_main, &at);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd) GoHistTo(n, (int)cmd - 1);
}

// ---- 주소 표시줄 편집 / 검색 ------------------------------------------------
static void BeginAddrEdit() {
    Node* n = FocusNode();
    PWSTR s = nullptr;
    std::wstring text;
    if (CurPidl(n) && SUCCEEDED(SHGetNameFromIDList(CurPidl(n), SIGDN_DESKTOPABSOLUTEEDITING, &s))) {
        text = s;
        CoTaskMemFree(s);
    }
    SetWindowTextW(g_addrEdit, text.c_str());
    g_addrEditing = true;
    ShowWindow(g_addrEdit, SW_SHOW);
    SetFocus(g_addrEdit);
    SendMessageW(g_addrEdit, EM_SETSEL, 0, -1);
    InvalidateNav();
}

static void EndAddrEdit(bool backToPane) {
    if (!g_addrEditing) return;
    g_addrEditing = false;
    ShowWindow(g_addrEdit, SW_HIDE);
    InvalidateNav();
    if (backToPane && FocusNode()) FocusPane(FocusNode());
}

static std::wstring EditText(HWND e) {
    int len = GetWindowTextLengthW(e);
    std::wstring s((size_t)len + 1, L'\0');
    GetWindowTextW(e, &s[0], len + 1);
    s.resize((size_t)len);
    return s;
}

// 경로로 이동. 안 되면 Win10처럼 실행해 본다 (예: "cmd", "notepad").
static void CommitAddr() {
    std::wstring text = EditText(g_addrEdit);
    Node* n = FocusNode();
    PIDLIST_ABSOLUTE p = nullptr;
    if (!text.empty() && SUCCEEDED(SHParseDisplayName(text.c_str(), nullptr, &p, 0, nullptr))) {
        EndAddrEdit(true);
        Navigate(n, p);
        ILFree(p);
        return;
    }
    wchar_t dir[MAX_PATH] = {};
    if (CurPidl(n)) SHGetPathFromIDListW(CurPidl(n), dir);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_main;
    sei.lpFile = text.c_str();
    sei.lpDirectory = dir[0] ? dir : nullptr;
    sei.nShow = SW_SHOWNORMAL;
    if (!text.empty() && ShellExecuteExW(&sei)) { EndAddrEdit(true); return; }
    std::wstring msg = L"'" + text + L"'을(를) 찾을 수 없습니다. 철자를 확인하고 다시 시도하세요.";
    MessageBoxW(g_main, msg.c_str(), L"ExtendedExplorer", MB_ICONERROR);
    SetFocus(g_addrEdit);
}

// SPEC §7: SearchFolder. 검색어는 탐색기 검색 상자와 같은 구문 해석기(AQS)로 조건을 만든다.
// search-ms: URL은 셸이 기본 탐색기로 넘겨 버리므로 쓰지 않는다.
static PIDLIST_ABSOLUTE BuildSearch(PCIDLIST_ABSOLUTE scope, const std::wstring& q) {
    PIDLIST_ABSOLUTE out = nullptr;
    IQueryParserManager* qpm = nullptr;
    IQueryParser* qp = nullptr;
    IQuerySolution* qs = nullptr;
    ICondition* cond = nullptr;
    ICondition* res = nullptr;
    ISearchFolderItemFactory* sf = nullptr;
    IShellItemArray* arr = nullptr;
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (scope && !q.empty() &&
        SUCCEEDED(CoCreateInstance(__uuidof(QueryParserManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&qpm))) &&
        SUCCEEDED(qpm->CreateLoadedParser(L"SystemIndex", LOCALE_USER_DEFAULT, IID_PPV_ARGS(&qp))) &&
        SUCCEEDED(qpm->InitializeOptions(FALSE, TRUE, qp)) &&
        SUCCEEDED(qp->Parse(q.c_str(), nullptr, &qs)) &&
        SUCCEEDED(qs->GetQuery(&cond, nullptr)) && cond &&
        SUCCEEDED(qs->Resolve(cond, SQRO_DONT_SPLIT_WORDS, &now, &res)) &&
        SUCCEEDED(CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sf))) &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists(1, &scope, &arr))) {
        std::wstring name = L"\"" + q + L"\" 검색 결과";
        sf->SetDisplayName(name.c_str());
        sf->SetScope(arr);
        sf->SetCondition(res);
        sf->GetIDList(&out);
    }
    if (arr) arr->Release();
    if (sf) sf->Release();
    if (res) res->Release();
    if (cond) cond->Release();
    if (qs) qs->Release();
    if (qp) qp->Release();
    if (qpm) qpm->Release();
    return out;
}

static void DoSearch() {
    Node* n = FocusNode();
    std::wstring q = EditText(g_searchEdit);
    if (!n || q.empty() || !CurPidl(n)) return;
    if (n->query.empty() || !n->scope) {      // 결과 화면이 아니면 지금 폴더가 범위
        ILFree(n->scope);
        n->scope = ILCloneFull(CurPidl(n));
    }
    PIDLIST_ABSOLUTE p = BuildSearch(n->scope, q);
    if (!p) return;
    n->query = q;
    n->searching = true;
    Navigate(n, p);
    ILFree(p);
}

static void EndTabRename(bool commit, bool refocus = true);

// id: 1 주소, 2 검색, 3 탭 이름
static LRESULT CALLBACK EditSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    bool addr = (id == 1);
    if (id == 3) {
        if (m == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) { EndTabRename(wp == VK_RETURN); return 0; }
        if (m == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;
        if (m == WM_KILLFOCUS) PostMessageW(g_main, WM_APP_ENDTAB, 0, 0);
        return DefSubclassProc(h, m, wp, lp);
    }
    switch (m) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { if (addr) CommitAddr(); else DoSearch(); return 0; }
        if (wp == VK_ESCAPE) {
            if (addr) EndAddrEdit(true);
            else {
                SetWindowTextW(h, L"");
                if (FocusNode()) { FocusNode()->query.clear(); FocusPane(FocusNode()); }
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == 27) return 0;   // 경고음 방지
        break;
    case WM_KILLFOCUS:
        if (addr) PostMessageW(g_main, WM_APP_ENDADDR, 0, 0);
        break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

static void FocusSearch() {
    SetFocus(g_searchEdit);
    SendMessageW(g_searchEdit, EM_SETSEL, 0, -1);
}

// ---- 탭 -------------------------------------------------------------------
static PIDLIST_ABSOLUTE DefaultPidl() {
    PIDLIST_ABSOLUTE p = nullptr;
    SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &p);
    return p;
}

static void SelectTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    if (Tab* old = Cur()) if (g_cur != i) ShowTree(old->root, false);
    g_cur = i;
    Tab* t = g_tabs[i];
    EndAddrEdit(false);
    Relayout();
    ShowTree(t->root, true);
    FocusPane(t->focus);
    UpdateTabInfo(t);
}

// pos에 탭을 끼운다. select=false면 백그라운드 탭 (현재 탭 유지).
static Tab* InsertTab(int pos, Tab* t, bool select) {
    pos = max(0, min(pos, (int)g_tabs.size()));
    g_tabs.insert(g_tabs.begin() + pos, t);
    if (g_cur >= pos) ++g_cur;
    // NewPane의 이동은 t->focus가 정해지기 전에 끝나서 그때는 탭 이름을 못 채운다.
    // 선택 탭은 SelectTab이 채우지만 백그라운드 탭(가운데 클릭, 세션 복원)은 여기서 채운다.
    UpdateTabInfo(t);
    if (select || g_cur < 0) SelectTab(pos);
    else InvalidateStrip();
    return t;
}

static Tab* NewTabAt(int pos, PCIDLIST_ABSOLUTE pidl, bool select) {
    Tab* t = new Tab;
    t->root = t->focus = NewPane(t, pidl);
    return InsertTab(pos, t, select);
}

static void NewTab(PCIDLIST_ABSOLUTE pidl) { NewTabAt((int)g_tabs.size(), pidl, true); }

// SPEC §5: 새 탭에서 열기 = 현재 탭 바로 오른쪽, 백그라운드 (브라우저와 같은 관례)
static void OpenInNewTab(PCIDLIST_ABSOLUTE p) { NewTabAt(g_cur + 1, p, false); }

// 선택된 폴더들. 파일·zip이 하나라도 섞이면 false. 호출자가 v를 ILFree.
static bool SelectedFolders(Node* n, std::vector<PIDLIST_ABSOLUTE>& v) {
    IShellView* sv = PaneView(n);
    IFolderView2* fv = nullptr;
    IShellItemArray* a = nullptr;
    bool allFolders = true;
    if (sv && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && SUCCEEDED(fv->GetSelection(FALSE, &a))) {
        DWORD cnt = 0;
        a->GetCount(&cnt);
        for (DWORD i = 0; i < cnt; ++i) {
            IShellItem* si = nullptr;
            if (FAILED(a->GetItemAt(i, &si))) continue;
            SFGAOF at = 0;
            si->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &at);
            PIDLIST_ABSOLUTE p = nullptr;
            if ((at & SFGAO_FOLDER) && !(at & SFGAO_STREAM) && SUCCEEDED(SHGetIDListFromObject(si, &p))) v.push_back(p);
            else allFolders = false;   // 파일, zip 등
            si->Release();
        }
        a->Release();
    }
    if (fv) fv->Release();
    if (sv) sv->Release();
    return allFolders && !v.empty();
}

// 선택이 모두 폴더면 각각 새 탭으로. 파일이 섞이면 false (기본 동작에 맡김).
static bool OpenSelectionInTabs(Node* n) {
    std::vector<PIDLIST_ABSOLUTE> v;
    bool ok = SelectedFolders(n, v);
    if (ok) for (size_t i = v.size(); i-- > 0;) OpenInNewTab(v[i]);   // 거꾸로 끼워야 선택 순서대로 놓인다
    for (PIDLIST_ABSOLUTE p : v) ILFree(p);
    return ok;
}

// 선택이 폴더(또는 폴더를 가리키는 바로 가기) 하나면 그 PIDL, 아니면 nullptr. 호출자가 ILFree.
static PIDLIST_ABSOLUTE SingleFolderTarget(Node* n) {
    PIDLIST_ABSOLUTE out = nullptr;
    IShellView* sv = PaneView(n);
    IFolderView2* fv = nullptr;
    IShellItemArray* a = nullptr;
    DWORD cnt = 0;
    IShellItem* si = nullptr;
    if (sv && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && SUCCEEDED(fv->GetSelection(FALSE, &a)) &&
        SUCCEEDED(a->GetCount(&cnt)) && cnt == 1 && SUCCEEDED(a->GetItemAt(0, &si))) {
        SFGAOF at = 0;
        si->GetAttributes(SFGAO_FOLDER | SFGAO_LINK, &at);
        if (at & SFGAO_LINK) {
            IShellLinkW* sl = nullptr;
            PIDLIST_ABSOLUTE t = nullptr;
            IShellItem* ti = nullptr;
            if (SUCCEEDED(si->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&sl))) &&
                SUCCEEDED(sl->GetIDList(&t)) && t && SUCCEEDED(SHCreateItemFromIDList(t, IID_PPV_ARGS(&ti)))) {
                SFGAOF ta = 0;
                ti->GetAttributes(SFGAO_FOLDER, &ta);
                if (ta & SFGAO_FOLDER) { out = t; t = nullptr; }
            }
            if (ti) ti->Release();
            ILFree(t);
            if (sl) sl->Release();
        } else if (at & SFGAO_FOLDER) {   // zip 포함: 탐색기도 제자리에서 연다
            SHGetIDListFromObject(si, &out);
        }
        si->Release();
    }
    if (a) a->Release();
    if (fv) fv->Release();
    if (sv) sv->Release();
    return out;
}

// 이동은 메시지로 미룬다: 셸 뷰 콜백 안에서 이동하면 그 뷰가 호출 도중에 파괴된다.
static bool OpenFolderInPlace(Node* n) {
    PIDLIST_ABSOLUTE p = SingleFolderTarget(n);
    if (!p) return false;
    if (!g_main || !PostMessageW(g_main, WM_APP_NAV, (WPARAM)n, (LPARAM)p)) { ILFree(p); return false; }
    return true;
}

// 복제: 분할 구조·비율·각 Pane 위치·포커스 위치·이름을 그대로. ponytail: 히스토리는 복사하지 않음.
static Node* CloneTree(Node* src, Tab* t, Node* srcFocus, Node** newFocus) {
    if (src->isPane) {
        Node* n = NewPane(t, CurPidl(src));
        if (src == srcFocus) *newFocus = n;
        return n;
    }
    Node* s = new Node;
    s->isPane = false;
    s->dir = src->dir;
    s->ratio = src->ratio;
    s->a = CloneTree(src->a, t, srcFocus, newFocus);
    s->b = CloneTree(src->b, t, srcFocus, newFocus);
    s->a->parent = s->b->parent = s;
    return s;
}

static void DuplicateTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab* src = g_tabs[i];
    Tab* t = new Tab;
    t->root = CloneTree(src->root, t, src->focus, &t->focus);
    if (!t->focus) t->focus = FirstLeaf(t->root);
    t->name = src->name;
    t->fixedName = src->fixedName;
    InsertTab(i + 1, t, true);
}

static void NewTabDefault() {
    PIDLIST_ABSOLUTE p = DefaultPidl();
    NewTab(p);
    ILFree(p);
}

static void CloseTab(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab* t = g_tabs[i];
    if (g_tabEditing == t) EndTabRename(false, false);
    g_tabs.erase(g_tabs.begin() + i);
    FreeSubtree(t->root);
    if (t->icon) DestroyIcon(t->icon);
    delete t;
    if (g_tabs.empty()) { g_cur = -1; DestroyWindow(g_main); return; }   // SPEC §5
    if (i < g_cur) --g_cur;
    else if (i == g_cur) { g_cur = -1; SelectTab(min(i, (int)g_tabs.size() - 1)); }
    InvalidateStrip();
}

// SPEC §5 탭 컨텍스트 메뉴: 다른 탭 모두 닫기 / 오른쪽 탭 모두 닫기. i 탭이 선택된다.
static void CloseOtherTabs(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab* keep = g_tabs[i];
    for (int k = (int)g_tabs.size() - 1; k >= 0; --k) if (g_tabs[k] != keep) CloseTab(k);
    SelectTab(0);
}

static void CloseTabsRight(int i) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    bool curGone = g_cur > i;
    for (int k = (int)g_tabs.size() - 1; k > i; --k) CloseTab(k);
    if (curGone) SelectTab(i);
}

// ---- 탭 이름 바꾸기 (SPEC §5) -------------------------------------------------
static RECT TabRc(int i);
static RECT CloseRc(int i);

static void BeginTabRename(int i) {
    if (i < 0 || i >= (int)g_tabs.size() || !g_tabEdit) return;
    g_tabEditing = g_tabs[i];
    RECT r = TabRc(i), c = CloseRc(i);
    RECT box = { r.left + S(34), r.top + S(4), c.left - S(2), r.bottom - S(4) };
    SetWindowTextW(g_tabEdit, g_tabEditing->name.c_str());
    SetWindowPos(g_tabEdit, HWND_TOP, box.left, box.top, max(0, (int)(box.right - box.left)), box.bottom - box.top,
                 SWP_SHOWWINDOW);
    SetFocus(g_tabEdit);
    SendMessageW(g_tabEdit, EM_SETSEL, 0, -1);
}

// 빈 이름으로 확정하면 자동 이름으로 돌아간다. refocus: 키로 끝냈으면 파일 목록으로 포커스.
static void EndTabRename(bool commit, bool refocus) {
    Tab* t = g_tabEditing;
    if (!t) return;
    g_tabEditing = nullptr;   // 먼저 지워야 포커스 이동으로 인한 재진입이 무시된다
    std::wstring s = EditText(g_tabEdit);
    ShowWindow(g_tabEdit, SW_HIDE);
    if (commit) {
        t->fixedName = !s.empty();
        if (t->fixedName) t->name = s;
        UpdateTabInfo(t);   // 창 제목도 여기서
    }
    InvalidateStrip();
    if (refocus && Cur()) FocusPane(Cur()->focus);
}

static void ResetTabName(Tab* t) {
    t->fixedName = false;
    UpdateTabInfo(t);
}

// 탭 우클릭 메뉴
static void TabMenu(int i, POINT screen) {
    if (i < 0 || i >= (int)g_tabs.size()) return;
    Tab* t = g_tabs[i];
    int n = (int)g_tabs.size();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"복제(&D)");
    AppendMenuW(m, MF_STRING, 2, L"이름 바꾸기(&M)");
    AppendMenuW(m, MF_STRING | (t->fixedName ? 0 : MF_GRAYED), 3, L"자동 이름으로 되돌리기(&A)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, L"닫기(&C)\tCtrl+F4");
    AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), 5, L"다른 탭 모두 닫기(&O)");
    AppendMenuW(m, MF_STRING | (i + 1 < n ? 0 : MF_GRAYED), 6, L"오른쪽 탭 모두 닫기(&R)");
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_main, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 1: DuplicateTab(i); break;
    case 2: BeginTabRename(i); break;
    case 3: ResetTabName(t); break;
    case 4: CloseTab(i); break;
    case 5: CloseOtherTabs(i); break;
    case 6: CloseTabsRight(i); break;
    }
}

// ---- 분할 명령 -------------------------------------------------------------
static void SplitFocus(int dir) {
    Tab* t = Cur();
    if (!t) return;
    Node* np = NewPane(t, CurPidl(t->focus));   // SPEC §4: 같은 경로로 시작
    InsertSplit(t, t->focus, np, dir);
    Relayout();
    ShowWindow(np->host, SW_SHOWNA);
    FocusPane(np);
}

static void CloseFocusPane() {
    Tab* t = Cur();
    if (!t) return;
    Node* p = t->focus;
    if (!p->parent) { CloseTab(g_cur); return; }   // SPEC §4: 마지막 Pane → 탭 닫기
    Node* sib = RemovePane(t, p);
    t->focus = FirstLeaf(sib);
    FreePane(p);
    Relayout();
    InvalidateRect(t->focus->host, nullptr, FALSE);
    FocusPane(t->focus);
    UpdateTabInfo(t);
}

static void CyclePane(int delta) {
    Tab* t = Cur();
    std::vector<Node*> v;
    CollectLeaves(t->root, v);
    if (v.size() < 2) return;
    int i = 0, n = (int)v.size();
    for (int k = 0; k < n; ++k) if (v[k] == t->focus) i = k;
    FocusPane(v[(i + delta + n) % n]);
}

static void NewWindow() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring arg = L"/newwindow";   // 실행 중인 창의 새 탭으로 넘기지 않는다
    PWSTR s = nullptr;
    if (CurPidl(FocusNode()) && SUCCEEDED(SHGetNameFromIDList(CurPidl(FocusNode()), SIGDN_DESKTOPABSOLUTEPARSING, &s))) {
        arg += L" \"" + std::wstring(s) + L"\"";
        CoTaskMemFree(s);
    }
    ShellExecuteW(nullptr, L"open", exe, arg.c_str(), nullptr, SW_SHOWNORMAL);
}

static bool FocusInEdit() {
    wchar_t cls[16] = {};
    GetClassNameW(GetFocus(), cls, 16);
    return lstrcmpiW(cls, L"Edit") == 0;
}

// 처리했으면 true. 창 전체에 대해 셸 뷰보다 먼저 본다.
static bool HandleKey(const MSG& m) {
    Tab* t = Cur();
    if (!t) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    UINT vk = (UINT)m.wParam;
    Node* f = t->focus;

    if (ctrl && alt) {
        int dx = vk == VK_LEFT ? -1 : vk == VK_RIGHT ? 1 : 0;
        int dy = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : 0;
        if (!dx && !dy) return false;
        if (Node* n = Neighbor(t->root, f, dx, dy)) FocusPane(n);
        return true;
    }
    if (ctrl) {
        switch (vk) {
        case 'R':    SplitFocus(shift ? DIR_H : DIR_V); return true;
        case 'W':    CloseFocusPane(); return true;   // SPEC §6: 마지막 Pane이면 탭이 닫힌다
        case VK_F4:  CloseTab(g_cur); return true;
        case 'T':    NewTabDefault(); return true;
        case VK_TAB: {
            int n = (int)g_tabs.size();
            SelectTab((g_cur + (shift ? -1 : 1) + n) % n);
            return true;
        }
        case 'N':    if (shift) return false; NewWindow(); return true;
        case 'L':    BeginAddrEdit(); return true;
        case 'E':    if (shift) { ExpandTreeTo(CurPidl(f)); return true; } FocusSearch(); return true;
        case 'F':    FocusSearch(); return true;
        }
        if (vk >= '1' && vk <= '8' && !shift) { SelectTab(vk - '1'); return true; }
        return false;
    }
    if (alt) {
        switch (vk) {
        case VK_LEFT:  GoHist(f, -1); return true;
        case VK_RIGHT: GoHist(f, +1); return true;
        case VK_UP:    GoUp(f); return true;
        case 'P':      ToggleInfo(shift ? INFO_DETAILS : INFO_PREVIEW); return true;   // Win10: Alt+P / Alt+Shift+P
        case 'D':      BeginAddrEdit(); return true;
        }
        return false;
    }
    switch (vk) {
    case VK_F4:   if (shift) return false; BeginAddrEdit(); return true;
    case VK_F3:   if (shift) return false; FocusSearch(); return true;
    case VK_F6:   CyclePane(shift ? -1 : 1); return true;
    case VK_F5:   if (shift) return false; Refresh(f); return true;
    case VK_BACK: if (shift || FocusInEdit()) return false; GoHist(f, -1); return true;
    }
    return false;
}

// 포커스 Pane의 셸 뷰 단축키 (F2, Del, Ctrl+C/X/V/Z/A …)
static bool ViewAccel(MSG* m) {
    Tab* t = Cur();
    if (!t || !t->focus->host || !IsChild(t->focus->host, m->hwnd)) return false;
    IShellView* sv = PaneView(t->focus);
    if (!sv) return false;
    bool done = sv->TranslateAccelerator(m) == S_OK;
    sv->Release();
    return done;
}

// ---- 그리기 공통 ------------------------------------------------------------
static void FillRc(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void FrameRc(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void FillRound(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, S(12), S(12));
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static void Glyph(HDC dc, RECT r, const wchar_t* g, HFONT f, COLORREF c) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, g, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void DrawX(HDC dc, RECT r, int half) {
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    MoveToEx(dc, cx - half, cy - half, nullptr); LineTo(dc, cx + half + 1, cy + half + 1);
    MoveToEx(dc, cx + half, cy - half, nullptr); LineTo(dc, cx - half - 1, cy + half + 1);
}

// 행 하나를 메모리 DC에 그린 뒤 한 번에 옮긴다 (깜빡임 방지)
static void Buffered(HDC hdc, RECT r, void (*paint)(HDC, RECT)) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ obm = SelectObject(dc, bm);
    SetViewportOrgEx(dc, -r.left, -r.top, nullptr);
    HGDIOBJ of = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    paint(dc, r);
    SelectObject(dc, of);
    SetViewportOrgEx(dc, 0, 0, nullptr);
    BitBlt(hdc, r.left, r.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
}

// ---- 탭 스트립 (Win11 외형, 직접 그림) ---------------------------------------
enum { PART_NONE = -1, PART_TAB = 0, PART_CLOSE, PART_PLUS };

static int TabW() {
    RECT rc = Client();
    int n = max(1, (int)g_tabs.size());
    int avail = rc.right - S(8) - S(8) - S(40);
    return max(S(48), min(S(240), avail / n));
}
static RECT TabRc(int i) {
    int w = TabW();
    RECT r = { S(8) + i * w, StripTop() + S(8), S(8) + (i + 1) * w, StripTop() + StripH() };
    return r;
}
static RECT CloseRc(int i) {
    RECT r = TabRc(i);
    RECT c = { r.right - S(34), r.top + S(4), r.right - S(6), r.bottom - S(4) };
    return c;
}
static RECT PlusRc() {
    int x = S(8) + (int)g_tabs.size() * TabW() + S(4);
    RECT r = { x, StripTop() + S(10), x + S(32), StripTop() + StripH() - S(2) };
    return r;
}

static int HitStrip(POINT pt, int* part) {
    *part = PART_NONE;
    if (pt.y < StripTop() || pt.y >= StripTop() + StripH()) return -1;
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        RECT r = TabRc(i);
        if (!PtInRect(&r, pt)) continue;
        RECT c = CloseRc(i);
        *part = PtInRect(&c, pt) ? PART_CLOSE : PART_TAB;
        return i;
    }
    RECT p = PlusRc();
    if (PtInRect(&p, pt)) *part = PART_PLUS;
    return -1;
}

static void PaintStrip(HDC dc, RECT all) {
    FillRc(dc, all, C_STRIP);
    HPEN pen = CreatePen(PS_SOLID, max(1, S(1)), C_TEXT);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int i = 0; i < (int)g_tabs.size(); ++i) {
        Tab* t = g_tabs[i];
        RECT r = TabRc(i);
        if (i == g_cur) {
            RECT rr = r; rr.bottom += S(12);   // 아래 모서리는 잘려서 각지게 보인다
            FillRound(dc, rr, C_ACTIVE);
        } else {
            if (i == g_hoverTab) { RECT rr = r; InflateRect(&rr, -S(2), 0); rr.bottom -= S(4); FillRound(dc, rr, C_HOVER); }
            if (i + 1 != g_cur && i != g_hoverTab && i + 1 != g_hoverTab)
                FillRc(dc, RECT{ r.right - 1, r.top + S(8), r.right, r.bottom - S(10) }, C_SEP);
        }
        int x = r.left + S(12);
        int ic = S(16);
        int iy = (r.top + r.bottom - ic) / 2 - (i == g_cur ? 0 : S(2));
        if (t->icon) DrawIconEx(dc, x, iy, t->icon, ic, ic, 0, nullptr, DI_NORMAL);
        RECT c = CloseRc(i);
        RECT tr = { x + ic + S(8), r.top, c.left, r.bottom - (i == g_cur ? 0 : S(4)) };
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, t->name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (i == g_hoverTab && g_hoverPart == PART_CLOSE) FillRound(dc, c, i == g_cur ? C_STRIP : C_ACTIVE);
        DrawX(dc, c, S(4));
    }
    RECT p = PlusRc();
    if (g_hoverPart == PART_PLUS) FillRound(dc, p, C_HOVER);
    int cx = (p.left + p.right) / 2, cy = (p.top + p.bottom) / 2, k = S(6);
    MoveToEx(dc, cx - k, cy, nullptr); LineTo(dc, cx + k + 1, cy);
    MoveToEx(dc, cx, cy - k, nullptr); LineTo(dc, cx, cy + k + 1);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// ---- 네비게이션 바 (Win10 외형) ---------------------------------------------
enum { HN_NONE = 0, HN_BACK, HN_FWD, HN_HIST, HN_UP, HN_REFRESH, HN_ADDR, HN_SEARCH,
       HN_CRUMB = 100, HN_ARROW = 200 };

static int HitNav(POINT pt) {
    if (pt.y < NavTop() || pt.y >= NavTop() + NavH()) return HN_NONE;
    for (int k = 0; k < 4; ++k) { RECT r = NavBtnRc(k); if (PtInRect(&r, pt)) return HN_BACK + k; }
    RECT r = SearchRc();
    if (PtInRect(&r, pt)) return HN_SEARCH;
    r = AddrRc();
    if (!PtInRect(&r, pt)) return HN_NONE;
    if (g_addrEditing) return HN_ADDR;
    r = RefreshRc();
    if (PtInRect(&r, pt)) return HN_REFRESH;
    for (int i = 0; i < (int)g_crumbs.size(); ++i) {
        if (i > 0 && PtInRect(&g_crumbs[i].rText, pt)) return HN_CRUMB + i;
        if (PtInRect(&g_crumbs[i].rArrow, pt)) return HN_ARROW + i;
    }
    return HN_ADDR;
}

static void PaintNav(HDC dc, RECT all) {
    FillRc(dc, all, C_ACTIVE);
    Node* f = FocusNode();
    bool en[4] = { f && f->hi > 0, f && f->hi + 1 < (int)f->hist.size(), f && f->hist.size() > 1, CanUp(f) };
    const wchar_t* g[4] = { G_BACK, G_FWD, G_DOWN, G_UP };
    for (int k = 0; k < 4; ++k) {
        RECT r = NavBtnRc(k);
        if (en[k] && g_hoverNav == HN_BACK + k) FillRc(dc, r, C_HOT);
        Glyph(dc, r, g[k], k == 2 ? g_glyphSm : g_glyph, en[k] ? C_TEXT : C_DIM);
    }
    // 주소 상자
    RECT box = AddrRc();
    FillRc(dc, box, C_BOX);
    FrameRc(dc, box, C_BORDER);
    if (!g_addrEditing) {
        Tab* t = Cur();
        int ic = S(16);
        if (t && t->icon) DrawIconEx(dc, box.left + S(5), (box.top + box.bottom - ic) / 2, t->icon, ic, ic, 0, nullptr, DI_NORMAL);
        for (int i = 0; i < (int)g_crumbs.size(); ++i) {
            Crumb& c = g_crumbs[i];
            if (c.rArrow.right <= c.rArrow.left) continue;
            bool hotT = g_hoverNav == HN_CRUMB + i, hotA = g_hoverNav == HN_ARROW + i || g_menuArrow == i;
            if (hotT || hotA) {
                RECT u = c.rText; u.right = c.rArrow.right;
                if (i == 0) u = c.rArrow;
                FillRc(dc, u, C_HOT);
                if (hotA && i > 0) FillRc(dc, c.rArrow, C_HOT2);
            }
            if (i > 0) {
                RECT tr = c.rText; tr.left += S(6);
                SetTextColor(dc, C_TEXT);
                DrawTextW(dc, c.name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (i == 0 && g_crumbFirst > 1) {
                SetTextColor(dc, C_TEXT);
                DrawTextW(dc, L"«", -1, &c.rArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                Glyph(dc, c.rArrow, g_menuArrow == i ? G_DOWN : G_RIGHT, g_glyphSm, C_TEXT);
            }
        }
        RECT rr = RefreshRc();
        if (g_hoverNav == HN_REFRESH) FillRc(dc, rr, C_HOT);
        Glyph(dc, rr, G_REFRESH, g_glyphSm, C_TEXT);
    }
    // 검색 상자 (Win10: 주소 표시줄 오른쪽에 분리)
    RECT sr = SearchRc();
    FillRc(dc, sr, C_BOX);
    FrameRc(dc, sr, C_BORDER);
    RECT gr = sr; gr.left = gr.right - S(28);
    Glyph(dc, gr, G_SEARCH, g_glyphSm, C_DIM);
}

static void PaintStatus(HDC dc, RECT r) {
    FillRc(dc, r, C_ACTIVE);
    RECT t = r; t.left += S(10);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, g_status1.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, g_status1.c_str(), (int)g_status1.size(), &sz);
    t.left += sz.cx + S(28);
    DrawTextW(dc, g_status2.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// SPEC §5: 탭 툴팁 = 포커스 Pane 전체 경로. 도구 영역을 마우스 아래 탭으로 옮기고 다시 띄운다.
static void UpdateTip() {
    if (!g_tip) return;
    TTTOOLINFOW ti = { sizeof(ti) };
    ti.hwnd = g_main;
    ti.uId = 1;
    if (g_hoverTab >= 0 && g_hoverPart == PART_TAB) ti.rect = TabRc(g_hoverTab);
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    SendMessageW(g_tip, TTM_POP, 0, 0);
}

static std::wstring TabPath(int i) {
    std::wstring s;
    PWSTR p = nullptr;
    if (i >= 0 && i < (int)g_tabs.size() && CurPidl(g_tabs[i]->focus) &&
        SUCCEEDED(SHGetNameFromIDList(CurPidl(g_tabs[i]->focus), SIGDN_DESKTOPABSOLUTEEDITING, &p))) {
        s = p;
        CoTaskMemFree(p);
    }
    return s;
}

static void SetHover(int tab, int part, int nav) {
    if (tab != g_hoverTab || part != g_hoverPart) {
        g_hoverTab = tab; g_hoverPart = part;
        InvalidateStrip();
        UpdateTip();
    }
    if (nav != g_hoverNav) { g_hoverNav = nav; InvalidateNav(); }
}

static void OnNavClick(int hit) {
    Node* f = FocusNode();
    switch (hit) {
    case HN_BACK:    GoHist(f, -1); return;
    case HN_FWD:     GoHist(f, +1); return;
    case HN_HIST:    HistMenu(); return;
    case HN_UP:      GoUp(f); return;
    case HN_REFRESH: Refresh(f); return;
    case HN_ADDR:    BeginAddrEdit(); return;
    case HN_SEARCH:  FocusSearch(); return;
    }
    if (hit >= HN_ARROW) { CrumbMenu(hit - HN_ARROW); return; }
    if (hit >= HN_CRUMB) {
        int i = hit - HN_CRUMB;
        if (i < (int)g_crumbs.size()) {
            PIDLIST_ABSOLUTE p = ILCloneFull(g_crumbs[i].pidl);
            Navigate(f, p);
            ILFree(p);
        }
    }
}

// ---- 분할선 드래그 ----------------------------------------------------------
// pt는 메인 창 클라이언트 좌표. 분할선(히트 여유 포함) 위면 드래그를 시작한다.
static bool BeginSplitDrag(POINT pt) {
    if (HitSideSplit(pt)) g_sideDrag = true;
    else if (HitInfoSplit(pt)) g_infoDrag = true;
    else if (!Cur() || !(g_drag = HitSplitter(Cur()->root, pt))) return false;
    SetCapture(g_main);
    return true;
}

// 히트 여유(HITPAD)는 Pane/사이드바 위에 걸쳐 있다. 커서는 거기서도 바뀌므로
// 그 자리 클릭도 자식에게 가기 전에 가로채 드래그로 만든다.
static bool SplitterClick(const MSG& m) {
    if (m.message != WM_LBUTTONDOWN || m.hwnd == g_main || !IsChild(g_main, m.hwnd)) return false;
    POINT pt = { GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam) };
    MapWindowPoints(m.hwnd, g_main, &pt, 1);
    return BeginSplitDrag(pt);
}

// ---- 리본 명령 (SPEC §9, 모두 포커스 Pane 기준) -----------------------------
static IFolderView2* FocusFV() {
    IFolderView2* fv = nullptr;
    if (IShellView* sv = PaneView(FocusNode())) { sv->QueryInterface(IID_PPV_ARGS(&fv)); sv->Release(); }
    return fv;
}

static IShellItemArray* SelectionArray() {
    IShellItemArray* a = nullptr;
    if (IFolderView2* fv = FocusFV()) {
        if (FAILED(fv->GetSelection(FALSE, &a))) a = nullptr;
        fv->Release();
    }
    return a;
}

static int SelCount() {
    int n = 0;
    if (IFolderView2* fv = FocusFV()) { fv->ItemCount(SVGIO_SELECTION, &n); fv->Release(); }
    return n;
}

static std::vector<std::wstring> SelPaths() {
    std::vector<std::wstring> v;
    IShellItemArray* a = SelectionArray();
    if (!a) return v;
    DWORD n = 0;
    a->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        IShellItem* si = nullptr;
        if (FAILED(a->GetItemAt(i, &si))) continue;
        PWSTR s = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &s)) ||
            SUCCEEDED(si->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &s))) {
            v.push_back(s);
            CoTaskMemFree(s);
        }
        si->Release();
    }
    a->Release();
    return v;
}

static std::wstring FolderPath() {
    wchar_t d[MAX_PATH] = {};
    if (CurPidl(FocusNode())) SHGetPathFromIDListW(CurPidl(FocusNode()), d);
    return d;
}

// 셸 컨텍스트 메뉴 동사 실행 (탐색기와 같은 경로: 타사 확장·되돌리기 포함)
static HRESULT InvokeMenu(IContextMenu* cm, const char* verb, DWORD mask) {
    HMENU m = CreatePopupMenu();
    HRESULT hr = cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL);
    if (SUCCEEDED(hr)) {
        wchar_t wv[64];
        MultiByteToWideChar(CP_ACP, 0, verb, -1, wv, 64);
        CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
        ici.fMask = CMIC_MASK_UNICODE | mask;
        ici.hwnd = g_main;
        ici.lpVerb = verb;
        ici.lpVerbW = wv;
        ici.nShow = SW_SHOWNORMAL;
        hr = cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici);
    }
    DestroyMenu(m);
    return hr;
}

static HRESULT ViewVerb(UINT svgio, const char* verb, DWORD mask = 0) {
    IShellView* sv = PaneView(FocusNode());
    if (!sv) return E_FAIL;
    IContextMenu* cm = nullptr;
    HRESULT hr = sv->GetItemObject(svgio, IID_PPV_ARGS(&cm));
    if (SUCCEEDED(hr)) {
        IUnknown_SetSite(cm, sv);   // 붙여넣기 위치, 새 폴더 이름 바꾸기 등에 필요
        hr = InvokeMenu(cm, verb, mask);
        IUnknown_SetSite(cm, nullptr);
        cm->Release();
    }
    sv->Release();
    return hr;
}

static HRESULT ItemVerb(PCIDLIST_ABSOLUTE p, const char* verb) {
    IShellItem* si = nullptr;
    if (!p || FAILED(SHCreateItemFromIDList(p, IID_PPV_ARGS(&si)))) return E_FAIL;
    IContextMenu* cm = nullptr;
    HRESULT hr = si->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm));
    if (SUCCEEDED(hr)) { hr = InvokeMenu(cm, verb, 0); cm->Release(); }
    si->Release();
    return hr;
}

// 선택이 있으면 선택 항목, 없으면 현재 폴더 (Win10: 속성, 즐겨찾기에 고정)
static HRESULT SelOrFolderVerb(const char* verb) {
    return SelCount() ? ViewVerb(SVGIO_SELECTION, verb) : ItemVerb(CurPidl(FocusNode()), verb);
}

static void SetClipboardText(const std::wstring& s) {
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// Win10: 따옴표로 감싼 경로, 여러 개면 줄 바꿈
static void CopyPath() {
    std::vector<std::wstring> v = SelPaths();
    PWSTR s = nullptr;
    if (v.empty() && CurPidl(FocusNode()) &&
        SUCCEEDED(SHGetNameFromIDList(CurPidl(FocusNode()), SIGDN_DESKTOPABSOLUTEPARSING, &s))) {
        v.push_back(s);
        CoTaskMemFree(s);
    }
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? L"\r\n\"" : L"\"") + v[i] + L"\"";
    if (!out.empty()) SetClipboardText(out);
}

// 이동 위치 / 복사 위치: 폴더를 고르고 IFileOperation (진행률·충돌 대화상자·되돌리기는 셸이 처리)
static void TransferTo(bool move) {
    IShellItemArray* a = SelectionArray();
    if (!a) return;
    IFileOpenDialog* d = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS);
        d->SetTitle(move ? L"이동할 위치 선택" : L"복사할 위치 선택");
        IShellItem* cur = nullptr;
        if (CurPidl(FocusNode()) && SUCCEEDED(SHCreateItemFromIDList(CurPidl(FocusNode()), IID_PPV_ARGS(&cur)))) {
            d->SetFolder(cur);
            cur->Release();
        }
        IShellItem* dest = nullptr;
        if (SUCCEEDED(d->Show(g_main)) && SUCCEEDED(d->GetResult(&dest))) {
            IFileOperation* op = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
                op->SetOwnerWindow(g_main);
                if (move) op->MoveItems(a, dest); else op->CopyItems(a, dest);
                op->PerformOperations();
                op->Release();
            }
            dest->Release();
        }
        d->Release();
    }
    a->Release();
}

static void RenameSel() {
    IFolderView2* fv = FocusFV();
    if (!fv) return;
    int i = -1;
    if (FAILED(fv->GetSelectedItem(-1, &i))) i = -1;
    IShellView* sv = nullptr;
    PITEMID_CHILD c = nullptr;
    if (i >= 0 && SUCCEEDED(fv->QueryInterface(IID_PPV_ARGS(&sv))) && SUCCEEDED(fv->Item(i, &c))) {
        sv->SelectItem(c, SVSI_EDIT);
        CoTaskMemFree(c);
    }
    if (sv) sv->Release();
    fv->Release();
}

// 0 모두 선택, 1 선택 안 함, 2 선택 영역 반전
// ponytail: 항목마다 SelectItem 호출. 수만 개 폴더에서 느리면 DefView 내부 명령으로 교체.
static void SelectMode(int mode) {
    IFolderView2* fv = FocusFV();
    if (!fv) return;
    if (mode == 1) {
        IShellView* sv = nullptr;
        if (SUCCEEDED(fv->QueryInterface(IID_PPV_ARGS(&sv)))) { sv->SelectItem(nullptr, SVSI_DESELECTOTHERS); sv->Release(); }
    } else {
        int n = 0;
        fv->ItemCount(SVGIO_ALLVIEW, &n);
        for (int i = 0; i < n; ++i) {
            DWORD f = SVSI_SELECT;
            if (mode == 2) {
                PITEMID_CHILD c = nullptr;
                DWORD st = 0;
                if (SUCCEEDED(fv->Item(i, &c))) { fv->GetSelectionState(c, &st); CoTaskMemFree(c); }
                f = (st & SVSI_SELECT) ? SVSI_DESELECT : SVSI_SELECT;
            }
            fv->SelectItem(i, f);
        }
    }
    fv->Release();
}

// 새 항목: 셸의 "새로 만들기" 메뉴 (ShellNew 등록 항목 전부)
static void NewItemMenu() {
    Node* n = FocusNode();
    IShellView* sv = PaneView(n);
    if (!sv || !CurPidl(n)) { if (sv) sv->Release(); return; }
    IContextMenu* cm = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cm)))) {
        IShellExtInit* ei = nullptr;
        if (SUCCEEDED(cm->QueryInterface(IID_PPV_ARGS(&ei)))) { ei->Initialize(CurPidl(n), nullptr, nullptr); ei->Release(); }
        IUnknown_SetSite(cm, sv);
        HMENU m = CreatePopupMenu();
        if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL))) {
            HMENU sub = GetSubMenu(m, 0);
            cm->QueryInterface(IID_PPV_ARGS(&g_cm3));   // 하위 항목은 WM_INITMENUPOPUP 때 채워진다
            POINT pt;
            GetCursorPos(&pt);
            UINT cmd = TrackPopupMenu(sub ? sub : m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_main, nullptr);
            if (g_cm3) { g_cm3->Release(); g_cm3 = nullptr; }
            if (cmd) {
                CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
                ici.fMask = CMIC_MASK_UNICODE;
                ici.hwnd = g_main;
                ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                ici.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                ici.nShow = SW_SHOWNORMAL;
                cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici);
            }
        }
        DestroyMenu(m);
        IUnknown_SetSite(cm, nullptr);
        cm->Release();
    }
    sv->Release();
}

// 선택 항목을 target에 끌어다 놓은 것과 같다 (보내기 메뉴와 같은 경로)
static bool DropOn(IShellItem* target) {
    IShellItemArray* a = SelectionArray();
    if (!a) return false;
    IDataObject* dobj = nullptr;
    IDropTarget* dt = nullptr;
    bool ok = false;
    if (SUCCEEDED(a->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dobj))) &&
        SUCCEEDED(target->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt)))) {
        POINTL pt = { 0, 0 };
        DWORD eff = DROPEFFECT_COPY | DROPEFFECT_LINK;
        if (SUCCEEDED(dt->DragEnter(dobj, MK_LBUTTON, pt, &eff)) && eff) {
            eff = DROPEFFECT_COPY | DROPEFFECT_LINK;
            ok = SUCCEEDED(dt->Drop(dobj, MK_LBUTTON, pt, &eff));
        } else {
            dt->DragLeave();
        }
    }
    if (dt) dt->Release();
    if (dobj) dobj->Release();
    a->Release();
    return ok;
}

// 보내기(SendTo) 폴더에서 확장자 또는 이름으로 대상을 찾아 놓는다
static bool SendTo(const wchar_t* ext, const wchar_t* name) {
    IShellItem* folder = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_SendTo, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) return false;
    IEnumShellItems* en = nullptr;
    bool done = false;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* si = nullptr;
        while (!done && en->Next(1, &si, nullptr) == S_OK) {
            PWSTR p = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                bool hit = (ext && lstrcmpiW(PathFindExtensionW(p), ext) == 0) ||
                           (name && StrStrIW(PathFindFileNameW(p), name));
                if (hit) done = DropOn(si);
                CoTaskMemFree(p);
            }
            si->Release();
        }
        en->Release();
    }
    folder->Release();
    return done;
}

static void NotAvailable(const wchar_t* what) {
    std::wstring msg = std::wstring(what) + L"을(를) 사용할 수 없습니다.";
    MessageBoxW(g_main, msg.c_str(), L"ExtendedExplorer", MB_ICONINFORMATION);
}

static void Burn() {
    ICDBurn* b = nullptr;
    BOOL has = FALSE;
    wchar_t d[4] = {};
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_CDBurn, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&b)))) {
        if (SUCCEEDED(b->HasRecordableDrive(&has)) && has && SUCCEEDED(b->GetRecorderDriveLetter(d, 4))) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(d, nullptr, IID_PPV_ARGS(&si)))) { ok = DropOn(si); si->Release(); }
        }
        b->Release();
    }
    if (!ok && !has) NotAvailable(L"디스크 굽기 장치");
}

static void AdvancedSecurity() {
    std::vector<std::wstring> v = SelPaths();
    std::wstring p = v.empty() ? FolderPath() : v[0];
    if (!p.empty()) SHObjectProperties(g_main, SHOP_FILEPATH, p.c_str(), L"보안");
}

// 보기 모드 8종
struct ViewMode { UINT id; FOLDERVIEWMODE mode; int size; };
static const ViewMode kViews[] = {
    { cmdViewXL, FVM_ICON, 256 }, { cmdViewL, FVM_ICON, 96 }, { cmdViewM, FVM_ICON, 48 },
    { cmdViewS, FVM_SMALLICON, 16 }, { cmdViewList, FVM_LIST, 16 }, { cmdViewDetails, FVM_DETAILS, 16 },
    { cmdViewTiles, FVM_TILE, 48 }, { cmdViewContent, FVM_CONTENT, 32 },
};

// 현재 보기 → 리본 항목. 아이콘 크기는 Ctrl+휠로 연속이므로 Win10처럼 구간으로 나눈다.
static UINT ViewCmd(FOLDERVIEWMODE m, int size) {
    switch (m) {
    case FVM_ICON: case FVM_THUMBNAIL:
        return size >= 192 ? cmdViewXL : size >= 80 ? cmdViewL : size >= 40 ? cmdViewM : cmdViewS;
    case FVM_SMALLICON: return cmdViewS;
    case FVM_LIST:      return cmdViewList;
    case FVM_DETAILS:   return cmdViewDetails;
    case FVM_TILE:      return cmdViewTiles;
    case FVM_CONTENT:   return cmdViewContent;
    default:            return 0;
    }
}

struct KeyCmd { UINT id; const PROPERTYKEY* key; };
static const KeyCmd kSort[]  = { { cmdSortName, &PKEY_ItemNameDisplay }, { cmdSortDate, &PKEY_DateModified },
                                 { cmdSortType, &PKEY_ItemTypeText }, { cmdSortSize, &PKEY_Size } };
static const KeyCmd kGroup[] = { { cmdGroupName, &PKEY_ItemNameDisplay }, { cmdGroupDate, &PKEY_DateModified },
                                 { cmdGroupType, &PKEY_ItemTypeText }, { cmdGroupSize, &PKEY_Size } };
static const KeyCmd kCols[]  = { { cmdColDate, &PKEY_DateModified }, { cmdColType, &PKEY_ItemTypeText },
                                 { cmdColSize, &PKEY_Size }, { cmdColCreated, &PKEY_DateCreated },
                                 { cmdColAuthor, &PKEY_Author }, { cmdColTags, &PKEY_Keywords },
                                 { cmdColTitle, &PKEY_Title } };
static const PROPERTYKEY kNoKey = { GUID_NULL, 0 };   // = PKEY_Null: 그룹화 해제

template <size_t N>
static const PROPERTYKEY* FindKey(const KeyCmd (&t)[N], UINT id) {
    for (const KeyCmd& k : t) if (k.id == id) return k.key;
    return nullptr;
}

static bool GetSort(IFolderView2* fv, SORTCOLUMN& sc) {
    int n = 0;
    return SUCCEEDED(fv->GetSortColumnCount(&n)) && n > 0 && SUCCEEDED(fv->GetSortColumns(&sc, 1));
}

static IColumnManager* FocusCM() {
    IColumnManager* cm = nullptr;
    if (IShellView* sv = PaneView(FocusNode())) { sv->QueryInterface(IID_PPV_ARGS(&cm)); sv->Release(); }
    return cm;
}

static std::vector<PROPERTYKEY> VisibleCols(IColumnManager* cm) {
    UINT n = 0;
    cm->GetColumnCount(CM_ENUM_VISIBLE, &n);
    std::vector<PROPERTYKEY> v(n);
    if (n && FAILED(cm->GetColumns(CM_ENUM_VISIBLE, v.data(), n))) v.clear();
    return v;
}

static void ToggleColumn(const PROPERTYKEY& key) {
    IColumnManager* cm = FocusCM();
    if (!cm) return;
    std::vector<PROPERTYKEY> v = VisibleCols(cm);
    auto it = std::find_if(v.begin(), v.end(), [&](const PROPERTYKEY& k) { return IsEqualPropertyKey(k, key); });
    if (it != v.end()) v.erase(it); else v.push_back(key);
    if (!v.empty()) cm->SetColumns(v.data(), (UINT)v.size());
    cm->Release();
}

static void SizeAllColumns() {
    IColumnManager* cm = FocusCM();
    if (!cm) return;
    for (const PROPERTYKEY& k : VisibleCols(cm)) {
        CM_COLUMNINFO ci = { sizeof(ci), CM_MASK_WIDTH };
        if (SUCCEEDED(cm->GetColumnInfo(k, &ci))) {
            ci.uWidth = (UINT)CM_WIDTH_AUTOSIZE;
            cm->SetColumnInfo(k, &ci);
        }
    }
    cm->Release();
}

template <class F> static void ForEachPane(F f) {
    for (Tab* t : g_tabs) {
        std::vector<Node*> v;
        CollectLeaves(t->root, v);
        for (Node* n : v) f(n);
    }
}

// 확장명·숨긴 항목·확인란은 Win10 리본처럼 시스템 셸 설정을 바꾼다 (SPEC §4: 창 전역)
static bool ShellFlag(DWORD ssf) {
    SHELLSTATEW ss = {};
    SHGetSetSettings(&ss, ssf, FALSE);
    switch (ssf) {
    case SSF_SHOWEXTENSIONS:  return ss.fShowExtensions != 0;
    case SSF_SHOWALLOBJECTS:  return ss.fShowAllObjects != 0;
    case SSF_AUTOCHECKSELECT: return ss.fAutoCheckSelect != 0;
    }
    return false;
}

static void SetShellFlag(DWORD ssf, bool on) {
    SHELLSTATEW ss = {};
    SHGetSetSettings(&ss, ssf, FALSE);
    switch (ssf) {
    case SSF_SHOWEXTENSIONS:  ss.fShowExtensions = on; break;
    case SSF_SHOWALLOBJECTS:  ss.fShowAllObjects = on; break;
    case SSF_AUTOCHECKSELECT: ss.fAutoCheckSelect = on; break;
    }
    SHGetSetSettings(&ss, ssf, TRUE);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ShellState", SMTO_ABORTIFHUNG, 200, nullptr);
    ForEachPane([&](Node* n) {
        IShellView* sv = PaneView(n);
        if (!sv) return;
        IFolderView2* fv = nullptr;
        if (ssf == SSF_AUTOCHECKSELECT && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) {
            fv->SetCurrentFolderFlags(FWF_CHECKSELECT, on ? FWF_CHECKSELECT : 0);
            fv->Release();
        }
        sv->Refresh();
        sv->Release();
    });
}

// 모두 숨김이면 숨김 해제, 아니면 숨김 (Win10 토글)
static void HideSelected() {
    std::vector<std::wstring> v = SelPaths();
    bool allHidden = !v.empty();
    for (const std::wstring& p : v) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_HIDDEN)) allHidden = false;
    }
    for (const std::wstring& p : v) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        a = allHidden ? (a & ~FILE_ATTRIBUTE_HIDDEN) : (a | FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW(p.c_str(), a ? a : FILE_ATTRIBUTE_NORMAL);
    }
    Refresh(FocusNode());
}

static void OpenShellHere(const wchar_t* exe) {
    std::wstring d = FolderPath();
    ShellExecuteW(nullptr, L"open", exe, nullptr, d.empty() ? nullptr : d.c_str(), SW_SHOWNORMAL);
}

// 드라이브 도구: 드라이브를 선택했거나 드라이브 루트에 있을 때
static wchar_t ContextDrive() {
    std::vector<std::wstring> v = SelPaths();
    if (v.size() == 1 && v[0].size() == 3 && v[0][1] == L':') return towupper(v[0][0]);
    if (!v.empty()) return 0;
    std::wstring f = FolderPath();
    return (f.size() == 3 && f[1] == L':') ? towupper(f[0]) : 0;
}

static bool IsImagePath(const std::wstring& p) {
    PERCEIVED t = PERCEIVED_TYPE_UNSPECIFIED;
    PERCEIVEDFLAG f = 0;
    return SUCCEEDED(AssocGetPerceivedType(PathFindExtensionW(p.c_str()), &t, &f, nullptr)) && t == PERCEIVED_TYPE_IMAGE;
}

static bool SelHasImage() {
    for (const std::wstring& p : SelPaths()) if (IsImagePath(p)) return true;
    return false;
}

static void DriveCmd(UINT id) {
    wchar_t d = ContextDrive();
    if (!d) return;
    wchar_t root[4] = { d, L':', L'\\', 0 };
    switch (id) {
    case cmdOptimize: ShellExecuteW(g_main, L"open", L"dfrgui.exe", nullptr, nullptr, SW_SHOWNORMAL); break;
    case cmdCleanup: {
        wchar_t arg[8] = { L'/', L'd', L' ', d, 0 };
        ShellExecuteW(g_main, L"open", L"cleanmgr.exe", arg, nullptr, SW_SHOWNORMAL);
        break;
    }
    case cmdFormat: SHFormatDrive(g_main, d - L'A', SHFMT_ID_DEFAULT, 0); break;
    case cmdEject: {
        PIDLIST_ABSOLUTE p = nullptr;
        if (SUCCEEDED(SHParseDisplayName(root, nullptr, &p, 0, nullptr))) { ItemVerb(p, "eject"); ILFree(p); }
        break;
    }
    }
}

static bool CmdEnabled(UINT id) {
    switch (id) {
    case cmdCopy: case cmdCut: case cmdMoveTo: case cmdCopyTo: case cmdDelete: case cmdDeleteMenu:
    case cmdRecycle: case cmdDeletePerm: case cmdRename: case cmdShare: case cmdEmail: case cmdZip:
    case cmdBurn: case cmdPrint: case cmdFax: case cmdHideSelected:
    case cmdRotateLeft: case cmdRotateRight: case cmdSetBackground:
        return SelCount() > 0;
    }
    return true;
}

static bool CmdChecked(UINT id) {
    switch (id) {
    case cmdNavPane:    return g_sideOn;
    case cmdPreviewPane: return g_infoMode == INFO_PREVIEW;
    case cmdDetailsPane: return g_infoMode == INFO_DETAILS;
    case cmdCheckBoxes: return ShellFlag(SSF_AUTOCHECKSELECT);
    case cmdShowExt:    return ShellFlag(SSF_SHOWEXTENSIONS);
    case cmdShowHidden: return ShellFlag(SSF_SHOWALLOBJECTS);
    case cmdRestoreSession: return g_restore;
    case cmdOpenInTab:      return g_openInTab;
    }
    if (const PROPERTYKEY* k = FindKey(kCols, id)) {
        IColumnManager* cm = FocusCM();
        if (!cm) return false;
        bool on = false;
        for (const PROPERTYKEY& v : VisibleCols(cm)) if (IsEqualPropertyKey(v, *k)) on = true;
        cm->Release();
        return on;
    }
    IFolderView2* fv = FocusFV();
    if (!fv) return false;
    bool r = false;
    FOLDERVIEWMODE m;
    int sz = 0;
    SORTCOLUMN sc;
    PROPERTYKEY gk;
    BOOL asc;
    if (id >= cmdViewXL && id <= cmdViewContent) {
        r = SUCCEEDED(fv->GetViewModeAndIconSize(&m, &sz)) && ViewCmd(m, sz) == id;
    } else if (const PROPERTYKEY* k = FindKey(kSort, id)) {
        r = GetSort(fv, sc) && IsEqualPropertyKey(sc.propkey, *k);
    } else if (id == cmdSortAsc || id == cmdSortDesc) {
        r = GetSort(fv, sc) && (sc.direction == SORT_ASCENDING) == (id == cmdSortAsc);
    } else if (const PROPERTYKEY* k = FindKey(kGroup, id)) {
        r = SUCCEEDED(fv->GetGroupBy(&gk, &asc)) && IsEqualPropertyKey(gk, *k);
    } else if (id == cmdGroupNone) {
        r = FAILED(fv->GetGroupBy(&gk, &asc)) || IsEqualPropertyKey(gk, kNoKey);
    }
    fv->Release();
    return r;
}

static void ViewCmdExec(UINT id) {
    IFolderView2* fv = FocusFV();
    if (!fv) return;
    SORTCOLUMN sc, cur;
    for (const ViewMode& v : kViews) if (v.id == id) fv->SetViewModeAndIconSize(v.mode, v.size);
    if (const PROPERTYKEY* k = FindKey(kSort, id)) {
        sc.propkey = *k;
        sc.direction = (GetSort(fv, cur) && IsEqualPropertyKey(cur.propkey, *k)) ? cur.direction : SORT_ASCENDING;
        fv->SetSortColumns(&sc, 1);
    }
    if ((id == cmdSortAsc || id == cmdSortDesc) && GetSort(fv, cur)) {
        cur.direction = id == cmdSortAsc ? SORT_ASCENDING : SORT_DESCENDING;
        fv->SetSortColumns(&cur, 1);
    }
    if (const PROPERTYKEY* k = FindKey(kGroup, id)) fv->SetGroupBy(*k, TRUE);
    if (id == cmdGroupNone) fv->SetGroupBy(kNoKey, TRUE);
    fv->Release();
}

static void RibbonExec(UINT id) {
    switch (id) {
    case cmdPin:           SelOrFolderVerb("pintohome"); break;
    case cmdCopy:          ViewVerb(SVGIO_SELECTION, "copy"); break;
    case cmdCut:           ViewVerb(SVGIO_SELECTION, "cut"); break;
    case cmdPaste:         ViewVerb(SVGIO_BACKGROUND, "paste"); break;
    case cmdPasteShortcut: ViewVerb(SVGIO_BACKGROUND, "pastelink"); break;
    case cmdCopyPath:      CopyPath(); break;
    case cmdMoveTo:        TransferTo(true); break;
    case cmdCopyTo:        TransferTo(false); break;
    case cmdDelete: case cmdDeleteMenu: case cmdRecycle:
                           ViewVerb(SVGIO_SELECTION, "delete"); break;
    case cmdDeletePerm:    ViewVerb(SVGIO_SELECTION, "delete", CMIC_MASK_SHIFT_DOWN); break;
    case cmdRename:        RenameSel(); break;
    case cmdNewFolder:     ViewVerb(SVGIO_BACKGROUND, "NewFolder"); break;
    case cmdNewItem:       NewItemMenu(); break;
    case cmdProperties:    SelOrFolderVerb("properties"); break;
    case cmdSelectAll:     SelectMode(0); break;
    case cmdSelectNone:    SelectMode(1); break;
    case cmdInvertSel:     SelectMode(2); break;
    case cmdNewTab:        NewTabDefault(); break;
    case cmdShare:         ViewVerb(SVGIO_SELECTION, "Windows.ModernShare"); break;
    case cmdEmail:         if (!SendTo(L".MAPIMail", nullptr)) NotAvailable(L"전자 메일"); break;
    case cmdZip:           if (!SendTo(L".ZFSendToTarget", nullptr)) NotAvailable(L"압축(ZIP) 폴더"); break;
    case cmdBurn:          Burn(); break;
    case cmdPrint:         ViewVerb(SVGIO_SELECTION, "print"); break;
    case cmdFax:           if (!SendTo(nullptr, L"팩스") && !SendTo(nullptr, L"Fax")) NotAvailable(L"팩스"); break;
    case cmdSecurity:      AdvancedSecurity(); break;
    case cmdNavPane:       g_sideOn = !g_sideOn; Relayout(); break;
    case cmdPreviewPane:   ToggleInfo(INFO_PREVIEW); break;
    case cmdDetailsPane:   ToggleInfo(INFO_DETAILS); break;
    case cmdSizeColumns:   SizeAllColumns(); break;
    case cmdCheckBoxes:    SetShellFlag(SSF_AUTOCHECKSELECT, !ShellFlag(SSF_AUTOCHECKSELECT)); break;
    case cmdShowExt:       SetShellFlag(SSF_SHOWEXTENSIONS, !ShellFlag(SSF_SHOWEXTENSIONS)); break;
    case cmdShowHidden:    SetShellFlag(SSF_SHOWALLOBJECTS, !ShellFlag(SSF_SHOWALLOBJECTS)); break;
    case cmdHideSelected:  HideSelected(); break;
    // 설정은 바로 파일에 쓴다 (다른 창·다음 실행이 같은 값을 본다)
    case cmdRestoreSession:
        g_restore = !g_restore;
        IniSetInt(L"General", L"RestoreSession", g_restore);
        break;
    case cmdOpenInTab:
        g_openInTab = !g_openInTab;
        IniSetInt(L"General", L"OpenFoldersInNewTab", g_openInTab);
        break;
    case cmdOptions:       ShellExecuteW(g_main, L"open", L"control.exe", L"folders", nullptr, SW_SHOWNORMAL); break;
    case cmdSplitV:        SplitFocus(DIR_V); break;
    case cmdSplitH:        SplitFocus(DIR_H); break;
    case cmdClosePane:     CloseFocusPane(); break;
    case cmdNewWindow:     NewWindow(); break;
    case cmdOpenPS:        OpenShellHere(L"powershell.exe"); break;
    case cmdOpenCmd:       OpenShellHere(L"cmd.exe"); break;
    case cmdClose:         PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case cmdOptimize: case cmdCleanup: case cmdFormat: case cmdEject:
                           DriveCmd(id); break;
    // ponytail: 회전 동사는 셸 이미지 확장(ShellImagePreview)이 있어야 동작
    case cmdRotateLeft:    ViewVerb(SVGIO_SELECTION, "rotate270"); break;
    case cmdRotateRight:   ViewVerb(SVGIO_SELECTION, "rotate90"); break;
    case cmdSetBackground: ViewVerb(SVGIO_SELECTION, "setdesktopwallpaper"); break;
    default:
        if (const PROPERTYKEY* k = FindKey(kCols, id)) ToggleColumn(*k);
        else ViewCmdExec(id);
    }
    if (g_main) RibbonRefresh();
}

// ---- Pane 컨텍스트 메뉴 (SPEC §5, §6) ----------------------------------------
// 셸 메뉴를 그대로 만들고 우리 항목만 끼운다: 항목 메뉴에 "새 탭에서 열기",
// 빈 공간 메뉴 끝에 구분선 + 분할 명령.
enum { PM_FIRST_SHELL = 1, PM_LAST_SHELL = 0x7EFF, PM_NEWTAB = 0x7F00, PM_SPLITV, PM_SPLITH, PM_CLOSEPANE };

static void ShowPaneMenu(Node* n, POINT pt, bool keyboard) {
    IShellView* sv = PaneView(n);
    if (!sv) return;
    if (n->tab == Cur()) SetFocusPane(n);
    if (keyboard) {   // ponytail: 키보드(Shift+F10)면 Pane 왼쪽 위. 항목 위치 API는 스크롤 보정이 필요.
        RECT r;
        GetWindowRect(n->host, &r);
        pt = POINT{ r.left + S(40), r.top + S(40) };
    }
    int sel = 0;
    IFolderView2* fv = nullptr;
    if (SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv)))) { fv->ItemCount(SVGIO_SELECTION, &sel); fv->Release(); }
    bool items = sel > 0;
    IContextMenu* cm = nullptr;
    if (FAILED(sv->GetItemObject(items ? SVGIO_SELECTION : SVGIO_BACKGROUND, IID_PPV_ARGS(&cm)))) { sv->Release(); return; }
    IUnknown_SetSite(cm, sv);   // "열기"가 이 Pane 안에서 이동하도록, 이름 바꾸기 등
    HMENU m = CreatePopupMenu();
    UINT flags = CMF_NORMAL | CMF_CANRENAME | (items ? CMF_ITEMMENU : 0);
    if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;
    cm->QueryContextMenu(m, 0, PM_FIRST_SHELL, PM_LAST_SHELL, flags);
    if (items) {
        std::vector<PIDLIST_ABSOLUTE> v;
        if (SelectedFolders(n, v)) {   // Win11처럼 기본 항목("열기") 바로 아래
            UINT def = GetMenuDefaultItem(m, TRUE, 0);
            UINT pos = def == (UINT)-1 ? 0 : def + 1;
            InsertMenuW(m, pos, MF_BYPOSITION | MF_STRING, PM_NEWTAB, L"새 탭에서 열기");
        }
        for (PIDLIST_ABSOLUTE p : v) ILFree(p);
    } else {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, PM_SPLITV, L"좌우 분할\tCtrl+R");
        AppendMenuW(m, MF_STRING, PM_SPLITH, L"상하 분할\tCtrl+Shift+R");
        AppendMenuW(m, MF_STRING, PM_CLOSEPANE, L"Pane 닫기\tCtrl+W");
    }
    cm->QueryInterface(IID_PPV_ARGS(&g_cm3));   // 보내기·새로 만들기 하위 메뉴
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_main, nullptr);
    if (g_cm3) { g_cm3->Release(); g_cm3 = nullptr; }
    wchar_t verb[64] = L"";
    if (cmd >= PM_FIRST_SHELL && cmd <= PM_LAST_SHELL &&
        FAILED(cm->GetCommandString(cmd - PM_FIRST_SHELL, GCS_VERBW, nullptr, (LPSTR)verb, 64))) verb[0] = 0;
    // "열기"는 등록 여부와 상관없이 이 Pane 안에서 (SPEC §10)
    bool inPlace = (!lstrcmpiW(verb, L"open") || !lstrcmpiW(verb, L"explore")) && OpenFolderInPlace(n);
    if (cmd >= PM_FIRST_SHELL && cmd <= PM_LAST_SHELL && !inPlace) {
        CMINVOKECOMMANDINFOEX ici = { sizeof(ici) };
        ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE |
                    (GetKeyState(VK_CONTROL) < 0 ? CMIC_MASK_CONTROL_DOWN : 0) |
                    (GetKeyState(VK_SHIFT) < 0 ? CMIC_MASK_SHIFT_DOWN : 0);
        ici.hwnd = g_main;
        ici.lpVerb = MAKEINTRESOURCEA(cmd - PM_FIRST_SHELL);
        ici.lpVerbW = MAKEINTRESOURCEW(cmd - PM_FIRST_SHELL);
        ici.nShow = SW_SHOWNORMAL;
        ici.ptInvoke = pt;
        cm->InvokeCommand((LPCMINVOKECOMMANDINFO)&ici);
    }
    DestroyMenu(m);
    IUnknown_SetSite(cm, nullptr);
    cm->Release();
    sv->Release();
    switch (cmd) {
    case PM_NEWTAB:    OpenSelectionInTabs(n); break;
    case PM_SPLITV:    SplitFocus(DIR_V); break;
    case PM_SPLITH:    SplitFocus(DIR_H); break;
    case PM_CLOSEPANE: CloseFocusPane(); break;
    }
}

static LRESULT CALLBACK ViewSubProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (m == WM_CONTEXTMENU) {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ShowPaneMenu((Node*)ref, pt, lp == -1);
        return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// 창 핸들이 속한 Pane (현재 탭에서만)
static Node* PaneFromHwnd(HWND w) {
    for (HWND h = w; h; h = GetParent(h)) {
        if (GetClassLongPtrW(h, GCW_ATOM) == g_paneAtom) {
            Node* n = (Node*)GetWindowLongPtrW(h, GWLP_USERDATA);
            return (n && n->tab == Cur()) ? n : nullptr;
        }
    }
    return nullptr;
}

// SPEC §5: 파일 목록에서 폴더를 가운데 클릭 = 새 탭에서 열기.
// ponytail: DefView에 hit-test API가 없어 왼쪽 클릭을 흉내 내 커서 아래 항목을 고른 뒤 선택을 본다.
static bool PaneMiddleClick(const MSG& m) {
    if (m.message != WM_MBUTTONDOWN && m.message != WM_MBUTTONUP) return false;
    Node* n = PaneFromHwnd(m.hwnd);
    if (!n) return false;
    if (m.message == WM_MBUTTONUP) {
        SetFocusPane(n);
        SendMessageW(m.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, m.lParam);
        SendMessageW(m.hwnd, WM_LBUTTONUP, 0, m.lParam);
        OpenSelectionInTabs(n);
    }
    return true;   // 누름도 삼킨다
}

// 화면 좌표 아래의 Pane (현재 탭). WindowFromPoint는 다른 앱 창이 겹쳐 있으면 엉뚱한 창을 주므로
// 각 Pane 영역으로 직접 판정한다.
static Node* PaneAtScreen(POINT pt) {
    Tab* t = Cur();
    if (!t || !g_main) return nullptr;
    ScreenToClient(g_main, &pt);
    std::vector<Node*> v;
    CollectLeaves(t->root, v);
    for (Node* n : v) if (PtInRect(&n->rc, pt)) return n;
    return nullptr;
}

// 사이드바 클릭 (SPEC §7). 트리의 클릭 알림(OnItemClick)은 오지 않고, 왼쪽 버튼 놓음은 트리의
// 끌기 감지 루프가 먼저 가져간다. 그래서 '누를 때' 그 자리 항목을 구해 두고, 트리가 클릭을 다 처리한
// 뒤 이동한다. 이미 선택된 항목을 다시 눌러도(선택 변경 없음) 이동한다. 펼침 화살표는 접기/펼치기만.
// 돌려준 항목은 호출자가 Release.
static IShellItem* TreeItemAt(const MSG& m) {
    if (!g_tree || !g_treeWnd || (m.message != WM_LBUTTONDOWN && m.message != WM_MBUTTONUP)) return nullptr;
    wchar_t cls[32] = {};
    GetClassNameW(m.hwnd, cls, 32);
    if (lstrcmpW(cls, L"SysTreeView32") || !IsChild(g_treeWnd, m.hwnd)) return nullptr;
    TVHITTESTINFO hti = {};
    hti.pt = POINT{ GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam) };
    TreeView_HitTest(m.hwnd, &hti);
    if (!hti.hItem || (hti.flags & TVHT_ONITEMBUTTON) ||
        !(hti.flags & (TVHT_ONITEMICON | TVHT_ONITEMLABEL | TVHT_ONITEMRIGHT))) return nullptr;
    POINT pt = hti.pt;
    MapWindowPoints(m.hwnd, g_treeWnd, &pt, 1);
    IShellItem* si = nullptr;
    if (FAILED(g_tree->HitTest(&pt, &si))) si = nullptr;
    return si;
}

// 마우스 엄지 버튼: XBUTTON1 = 뒤로, XBUTTON2 = 앞으로 (탐색기와 같다).
// 커서 아래 Pane을 포커스로 만들고 그 Pane에서. Pane 밖(사이드바·리본 등)이면 포커스 Pane.
// 누름·더블클릭은 삼킨다: 뷰가 따로 처리하거나 WM_APPCOMMAND로 한 번 더 오지 않게.
static bool ThumbButton(const MSG& m) {
    if (m.message != WM_XBUTTONDOWN && m.message != WM_XBUTTONUP && m.message != WM_XBUTTONDBLCLK) return false;
    if (m.hwnd != g_main && !IsChild(g_main, m.hwnd)) return false;
    if (m.message == WM_XBUTTONUP) {
        Node* n = PaneAtScreen(m.pt);
        if (n && n != FocusNode()) FocusPane(n);
        if (!n) n = FocusNode();
        GoHist(n, GET_XBUTTON_WPARAM(m.wParam) == XBUTTON1 ? -1 : +1);
    }
    return true;
}

// SPEC §6: Ctrl+마우스휠 = 보기 크기. 탐색기와 같은 순서:
// 내용 ↔ 바둑판식 ↔ 자세히 ↔ 목록 ↔ 작은 아이콘 ↔ 아이콘 48px … 256px (약 12%씩, 썸네일도 같이 커짐)
// 아이콘 구간은 48부터: 그보다 작은 FVM_ICON은 뷰가 '작은 아이콘'으로 되돌려 보고해서
// 다음 단계가 다시 처음으로 돌아가 크기가 늘지 않는다.
static const int kIconMin = 48, kIconMax = 256;
struct ViewStep { FOLDERVIEWMODE mode; int size; };
static ViewStep NextView(FOLDERVIEWMODE m, int size, int steps) {
    static const ViewStep order[] = { { FVM_CONTENT, 32 }, { FVM_TILE, 48 }, { FVM_DETAILS, 16 },
                                      { FVM_LIST, 16 }, { FVM_SMALLICON, 16 }, { FVM_ICON, kIconMin } };
    const int last = 5;
    int i = last;   // 썸네일 등 그 밖의 모드는 아이콘으로 본다
    for (int k = 0; k < last; ++k) if (order[k].mode == m) i = k;
    if (i == last) size = min(kIconMax, max(kIconMin, size));
    for (; steps > 0; --steps) {
        if (i < last) { ++i; size = order[i].size; }
        else size = min(kIconMax, size + max(4, size / 8));
    }
    for (; steps < 0; ++steps) {
        if (i == last && size > kIconMin) size = max(kIconMin, size - max(4, size / 9));
        else if (i > 0) { --i; size = order[i].size; }
    }
    return ViewStep{ order[i].mode, size };
}

static bool CtrlWheel(const MSG& m) {
    if (m.message != WM_MOUSEWHEEL || !(GET_KEYSTATE_WPARAM(m.wParam) & MK_CONTROL)) return false;
    if (m.hwnd != g_main && !IsChild(g_main, m.hwnd)) return false;
    POINT pt = { GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam) };   // 화면 좌표 = 휠을 굴린 곳
    Node* n = PaneAtScreen(pt);
    if (!n) return false;   // 사이드바 등은 각자 처리
    static int acc = 0;     // 고해상도 휠은 120보다 작게 나눠 온다
    acc += GET_WHEEL_DELTA_WPARAM(m.wParam);
    int steps = acc / WHEEL_DELTA;
    acc -= steps * WHEEL_DELTA;
    IShellView* sv = PaneView(n);
    IFolderView2* fv = nullptr;
    FOLDERVIEWMODE mode;
    int size = 0;
    if (steps && sv && SUCCEEDED(sv->QueryInterface(IID_PPV_ARGS(&fv))) && SUCCEEDED(fv->GetViewModeAndIconSize(&mode, &size))) {
        ViewStep v = NextView(mode, size, steps);
        fv->SetViewModeAndIconSize(v.mode, v.size);
    }
    if (fv) fv->Release();
    if (sv) sv->Release();
    return true;
}

// ---- 리본 아이콘 ------------------------------------------------------------
// ponytail: Win10 리본 아이콘은 ExplorerFrame.dll 내부 자원이라 쓸 수 없다. Segoe MDL2 글리프로 그린다.
// 1~3은 직접 그리는 분할 아이콘.
struct CmdGlyph { UINT id; wchar_t g; COLORREF c; };
static const COLORREF IC_N = RGB(64, 64, 64), IC_B = RGB(0, 103, 192), IC_Y = RGB(214, 160, 30), IC_R = RGB(200, 50, 50);
static const CmdGlyph kGlyphs[] = {
    { cmdPin, 0xE718, IC_B }, { cmdCopy, 0xE8C8, IC_N }, { cmdPaste, 0xE77F, IC_N }, { cmdCut, 0xE8C6, IC_N },
    { cmdCopyPath, 0xE71B, IC_N }, { cmdPasteShortcut, 0xE71B, IC_B },
    { cmdMoveTo, 0xE8DE, IC_Y }, { cmdCopyTo, 0xE8B7, IC_Y },
    { cmdDelete, 0xE74D, IC_R }, { cmdDeleteMenu, 0xE74D, IC_R }, { cmdRecycle, 0xE74D, IC_R }, { cmdDeletePerm, 0xE711, IC_R },
    { cmdRename, 0xE8AC, IC_N }, { cmdNewFolder, 0xE8F4, IC_Y }, { cmdNewItem, 0xE8A5, IC_B },
    { cmdProperties, 0xE946, IC_B }, { cmdSelectAll, 0xE8B3, IC_N }, { cmdSelectNone, 0xE8E6, IC_N },
    { cmdInvertSel, 0xE8AB, IC_N }, { cmdNewTab, 0xE78B, IC_B },
    { cmdShare, 0xE72D, IC_B }, { cmdEmail, 0xE715, IC_B }, { cmdZip, 0xE7B8, IC_Y }, { cmdBurn, 0xE958, IC_N },
    { cmdPrint, 0xE749, IC_N }, { cmdFax, 0xE749, IC_B }, { cmdSecurity, 0xE72E, IC_Y },
    { cmdNavPane, 0xE8A0, IC_B }, { cmdPreviewPane, 0xE8FF, IC_N }, { cmdDetailsPane, 0xE946, IC_N },
    { cmdViewXL, 0xE91B, IC_B }, { cmdViewL, 0xE91B, IC_B }, { cmdViewM, 0xE80A, IC_B }, { cmdViewS, 0xE80A, IC_N },
    { cmdViewList, 0xE8FD, IC_N }, { cmdViewDetails, 0xE71D, IC_N }, { cmdViewTiles, 0xE8A9, IC_N }, { cmdViewContent, 0xE8A5, IC_N },
    { cmdSortBy, 0xE8CB, IC_N }, { cmdGroupBy, 0xE71D, IC_N }, { cmdAddColumns, 0xE710, IC_N }, { cmdSizeColumns, 0xE740, IC_N },
    { cmdHideSelected, 0xED1A, IC_N }, { cmdOptions, 0xE713, IC_N },
    { cmdSplitV, 1, IC_B }, { cmdSplitH, 2, IC_B }, { cmdClosePane, 3, IC_R },
    { cmdNewWindow, 0xE78B, IC_B }, { cmdOpenPS, 0xE756, IC_B }, { cmdOpenCmd, 0xE756, IC_N }, { cmdClose, 0xE711, IC_N },
    { cmdOptimize, 0xE9F5, IC_B }, { cmdCleanup, 0xE74D, IC_N }, { cmdFormat, 0xE7F8, IC_N }, { cmdEject, 0xE7E7, IC_N },
    { cmdRotateLeft, 0xE7AD, IC_B }, { cmdRotateRight, 0xE7AD, IC_B }, { cmdSetBackground, 0xE91B, IC_N },
};

// 흰 글자를 검은 바탕에 그려 밝기를 알파로 쓴다 (premultiplied 32bpp)
static HBITMAP GlyphBitmap(wchar_t g, int px, COLORREF c) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb) return nullptr;
    memset(bits, 0, (size_t)px * px * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ ob = SelectObject(dc, hb);
    if (g < 0x100) {
        HPEN pen = CreatePen(PS_SOLID, max(1, px / 14), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        int m = px / 8;
        Rectangle(dc, m, m + px / 16, px - m, px - m - px / 16);
        if (g == 1) { MoveToEx(dc, px / 2, m, nullptr); LineTo(dc, px / 2, px - m); }
        if (g == 2) { MoveToEx(dc, m, px / 2, nullptr); LineTo(dc, px - m, px / 2); }
        if (g == 3) {
            int a = px / 3, b = px - px / 3;
            MoveToEx(dc, a, a, nullptr); LineTo(dc, b, b);
            MoveToEx(dc, b, a, nullptr); LineTo(dc, a, b);
        }
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(pen);
    } else {
        LOGFONTW lf = {};
        lstrcpyW(lf.lfFaceName, L"Segoe MDL2 Assets");
        lf.lfHeight = -(px * 13 / 16);
        lf.lfQuality = ANTIALIASED_QUALITY;   // ClearType 색 번짐 방지
        HFONT f = CreateFontIndirectW(&lf);
        HGDIOBJ of = SelectObject(dc, f);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        RECT r = { 0, 0, px, px };
        wchar_t s[2] = { g, 0 };
        DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(f);
    }
    GdiFlush();
    DWORD* p = (DWORD*)bits;
    for (int i = 0; i < px * px; ++i) {
        DWORD a = (p[i] >> 8) & 0xFF;
        p[i] = (a << 24) | ((GetRValue(c) * a / 255) << 16) | ((GetGValue(c) * a / 255) << 8) | (GetBValue(c) * a / 255);
    }
    SelectObject(dc, ob);
    DeleteDC(dc);
    return hb;
}

static IUIImageFromBitmap* g_imgFactory = nullptr;

static IUIImage* CmdImage(UINT id, bool large) {
    for (const CmdGlyph& cg : kGlyphs) {
        if (cg.id != id) continue;
        if (!g_imgFactory &&
            FAILED(CoCreateInstance(CLSID_UIRibbonImageFromBitmapFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_imgFactory))))
            return nullptr;
        COLORREF c = (g_dark && cg.c == IC_N) ? RGB(220, 220, 220) : cg.c;   // 어두운 리본에서 보이게
        HBITMAP hb = GlyphBitmap(cg.g, S(large ? 32 : 16), c);
        IUIImage* img = nullptr;
        if (hb && FAILED(g_imgFactory->CreateImage(hb, UI_OWNERSHIP_TRANSFER, &img))) { DeleteObject(hb); img = nullptr; }
        return img;
    }
    return nullptr;
}

// ---- 리본 호스트 ------------------------------------------------------------
static HRESULT RibbonProp(UINT id, REFPROPERTYKEY key, PROPVARIANT* v) {
    if (IsEqualPropertyKey(key, UI_PKEY_Enabled))     return UIInitPropertyFromBoolean(key, CmdEnabled(id), v);
    if (IsEqualPropertyKey(key, UI_PKEY_BooleanValue)) return UIInitPropertyFromBoolean(key, CmdChecked(id), v);
    if (IsEqualPropertyKey(key, UI_PKEY_ContextAvailable)) {
        bool on = id == cmdTabGroupDrive ? ContextDrive() != 0 : id == cmdTabGroupPicture ? SelHasImage() : false;
        return UIInitPropertyFromUInt32(key, on ? UI_CONTEXTAVAILABILITY_AVAILABLE : UI_CONTEXTAVAILABILITY_NOTAVAILABLE, v);
    }
    if (IsEqualPropertyKey(key, UI_PKEY_LargeImage) || IsEqualPropertyKey(key, UI_PKEY_SmallImage)) {
        IUIImage* img = CmdImage(id, IsEqualPropertyKey(key, UI_PKEY_LargeImage));
        if (!img) return E_NOTIMPL;
        HRESULT hr = UIInitPropertyFromImage(key, img, v);
        img->Release();
        return hr;
    }
    return E_NOTIMPL;
}

struct RibbonHost : IUIApplication, IUICommandHandler {
    IFACEMETHODIMP QueryInterface(REFIID r, void** p) {
        if (r == IID_IUnknown || r == __uuidof(IUIApplication)) *p = static_cast<IUIApplication*>(this);
        else if (r == __uuidof(IUICommandHandler)) *p = static_cast<IUICommandHandler*>(this);
        else { *p = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() { return 1; }    // 전역 수명
    IFACEMETHODIMP_(ULONG) Release() { return 1; }
    IFACEMETHODIMP OnViewChanged(UINT32, UI_VIEWTYPE type, IUnknown* view, UI_VIEWVERB verb, INT32) {
        if (type != UI_VIEWTYPE_RIBBON) return S_OK;
        int h = 0;
        IUIRibbon* r = nullptr;
        if (verb != UI_VIEWVERB_DESTROY && view && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&r)))) {
            IStream* st = nullptr;   // SPEC §4: 리본 접힘 상태·빠른 실행 도구 모음 복원
            if (verb == UI_VIEWVERB_CREATE && !g_dir.empty() &&
                SUCCEEDED(SHCreateStreamOnFileEx((g_dir + L"\\ribbon.dat").c_str(), STGM_READ, 0, FALSE, nullptr, &st))) {
                r->LoadSettingsFromStream(st);
                st->Release();
            }
            UINT32 v = 0;
            r->GetHeight(&v);
            h = (int)v;
            r->Release();
        }
        if (h != g_ribbonH) {   // 접기/펼치기(Ctrl+F1), 창이 작아 숨김 등
            g_ribbonH = h;
            Relayout();
            LayoutCrumbs();
        }
        return S_OK;
    }
    IFACEMETHODIMP OnCreateUICommand(UINT32, UI_COMMANDTYPE, IUICommandHandler** h) { *h = this; return S_OK; }
    IFACEMETHODIMP OnDestroyUICommand(UINT32, UI_COMMANDTYPE, IUICommandHandler*) { return S_OK; }
    IFACEMETHODIMP Execute(UINT32 id, UI_EXECUTIONVERB verb, const PROPERTYKEY*, const PROPVARIANT*, IUISimplePropertySet*) {
        if (verb == UI_EXECUTIONVERB_EXECUTE) PostMessageW(g_main, WM_APP_RIBBON, id, 0);
        return S_OK;
    }
    IFACEMETHODIMP UpdateProperty(UINT32 id, REFPROPERTYKEY key, const PROPVARIANT*, PROPVARIANT* nv) {
        return RibbonProp(id, key, nv);
    }
};
static RibbonHost g_ribbonHost;

static void RibbonRefresh() {
    if (!g_fw) return;
    g_fw->InvalidateUICommand(UI_ALL_COMMANDS, UI_INVALIDATIONS_STATE, nullptr);
    g_fw->InvalidateUICommand(UI_ALL_COMMANDS, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_BooleanValue);
    g_fw->InvalidateUICommand(cmdTabGroupDrive, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_ContextAvailable);
    g_fw->InvalidateUICommand(cmdTabGroupPicture, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_ContextAvailable);
}

static void CreateRibbon() {
    if (FAILED(CoCreateInstance(CLSID_UIRibbonFramework, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_fw)))) {
        g_fw = nullptr;
        return;
    }
    if (FAILED(g_fw->Initialize(g_main, &g_ribbonHost)) || FAILED(g_fw->LoadUI(g_inst, L"APPLICATION_RIBBON"))) {
        g_fw->Destroy();
        g_fw->Release();
        g_fw = nullptr;   // 리본 없이도 동작한다
    }
}

static void SaveRibbon() {
    IUIRibbon* r = nullptr;
    IStream* st = nullptr;
    if (g_fw && !g_dir.empty() && SUCCEEDED(g_fw->GetView(0, IID_PPV_ARGS(&r))) &&
        SUCCEEDED(SHCreateStreamOnFileEx((g_dir + L"\\ribbon.dat").c_str(), STGM_CREATE | STGM_WRITE,
                                         FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &st)))
        r->SaveSettingsToStream(st);
    if (st) st->Release();
    if (r) r->Release();
}

static void DestroyRibbon() {
    SaveRibbon();
    if (g_fw) { g_fw->Destroy(); g_fw->Release(); g_fw = nullptr; }
    if (g_imgFactory) { g_imgFactory->Release(); g_imgFactory = nullptr; }
    g_ribbonH = 0;
}

// ---- 설정 / 세션 (SPEC §11) ---------------------------------------------------
static void InitSettingsPath() {
    PWSTR app = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app))) return;
    g_dir = std::wstring(app) + L"\\ExtendedExplorer";
    CoTaskMemFree(app);
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_ini = g_dir + L"\\settings.ini";
}

// WritePrivateProfileStringW는 파일이 UTF-16 BOM으로 시작해야 유니코드(한글 경로·이름)로 쓴다.
// 없으면 ANSI 파일을 만들어 버리므로 쓰기 전에 항상 확인한다 (실행 중 파일을 지운 경우 포함).
static void EnsureUnicodeIni() {
    HANDLE f = CreateFileW(g_ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;   // 이미 있음
    WORD bom = 0xFEFF;
    DWORD w = 0;
    WriteFile(f, &bom, 2, &w, nullptr);
    CloseHandle(f);
}

static std::wstring IniStr(const wchar_t* sec, const wchar_t* key) {
    if (g_ini.empty()) return L"";
    std::vector<wchar_t> buf(32768);
    GetPrivateProfileStringW(sec, key, L"", buf.data(), (DWORD)buf.size(), g_ini.c_str());
    return buf.data();
}
// 음수도 읽는다 (GetPrivateProfileInt는 음수를 0으로 바꾼다. 보조 모니터 창 위치는 음수일 수 있다)
static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring s = IniStr(sec, key);
    return s.empty() ? def : _wtoi(s.c_str());
}
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    if (g_ini.empty()) return;
    EnsureUnicodeIni();
    WritePrivateProfileStringW(sec, key, v.c_str(), g_ini.c_str());
}
static void IniSetInt(const wchar_t* sec, const wchar_t* key, int v) { IniSet(sec, key, std::to_wstring(v)); }

// 분할 트리 직렬화: 선순위, "V0.5000;" / "H0.3000;" = Split, "P;" = Pane (경로는 잎 순서대로 따로 저장)
static void SerializeTree(Node* n, std::wstring& out, std::vector<Node*>& leaves) {
    if (n->isPane) { out += L"P;"; leaves.push_back(n); return; }
    wchar_t b[32];
    swprintf_s(b, L"%c%.4f;", n->dir == DIR_V ? L'V' : L'H', n->ratio);
    out += b;
    SerializeTree(n->a, out, leaves);
    SerializeTree(n->b, out, leaves);
}

// 깨진 문자열이면 nullptr (만들던 것은 해제). pidls는 잎 순서, 모자라면 마지막 것을 쓴다.
static Node* ParseTree(const wchar_t*& s, Tab* t, const std::vector<PIDLIST_ABSOLUTE>& pidls, size_t& idx) {
    if (*s == L'P' && s[1] == L';') {
        s += 2;
        PCIDLIST_ABSOLUTE p = pidls.empty() ? nullptr : pidls[min(idx, pidls.size() - 1)];
        ++idx;
        return NewPane(t, p);
    }
    if (*s != L'V' && *s != L'H') return nullptr;
    int dir = *s == L'V' ? DIR_V : DIR_H;
    wchar_t* end = nullptr;
    float ratio = wcstof(s + 1, &end);
    if (!end || *end != L';') return nullptr;
    s = end + 1;
    Node* a = ParseTree(s, t, pidls, idx);
    if (!a) return nullptr;
    Node* b = ParseTree(s, t, pidls, idx);
    if (!b) { FreeSubtree(a); return nullptr; }
    Node* sp = new Node;
    sp->isPane = false;
    sp->dir = dir;
    sp->ratio = min(0.95f, max(0.05f, ratio));
    sp->a = a; sp->b = b;
    a->parent = b->parent = sp;
    return sp;
}

static void SaveSession() {
    int old = IniInt(L"Session", L"TabCount", 0);
    for (int i = 0; i < old; ++i) {
        std::wstring sec = L"Tab" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, g_ini.c_str());   // 구역 삭제
    }
    int n = g_restore ? (int)g_tabs.size() : 0;
    IniSetInt(L"Session", L"TabCount", n);
    IniSetInt(L"Session", L"Current", max(0, g_cur));
    for (int i = 0; i < n; ++i) {
        Tab* t = g_tabs[i];
        std::wstring sec = L"Tab" + std::to_wstring(i), layout;
        std::vector<Node*> leaves;
        SerializeTree(t->root, layout, leaves);
        IniSet(sec.c_str(), L"Layout", layout);
        int focus = 0;
        for (size_t k = 0; k < leaves.size(); ++k) {
            if (leaves[k] == t->focus) focus = (int)k;
            PWSTR p = nullptr;
            std::wstring path;
            if (CurPidl(leaves[k]) && SUCCEEDED(SHGetNameFromIDList(CurPidl(leaves[k]), SIGDN_DESKTOPABSOLUTEPARSING, &p))) {
                path = p;
                CoTaskMemFree(p);
            }
            IniSet(sec.c_str(), (L"Path" + std::to_wstring(k)).c_str(), path);
        }
        IniSetInt(sec.c_str(), L"Focus", focus);
        IniSetInt(sec.c_str(), L"Fixed", t->fixedName ? 1 : 0);
        IniSet(sec.c_str(), L"Name", t->fixedName ? t->name : L"");
    }
}

static PIDLIST_ABSOLUTE DefaultPidl();

// 복원한 탭이 있으면 true. 경로를 못 찾으면(지운 폴더, 검색 결과 등) 기본 위치.
static bool RestoreSession() {
    int n = IniInt(L"Session", L"TabCount", 0);
    int before = (int)g_tabs.size();
    for (int i = 0; i < n; ++i) {
        std::wstring sec = L"Tab" + std::to_wstring(i);
        std::wstring layout = IniStr(sec.c_str(), L"Layout");
        size_t leaves = std::count(layout.begin(), layout.end(), L'P');
        if (!leaves) continue;
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (size_t k = 0; k < leaves; ++k) {
            std::wstring path = IniStr(sec.c_str(), (L"Path" + std::to_wstring(k)).c_str());
            PIDLIST_ABSOLUTE p = nullptr;
            if (path.empty() || FAILED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr))) p = DefaultPidl();
            pidls.push_back(p);
        }
        Tab* t = new Tab;
        const wchar_t* s = layout.c_str();
        size_t idx = 0;
        t->root = ParseTree(s, t, pidls, idx);
        for (PIDLIST_ABSOLUTE p : pidls) ILFree(p);
        if (!t->root) { delete t; continue; }
        std::vector<Node*> v;
        CollectLeaves(t->root, v);
        t->focus = v[min((size_t)max(0, IniInt(sec.c_str(), L"Focus", 0)), v.size() - 1)];
        t->fixedName = IniInt(sec.c_str(), L"Fixed", 0) != 0;
        t->name = IniStr(sec.c_str(), L"Name");
        if (t->name.empty()) t->fixedName = false;
        InsertTab((int)g_tabs.size(), t, false);
    }
    int got = (int)g_tabs.size() - before;
    if (got > 0) SelectTab(before + min(max(0, IniInt(L"Session", L"Current", 0)), got - 1));
    return got > 0;
}

static void LoadSettings() {
    g_restore   = IniInt(L"General", L"RestoreSession", 1) != 0;
    g_openInTab = IniInt(L"General", L"OpenFoldersInNewTab", 1) != 0;
    g_sideOn    = IniInt(L"General", L"Sidebar", 1) != 0;
    g_sideW96   = min(800, max(80, IniInt(L"General", L"SidebarWidth", 220)));
    g_infoMode  = min(2, max(0, IniInt(L"General", L"InfoPane", 0)));
    g_infoW96   = min(1200, max(160, IniInt(L"General", L"InfoPaneWidth", 300)));
    g_themePref = min(2, max(0, IniInt(L"General", L"Theme", 0)));
}

// 창 위치는 보이는 모니터 안일 때만 쓴다 (모니터를 뺀 경우)
static int ApplyWindowPlacement(int show) {
    if (IniStr(L"Window", L"Left").empty()) return show;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    GetWindowPlacement(g_main, &wp);
    RECT r = { IniInt(L"Window", L"Left", 0), IniInt(L"Window", L"Top", 0),
               IniInt(L"Window", L"Right", 0), IniInt(L"Window", L"Bottom", 0) };
    if (r.right - r.left < 200 || r.bottom - r.top < 150 || !MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return show;
    wp.rcNormalPosition = r;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(g_main, &wp);
    bool normal = show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW;
    return (normal && IniInt(L"Window", L"Maximized", 0)) ? SW_SHOWMAXIMIZED : show;
}

static void SaveSettings() {
    if (g_ini.empty()) return;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (g_main && GetWindowPlacement(g_main, &wp)) {
        IniSetInt(L"Window", L"Left", wp.rcNormalPosition.left);
        IniSetInt(L"Window", L"Top", wp.rcNormalPosition.top);
        IniSetInt(L"Window", L"Right", wp.rcNormalPosition.right);
        IniSetInt(L"Window", L"Bottom", wp.rcNormalPosition.bottom);
        IniSetInt(L"Window", L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
    IniSetInt(L"General", L"RestoreSession", g_restore);
    IniSetInt(L"General", L"OpenFoldersInNewTab", g_openInTab);
    IniSetInt(L"General", L"Sidebar", g_sideOn);
    IniSetInt(L"General", L"SidebarWidth", g_sideW96);
    IniSetInt(L"General", L"InfoPane", g_infoMode);
    IniSetInt(L"General", L"InfoPaneWidth", g_infoW96);
    SaveSession();
}

// 다른 프로세스(폴더 열기)가 보낸 경로를 새 탭으로 열고 앞으로 가져온다
static bool OpenFromOtherProcess(const COPYDATASTRUCT* cd) {
    if (!cd || cd->dwData != COPY_OPEN || cd->cbData < sizeof(wchar_t)) return false;
    std::wstring path((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
    path.resize(wcsnlen(path.c_str(), path.size()));
    PIDLIST_ABSOLUTE p = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &p, 0, nullptr))) p = DefaultPidl();
    NewTab(p);
    ILFree(p);
    if (IsIconic(g_main)) ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
    return true;
}

// ---- 테마 (SPEC §12: 시스템 라이트/다크) --------------------------------------
static bool SystemDark() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// 공개 API가 없어 uxtheme 서수 함수를 쓴다 (Win10 1903+, 메모장++·터미널 등과 같은 방식). 없으면 건너뜀.
// ponytail: 1809의 #135는 BOOL 인자라 '라이트'가 '다크 허용'이 된다. 1809 지원이 필요하면 빌드 번호로 분기.
static HMODULE Ux() {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return ux;
}
static void AllowDarkForWindow(HWND h, bool dark) {
    using Fn = BOOL (WINAPI*)(HWND, BOOL);   // #133 AllowDarkModeForWindow
    if (Ux()) if (Fn f = (Fn)GetProcAddress(Ux(), MAKEINTRESOURCEA(133))) f(h, dark);
}

static void ThemeWindow(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    AllowDarkForWindow(h, g_dark);
    const wchar_t* th = nullptr;
    if (!lstrcmpW(cls, L"SysTreeView32") || !lstrcmpW(cls, L"ScrollBar")) th = g_dark ? L"DarkMode_Explorer" : L"Explorer";
    else if (!lstrcmpW(cls, L"DirectUIHWND") || !lstrcmpW(cls, L"SysHeader32")) th = g_dark ? L"DarkMode_ItemsView" : L"ItemsView";
    else if (h == g_pvText) th = g_dark ? L"DarkMode_Explorer" : L"Explorer";   // 텍스트 미리 보기의 스크롤바
    if (th) SetWindowTheme(h, th, nullptr);
    if (!lstrcmpW(cls, L"SysTreeView32")) {   // 트리 배경·글자는 테마가 아니라 컨트롤 색
        TreeView_SetBkColor(h, g_dark ? C_BOX : (COLORREF)-1);
        TreeView_SetTextColor(h, g_dark ? C_TEXT : (COLORREF)-1);
        TreeView_SetIndent(h, TreeIndent());   // 탐색기 탐색 창처럼 좁은 들여쓰기
    }
    SendMessageW(h, WM_THEMECHANGED, 0, 0);
}

static void ThemeWindowTree(HWND root) {
    if (!root) return;
    ThemeWindow(root);
    EnumChildWindows(root, [](HWND h, LPARAM) -> BOOL { ThemeWindow(h); return TRUE; }, 0);
}

// 리본: 비공개 DarkModeRibbon 키(WordPad·그림판이 쓰는 2004)를 먼저, 안 되면 전역 색으로 어둡게
static const PROPERTYKEY kDarkModeRibbon = { { 2004, 0x7363, 0x696e, { 0x84, 0x41, 0x79, 0x8a, 0xcf, 0x5a, 0xeb, 0xb7 } }, VT_BOOL };
static bool g_ribbonDarkKey = false;   // 시험용: 어느 방식이 먹혔나

static void ThemeRibbon() {
    IPropertyStore* ps = nullptr;
    if (!g_fw || FAILED(g_fw->QueryInterface(IID_PPV_ARGS(&ps)))) return;
    PROPVARIANT v;
    InitPropVariantFromBoolean(g_dark, &v);
    g_ribbonDarkKey = SUCCEEDED(ps->SetValue(kDarkModeRibbon, v));
    if (!g_ribbonDarkKey && g_dark) {
        InitPropVariantFromUInt32(UI_HSB(0, 0, 40), &v);   ps->SetValue(UI_PKEY_GlobalBackgroundColor, v);
        InitPropVariantFromUInt32(UI_HSB(0, 0, 255), &v);  ps->SetValue(UI_PKEY_GlobalTextColor, v);
        InitPropVariantFromUInt32(UI_HSB(0, 0, 80), &v);   ps->SetValue(UI_PKEY_GlobalHighlightColor, v);
    }
    ps->Commit();
    ps->Release();
    // 회색 아이콘은 어두운 리본에서 안 보이므로 다시 받는다 (CmdImage가 밝게 그린다)
    g_fw->InvalidateUICommand(UI_ALL_COMMANDS, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_LargeImage);
    g_fw->InvalidateUICommand(UI_ALL_COMMANDS, UI_INVALIDATIONS_PROPERTY, &UI_PKEY_SmallImage);
}

static void ApplyTheme() {
    g_dark = g_themePref == 2 || (g_themePref == 0 && SystemDark());
    ApplyPalette(g_dark ? kDark : kLight);
    using SetMode = int (WINAPI*)(int);   // #135 SetPreferredAppMode: 2 ForceDark, 3 ForceLight
    using Flush = void (WINAPI*)();       // #136 FlushMenuThemes (셸 컨텍스트 메뉴)
    if (Ux()) {
        if (SetMode f = (SetMode)GetProcAddress(Ux(), MAKEINTRESOURCEA(135))) f(g_dark ? 2 : 3);
        if (Flush f = (Flush)GetProcAddress(Ux(), MAKEINTRESOURCEA(136))) f();
    }
    if (!g_main) return;
    BOOL d = g_dark;
    DwmSetWindowAttribute(g_main, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
    ThemeWindowTree(g_main);   // 사이드바, 각 Pane의 뷰, 스크롤바
    ThemeRibbon();
    if (g_pvKind == PV_HANDLER) { StopPreview(); ScheduleInfo(); }   // 처리기 색은 만들 때 정한다 → 다시 띄움
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// ---- 미리 보기 / 세부 정보 창 -------------------------------------------------
// SPEC §3·§13: 썸네일 + 속성. IPreviewHandler(실시간 문서 미리보기)는 만들지 않는다.
// 미리 보기 = 큰 썸네일 + 주요 속성, 세부 정보 = 작은 썸네일 + 속성 전체 (Win10 PreviewDetails 목록).
struct InfoData {
    HBITMAP thumb = nullptr;
    int tw = 0, th = 0;
    std::wstring title, sub;
    std::vector<std::pair<std::wstring, std::wstring>> props;
};
static InfoData g_info;
static HFONT g_fontTitle = nullptr;

static void ClearInfo() {
    if (g_info.thumb) DeleteObject(g_info.thumb);
    g_info = InfoData();
}

// ponytail: UI 스레드에서 동기로 만든다. 느린 위치(네트워크·큰 동영상)에서 끊기면 작업 스레드로.
static HBITMAP ItemThumb(IShellItem* si, int px, int* w, int* h) {
    IShellItemImageFactory* f = nullptr;
    HBITMAP hb = nullptr;
    if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&f)))) {
        SIZE sz = { px, px };
        if (FAILED(f->GetImage(sz, SIIGBF_RESIZETOFIT, &hb))) hb = nullptr;
        f->Release();
    }
    DIBSECTION ds = {};
    if (!hb || GetObjectW(hb, sizeof(ds), &ds) != sizeof(ds)) {
        BITMAP bm = {};
        if (hb && GetObjectW(hb, sizeof(bm), &bm)) { *w = bm.bmWidth; *h = bm.bmHeight; }
        return hb;
    }
    *w = ds.dsBm.bmWidth;
    *h = ds.dsBm.bmHeight;
    // AlphaBlend는 premultiplied 알파를 기대한다. 셸 썸네일은 아닐 때가 있어 가장자리가 하얗게 번진다.
    // 알파가 전부 0이면(알파 없는 이미지) 불투명으로 본다.
    if (ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits) {
        BYTE* p = (BYTE*)ds.dsBm.bmBits;
        size_t n = (size_t)ds.dsBm.bmWidth * abs(ds.dsBm.bmHeight);
        bool premul = true, anyAlpha = false;
        for (size_t i = 0; i < n; ++i) {
            BYTE a = p[i * 4 + 3];
            if (a) anyAlpha = true;
            if (p[i * 4] > a || p[i * 4 + 1] > a || p[i * 4 + 2] > a) premul = false;
        }
        for (size_t i = 0; i < n && !(premul && anyAlpha); ++i) {
            BYTE* q = p + i * 4;
            if (!anyAlpha) { q[3] = 255; continue; }
            q[0] = (BYTE)(q[0] * q[3] / 255);
            q[1] = (BYTE)(q[1] * q[3] / 255);
            q[2] = (BYTE)(q[2] * q[3] / 255);
        }
    }
    return hb;
}

static void ItemProps(IShellItem* si, size_t limit, std::vector<std::pair<std::wstring, std::wstring>>& out) {
    IShellItem2* s2 = nullptr;
    if (FAILED(si->QueryInterface(IID_PPV_ARGS(&s2)))) return;
    IPropertyDescriptionList* pl = nullptr;
    if (SUCCEEDED(s2->GetPropertyDescriptionList(PKEY_PropList_PreviewDetails, IID_PPV_ARGS(&pl)))) {
        UINT n = 0;
        pl->GetCount(&n);
        for (UINT i = 0; i < n && out.size() < limit; ++i) {
            IPropertyDescription* pd = nullptr;
            if (FAILED(pl->GetAt(i, IID_PPV_ARGS(&pd)))) continue;
            PROPERTYKEY k;
            PROPVARIANT v;
            PropVariantInit(&v);
            PWSTR label = nullptr, val = nullptr;
            if (SUCCEEDED(pd->GetPropertyKey(&k)) && SUCCEEDED(s2->GetProperty(k, &v)) && v.vt != VT_EMPTY &&
                SUCCEEDED(pd->GetDisplayName(&label)) && label &&
                SUCCEEDED(pd->FormatForDisplay(v, PDFF_DEFAULT, &val)) && val && *val)
                out.push_back({ label, val });
            CoTaskMemFree(label);
            CoTaskMemFree(val);
            PropVariantClear(&v);
            pd->Release();
        }
        pl->Release();
    }
    s2->Release();
}

// ---- 실시간 미리 보기 (탐색기 미리 보기 창과 같은 방식) ------------------------
// 1) 형식에 등록된 미리 보기 처리기(IPreviewHandler): 텍스트·PDF·Office·한글·HTML·음악 …
//    탐색기처럼 별도 프로세스(prevhost.exe)에 띄우므로 처리기가 죽어도 앱은 살아 있다.
// 2) 처리기가 없지만 내용이 텍스트(.md, .json, .log …): 읽기 전용 텍스트 상자
// 3) 그 밖(그림·동영상 등): 큰 썸네일 + 속성 (세부 정보 창도 같은 경로)
//
// 스레드: 처리기 생성·초기화·그리기, 텍스트 읽기, 썸네일·속성은 모두 '미리 보기 스레드'(자체 STA +
// 메시지 루프)에서 한다. UI 스레드는 선택이 바뀌면 요청만 보내고, 결과가 오면 그것이 최신 요청일 때만
// 적용한다. 큰 PDF·Office 문서를 여는 동안에도 파일 목록·창이 멈추지 않는다.
// 스레드 사이에는 COM 객체를 넘기지 않고 PIDL·경로 같은 값만 넘긴다. 처리기 객체는 그 스레드만 만진다.
static const wchar_t* const kPreviewHandlerIID = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
static const UINT WM_PVW_REQ  = WM_APP + 20;   // 미리 보기 스레드: lp = InfoReq*
static const UINT WM_PVW_SIZE = WM_APP + 21;   // 미리 보기 스레드: lp = MAKELPARAM(cx, cy)
static const UINT WM_APP_INFO = WM_APP + 22;   // UI 스레드: lp = InfoRes*
static HANDLE g_pvThread = nullptr;
static DWORD  g_pvTid = 0;
static volatile LONG g_infoGen = 0;            // 가장 최근 요청 번호. 미리 보기 스레드는 이것과 비교해 낡은 작업을 버린다.
static HWND   g_pvKeepFocus = nullptr;         // 처리기가 포커스를 가져가면 돌려줄 곳 (탐색기도 목록에 포커스를 둔다)

struct InfoReq {
    LONG gen = 0;
    int mode = INFO_NONE;                 // INFO_NONE = 처리기 내리기만
    PIDLIST_ABSOLUTE pidl = nullptr;      // 선택 1개면 그 항목, 0개면 현재 폴더, 여러 개면 null
    DWORD sel = 0;
    std::wstring status1, status2;        // "N개 항목" / "N개 항목 선택함 크기"
    int thumbPx = 0;
    HWND host = nullptr;
    RECT rc = {};                         // 미리 보기 창 클라이언트 크기
    COLORREF bg = 0, fg = 0;
    LOGFONTW lf = {};
    ~InfoReq() { ILFree(pidl); }
};
struct InfoRes {
    LONG gen = 0;
    InfoData data;
    int pvKind = PV_NONE;
    std::wstring text;
};

static bool Superseded(const InfoReq* r) { return r->gen != InterlockedCompareExchange(&g_infoGen, 0, 0); }

static bool InitHandler(IPreviewHandler* ph, IShellItem* si, const std::wstring& path) {
    IInitializeWithStream* ws = nullptr;
    IInitializeWithItem* wi = nullptr;
    IInitializeWithFile* wf = nullptr;
    bool ok = false;
    if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&ws)))) {
        IStream* st = nullptr;
        if (SUCCEEDED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st))) {
            ok = SUCCEEDED(ws->Initialize(st, STGM_READ));
            st->Release();
        }
        ws->Release();
    } else if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&wi)))) {
        ok = SUCCEEDED(wi->Initialize(si, STGM_READ));
        wi->Release();
    } else if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&wf)))) {
        ok = SUCCEEDED(wf->Initialize(path.c_str(), STGM_READ));
        wf->Release();
    }
    return ok;
}

// 처리기가 없을 때: 앞부분이 텍스트로 보이면 (NUL 바이트 없음) 읽어서 보여 준다. 256KB까지.
static bool ReadTextPreview(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    const DWORD limit = 256 * 1024;
    std::string b((size_t)min((LONGLONG)limit, size.QuadPart), '\0');
    DWORD got = 0;
    if (!b.empty()) ReadFile(f, &b[0], (DWORD)b.size(), &got, nullptr);
    CloseHandle(f);
    b.resize(got);
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) {   // UTF-16 LE
        out.assign((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    } else {
        size_t skip = (b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;   // UTF-8 BOM
        if (b.find('\0', skip) != std::string::npos) return false;                       // 이진 파일
        UINT cp = CP_UTF8;
        int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, b.data() + skip, (int)(b.size() - skip), nullptr, 0);
        if (n == 0 && b.size() > skip) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, b.data() + skip, (int)(b.size() - skip), nullptr, 0); }
        out.assign((size_t)n, L'\0');
        if (n) MultiByteToWideChar(cp, 0, b.data() + skip, (int)(b.size() - skip), &out[0], n);
    }
    std::wstring crlf;   // EDIT는 줄바꿈이 \r\n이어야 한다
    crlf.reserve(out.size() + out.size() / 16);
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\n' && (i == 0 || out[i - 1] != L'\r')) crlf += L'\r';
        crlf += out[i];
    }
    if ((ULONGLONG)size.QuadPart > limit) crlf += L"\r\n…";
    out.swap(crlf);
    return true;
}

// ---- 미리 보기 스레드 쪽 (이 아래 세 함수는 미리 보기 스레드에서만 부른다) ----
static IPreviewHandler* t_ph = nullptr;   // 지금 떠 있는 처리기
static std::wstring t_phPath;             // 그 파일 (같은 파일이면 다시 만들지 않는다)

static void UnloadHandler() {
    if (t_ph) { t_ph->Unload(); t_ph->Release(); t_ph = nullptr; }
    t_phPath.clear();
}

// 처리기로 미리 보기. 성공하면 처리기를 남겨 두고 true.
static bool TryHandler(const InfoReq* r, IShellItem* si, const std::wstring& path) {
    wchar_t clsidStr[64];
    DWORD n = 64;
    CLSID clsid;
    if (FAILED(AssocQueryStringW(0, ASSOCSTR_SHELLEXTENSION, PathFindExtensionW(path.c_str()), kPreviewHandlerIID, clsidStr, &n)) ||
        FAILED(CLSIDFromString(clsidStr, &clsid))) return false;
    IPreviewHandler* ph = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&ph))) &&
        FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ph)))) return false;
    bool ok = false;
    if (!Superseded(r) && InitHandler(ph, si, path) && !Superseded(r)) {
        RECT rc = r->rc;
        ph->SetWindow(r->host, &rc);
        IPreviewHandlerVisuals* vis = nullptr;   // 다크 테마 색·글꼴 (지원하는 처리기만)
        if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&vis)))) {
            vis->SetBackgroundColor(r->bg);
            vis->SetTextColor(r->fg);
            LOGFONTW lf = r->lf;
            vis->SetFont(&lf);
            vis->Release();
        }
        ok = SUCCEEDED(ph->DoPreview());   // 큰 문서는 여기서 오래 걸린다 (UI는 멈추지 않음)
        if (ok && Superseded(r)) ok = false;
        if (!ok) ph->Unload();
    }
    if (ok) { t_ph = ph; t_phPath = path; }
    else ph->Release();
    return ok;
}

static InfoRes* WorkInfo(const InfoReq* r) {
    InfoRes* res = new InfoRes;
    res->gen = r->gen;
    if (r->mode == INFO_NONE) { UnloadHandler(); return res; }
    if (!r->pidl) { UnloadHandler(); res->data.title = r->status2; res->data.sub = r->status1; return res; }
    IShellItem* si = nullptr;
    if (FAILED(SHCreateItemFromIDList(r->pidl, IID_PPV_ARGS(&si)))) { UnloadHandler(); return res; }
    if (r->mode == INFO_PREVIEW && r->sel == 1) {
        SFGAOF at = 0;
        si->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &at);
        PWSTR p = nullptr;
        if (!((at & SFGAO_FOLDER) && !(at & SFGAO_STREAM)) && SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            std::wstring path = p;
            CoTaskMemFree(p);
            if (t_ph && path == t_phPath) { res->pvKind = PV_HANDLER; si->Release(); return res; }
            UnloadHandler();
            if (TryHandler(r, si, path)) { res->pvKind = PV_HANDLER; si->Release(); return res; }
            if (Superseded(r)) { si->Release(); return res; }
            if (ReadTextPreview(path, res->text)) { res->pvKind = PV_TEXT; si->Release(); return res; }
        }
    }
    UnloadHandler();
    if (!Superseded(r)) {   // 썸네일 + 속성
        res->data.thumb = ItemThumb(si, r->thumbPx, &res->data.tw, &res->data.th);
        PWSTR s = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &s))) { res->data.title = s; CoTaskMemFree(s); }
        IShellItem2* s2 = nullptr;
        if (r->sel == 0) res->data.sub = r->status1;   // "N개 항목"
        else if (SUCCEEDED(si->QueryInterface(IID_PPV_ARGS(&s2)))) {
            if (SUCCEEDED(s2->GetString(PKEY_ItemTypeText, &s))) { res->data.sub = s; CoTaskMemFree(s); }
            s2->Release();
        }
        ItemProps(si, r->mode == INFO_PREVIEW ? 4 : 64, res->data.props);
    }
    si->Release();
    return res;
}

static DWORD WINAPI PreviewThread(void* ready) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // 메시지 큐를 만든 뒤에 알린다
    SetEvent((HANDLE)ready);
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_PVW_REQ) {
            InfoReq* r = (InfoReq*)m.lParam;
            MSG next;   // 밀린 요청이 있으면 마지막 것만
            while (PeekMessageW(&next, nullptr, WM_PVW_REQ, WM_PVW_REQ, PM_REMOVE)) { delete r; r = (InfoReq*)next.lParam; }
            InfoRes* res = WorkInfo(r);
            delete r;
            if (!PostMessageW(g_main, WM_APP_INFO, 0, (LPARAM)res)) {
                if (res->data.thumb) DeleteObject(res->data.thumb);
                delete res;
            }
        } else if (m.message == WM_PVW_SIZE) {
            RECT rc = { 0, 0, LOWORD(m.lParam), HIWORD(m.lParam) };
            if (t_ph) t_ph->SetRect(&rc);
        } else {
            TranslateMessage(&m);   // 처리기·COM이 쓰는 메시지
            DispatchMessageW(&m);
        }
    }
    UnloadHandler();
    CoUninitialize();
    return 0;
}

// ---- UI 스레드 쪽 ----
static void StartPreviewThread() {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_pvThread = CreateThread(nullptr, 0, PreviewThread, ready, 0, &g_pvTid);
    if (g_pvThread) WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

static void StopPreviewThread() {
    if (!g_pvThread) return;
    InterlockedIncrement(&g_infoGen);   // 하던 일은 버리게
    PostThreadMessageW(g_pvTid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_pvThread, 5000);   // 처리기를 내리고 끝날 때까지 (멈춘 처리기 대비 제한 시간)
    CloseHandle(g_pvThread);
    g_pvThread = nullptr;
}

static void PlacePreview() {
    if (!g_pvHost) return;
    RECT r = InfoRc();
    bool on = g_infoMode == INFO_PREVIEW && g_pvKind != PV_NONE && r.right > r.left;
    SetWindowPos(g_pvHost, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (on ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static void PostInfoRequest(InfoReq* r) {
    if (!g_pvThread || !PostThreadMessageW(g_pvTid, WM_PVW_REQ, 0, (LPARAM)r)) delete r;
}

// 화면에서 미리 보기를 내리고 처리기도 내리게 한다
static void StopPreview() {
    if (g_pvText) { ShowWindow(g_pvText, SW_HIDE); SetWindowTextW(g_pvText, L""); }
    g_pvKind = PV_NONE;
    if (g_pvHost) ShowWindow(g_pvHost, SW_HIDE);
    InfoReq* r = new InfoReq;
    r->gen = InterlockedIncrement(&g_infoGen);
    PostInfoRequest(r);
}

// 선택 1개 = 그 항목, 0개 = 현재 폴더, 여러 개 = 개수와 크기 (Win10과 같다). 결과는 WM_APP_INFO로 온다.
static void ComputeInfo() {
    Node* n = FocusNode();
    if (!g_infoMode || !n) { ClearInfo(); StopPreview(); return; }
    InfoReq* r = new InfoReq;
    r->mode = g_infoMode;
    if (IShellItemArray* a = SelectionArray()) {
        a->GetCount(&r->sel);
        IShellItem* si = nullptr;
        if (r->sel == 1 && SUCCEEDED(a->GetItemAt(0, &si))) { SHGetIDListFromObject(si, &r->pidl); si->Release(); }
        a->Release();
    }
    if (r->sel == 0 && CurPidl(n)) r->pidl = ILCloneFull(CurPidl(n));
    r->status1 = g_status1;
    r->status2 = g_status2;
    RECT ir = InfoRc();
    int w = (int)(ir.right - ir.left) - S(32);
    r->thumbPx = g_infoMode == INFO_PREVIEW ? max(S(48), min(w, S(512))) : S(96);
    r->host = g_pvHost;
    r->rc = RECT{ 0, 0, ir.right - ir.left, ir.bottom - ir.top };
    r->bg = C_BOX;
    r->fg = C_TEXT;
    GetObjectW(g_font, sizeof(r->lf), &r->lf);
    r->gen = InterlockedIncrement(&g_infoGen);
    g_pvKeepFocus = GetFocus();
    PostInfoRequest(r);
}

// 미리 보기 스레드의 결과. 그사이 선택이 또 바뀌었으면 버린다.
static void ApplyInfoResult(InfoRes* res) {
    if (res->gen != InterlockedCompareExchange(&g_infoGen, 0, 0) || !g_infoMode) {
        if (res->data.thumb) DeleteObject(res->data.thumb);
        delete res;
        return;
    }
    ClearInfo();
    g_info = std::move(res->data);
    g_pvKind = (g_infoMode == INFO_PREVIEW) ? res->pvKind : PV_NONE;
    if (g_pvText) {
        if (g_pvKind == PV_TEXT) {
            RECT rc;
            GetClientRect(g_pvHost, &rc);
            RECT ir = InfoRc();
            SetWindowPos(g_pvText, nullptr, 0, 0, ir.right - ir.left, ir.bottom - ir.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            SetWindowTextW(g_pvText, res->text.c_str());
        } else {
            ShowWindow(g_pvText, SW_HIDE);
            SetWindowTextW(g_pvText, L"");
        }
    }
    PlacePreview();
    RECT r = InfoRc();
    InvalidateRect(g_main, &r, FALSE);
    HWND f = GetFocus();   // 처리기가 포커스를 가져갔으면 돌려준다
    if (g_pvKeepFocus && IsWindow(g_pvKeepFocus) && f != g_pvKeepFocus && (!f || IsChild(g_pvHost, f)))
        SetFocus(g_pvKeepFocus);
    delete res;
}

static LRESULT CALLBACK PreviewHostProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        if (g_pvThread) PostThreadMessageW(g_pvTid, WM_PVW_SIZE, 0, lp);   // 처리기는 그 스레드가 맞춘다
        if (g_pvText) SetWindowPos(g_pvText, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRc((HDC)wp, rc, C_BOX);
        return 1;
    }
    case WM_CTLCOLORSTATIC:   // 읽기 전용 텍스트 상자
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, C_BOX);
        SetTextColor((HDC)wp, C_TEXT);
        SetDCBrushColor((HDC)wp, C_BOX);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void PaintInfo(HDC dc, RECT r) {
    FillRc(dc, r, C_BOX);
    int x = r.left + S(16), w = (int)(r.right - r.left) - S(32), y = r.top + S(16);
    if (w <= 0) return;
    if (g_info.thumb) {
        HDC md = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(md, g_info.thumb);
        int tw = min(g_info.tw, w), th = g_info.th * tw / max(1, g_info.tw);   // 창이 줄었으면 비율 유지 축소
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, r.left + ((int)(r.right - r.left) - tw) / 2, y, tw, th, md, 0, 0, g_info.tw, g_info.th, bf);
        SelectObject(md, ob);
        DeleteDC(md);
        y += th + S(14);
    }
    SetTextColor(dc, C_TEXT);
    HGDIOBJ of = SelectObject(dc, g_fontTitle ? g_fontTitle : g_font);
    RECT tr = { x, y, x + w, y };
    DrawTextW(dc, g_info.title.c_str(), -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    tr.right = x + w;
    tr.bottom = min(tr.bottom, (LONG)(y + S(80)));   // 긴 이름은 3줄쯤에서 자른다
    DrawTextW(dc, g_info.title.c_str(), -1, &tr, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    y = tr.bottom + S(2);
    int lh = g_fontH + S(6);
    SetTextColor(dc, C_DIM);
    RECT sr = { x, y, x + w, y + lh };
    DrawTextW(dc, g_info.sub.c_str(), -1, &sr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    y += lh + S(10);
    int lw = w * 2 / 5;
    for (auto& p : g_info.props) {
        if (y + lh > r.bottom) break;
        RECT lr = { x, y, x + lw, y + lh }, vr = { x + lw + S(8), y, x + w, y + lh };
        SetTextColor(dc, C_DIM);
        DrawTextW(dc, p.first.c_str(), -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, p.second.c_str(), -1, &vr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += lh;
    }
}

// 미리 보기와 세부 정보는 Win10처럼 둘 중 하나만. 같은 것을 다시 누르면 닫힘.
static void ToggleInfo(int mode) {
    g_infoMode = g_infoMode == mode ? INFO_NONE : mode;
    if (g_infoMode != INFO_PREVIEW) StopPreview();
    if (!g_infoMode) ClearInfo();
    Relayout();
    ComputeInfo();
    RibbonRefresh();
}

// ---- 메인 창 ---------------------------------------------------------------
static void MakeFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_glyph) DeleteObject(g_glyph);
    if (g_glyphSm) DeleteObject(g_glyphSm);
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    LOGFONTW tf = ncm.lfMessageFont;   // 정보 창 제목: 1.4배, 굵게
    tf.lfHeight = tf.lfHeight * 7 / 5;
    tf.lfWeight = FW_SEMIBOLD;
    g_fontTitle = CreateFontIndirectW(&tf);
    // 텍스트 미리 보기: 한글이 든 고정폭 글꼴이어야 한글·영문 크기가 같다
    // (Consolas는 한글이 없어 Windows가 대신 그리는 글꼴이 작게 나왔다). D2Coding → 돋움체(기본 설치).
    static HFONT mono = nullptr;
    if (mono) DeleteObject(mono);
    LOGFONTW mf = ncm.lfMessageFont;
    HDC sdc = GetDC(nullptr);
    LOGFONTW probe = {};
    probe.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW(probe.lfFaceName, L"D2Coding");
    bool hasD2 = false;
    EnumFontFamiliesExW(sdc, &probe, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *(bool*)p = true;
        return 0;
    }, (LPARAM)&hasD2, 0);
    ReleaseDC(nullptr, sdc);
    lstrcpyW(mf.lfFaceName, hasD2 ? L"D2Coding" : L"DotumChe");
    mf.lfCharSet = DEFAULT_CHARSET;
    mf.lfHeight = mf.lfHeight * 169 / 100;   // 본문 글꼴의 1.69배 (30%씩 두 번)
    mono = CreateFontIndirectW(&mf);
    if (g_pvText) SendMessageW(g_pvText, WM_SETFONT, (WPARAM)mono, TRUE);
    LOGFONTW lf = {};
    lstrcpyW(lf.lfFaceName, L"Segoe MDL2 Assets");
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfHeight = -S(15);
    g_glyph = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(11);
    g_glyphSm = CreateFontIndirectW(&lf);
    HDC dc = GetDC(nullptr);
    HGDIOBJ of = SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_fontH = tm.tmHeight;
    SelectObject(dc, of);
    ReleaseDC(nullptr, dc);
    if (g_addrEdit)   SendMessageW(g_addrEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_searchEdit) SendMessageW(g_searchEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_tabEdit)    SendMessageW(g_tabEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        if (g_tabEditing) EndTabRename(true, false);   // 탭 위치가 바뀐다
        Relayout();
        LayoutCrumbs();
        return 0;
    case WM_LBUTTONDBLCLK: {   // SPEC §5: 탭 더블클릭 = 이름 바꾸기
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int part, i = HitStrip(pt, &part);
        if (part == PART_TAB) { BeginTabRename(i); return 0; }
        return MainProc(h, WM_LBUTTONDOWN, wp, lp);   // 나머지는 두 번째 클릭으로 처리 (버튼 연타)
    }
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int part, i = HitStrip(pt, &part);
        if (i >= 0) {
            ClientToScreen(h, &pt);
            TabMenu(i, pt);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->hwndFrom == g_tip && nh->code == TTN_GETDISPINFOW) {
            static std::wstring text;
            text = TabPath(g_hoverTab);
            ((NMTTDISPINFOW*)lp)->lpszText = (LPWSTR)text.c_str();
            return 0;
        }
        break;
    }
    case WM_APP_ENDTAB:
        if (g_tabEditing && GetFocus() != g_tabEdit) EndTabRename(true, false);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT body = BodyRc();
        FillRc(dc, body, C_GAP);   // 자식이 덮지 않은 곳 = 분할선
        if (g_sideOn) {            // 사이드바 왼쪽 여백은 트리와 같은 색
            RECT sp = SideRc();
            sp.right = min(sp.right, sp.left + SidePad());
            FillRc(dc, sp, C_BOX);
        }
        Buffered(dc, RECT{ 0, StripTop(), Client().right, StripTop() + StripH() }, PaintStrip);
        Buffered(dc, RECT{ 0, NavTop(), Client().right, NavTop() + NavH() }, PaintNav);
        Buffered(dc, StatusRc(), PaintStatus);
        if (g_infoMode) Buffered(dc, InfoRc(), PaintInfo);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_INFO) {   // 선택이 빠르게 바뀌는 동안(방향키) 썸네일을 매번 만들지 않는다
            KillTimer(h, TIMER_INFO);
            ComputeInfo();
            RECT r = InfoRc();
            InvalidateRect(h, &r, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, C_BOX);
        SetTextColor((HDC)wp, C_TEXT);
        SetDCBrushColor((HDC)wp, C_BOX);
        return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && Cur()) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (HitSideSplit(pt) || HitInfoSplit(pt)) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
            if (Node* s = HitSplitter(Cur()->root, pt)) {
                SetCursor(LoadCursor(nullptr, s->dir == DIR_V ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
            if (HitNav(pt) == HN_ADDR || HitNav(pt) == HN_SEARCH) { SetCursor(LoadCursor(nullptr, IDC_IBEAM)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int part, i = HitStrip(pt, &part);
        if (part == PART_PLUS) { NewTabDefault(); return 0; }
        if (part == PART_CLOSE) { CloseTab(i); return 0; }
        if (part == PART_TAB) {
            SelectTab(i);
            g_tabDrag = i;
            SetCapture(h);
            return 0;
        }
        if (int hit = HitNav(pt)) { OnNavClick(hit); return 0; }
        BeginSplitDrag(pt);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_sideDrag) {
            RECT c = Client();
            int px = max(S(80), min((int)c.right - S(200), (int)pt.x));
            int w96 = MulDiv(px, 96, (int)g_dpi);
            if (w96 != g_sideW96) { g_sideW96 = w96; DragRelayout(); }
            return 0;
        }
        if (g_infoDrag) {
            RECT b = BodyRc();
            int px = max(S(160), min((int)(b.right - SideSplitRc().right) / 2, (int)(b.right - pt.x)));
            int w96 = MulDiv(px, 96, (int)g_dpi);
            if (w96 != g_infoW96) { g_infoW96 = w96; ScheduleInfo(); DragRelayout(); }   // 썸네일은 멈춘 뒤 폭에 맞춰 다시
            return 0;
        }
        if (g_drag) {
            RECT r = g_drag->rc;
            float v = g_drag->dir == DIR_V
                ? (float)(pt.x - r.left) / (float)max(1, (int)(r.right - r.left) - GAP)
                : (float)(pt.y - r.top)  / (float)max(1, (int)(r.bottom - r.top) - GAP);
            v = min(0.95f, max(0.05f, v));
            if (v != g_drag->ratio) { g_drag->ratio = v; DragRelayout(); }
            return 0;
        }
        if (g_tabDrag >= 0) {
            int n = (int)g_tabs.size();
            int to = min(n - 1, max(0, (int)((pt.x - S(8)) / max(1, TabW()))));
            if (to != g_tabDrag) {
                Tab* t = g_tabs[g_tabDrag];
                g_tabs.erase(g_tabs.begin() + g_tabDrag);
                g_tabs.insert(g_tabs.begin() + to, t);
                g_cur = g_tabDrag = to;
                InvalidateStrip();
            }
            return 0;
        }
        int part, i = HitStrip(pt, &part);
        SetHover(i, part, HitNav(pt));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        SetHover(-1, PART_NONE, HN_NONE);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_drag = nullptr;
        g_tabDrag = -1;
        g_sideDrag = false;
        g_infoDrag = false;
        return 0;
    case WM_MBUTTONUP: {   // SPEC §5: 가운데 클릭으로 닫기
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int part, i = HitStrip(pt, &part);
        if (i >= 0) CloseTab(i);
        return 0;
    }
    case WM_APP_ENDADDR:
        if (g_addrEditing && GetFocus() != g_addrEdit) EndAddrEdit(false);
        return 0;
    case WM_APP_RIBBON:
        RibbonExec((UINT)wp);
        return 0;
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR:
        if (g_cm3) {   // 새 항목 메뉴는 열릴 때 하위 항목을 채운다
            LRESULT r = 0;
            if (SUCCEEDED(g_cm3->HandleMenuMsg2(m, wp, lp, &r))) return r;
        }
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        MakeFonts();
        if (g_treeWnd) ThemeWindowTree(g_treeWnd);   // 사이드바 들여쓰기도 새 배율로
        if (Cur()) UpdateTabInfo(Cur());   // 브레드크럼 폭 다시 측정
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_APPCOMMAND:   // 키보드의 브라우저 뒤로/앞으로 키 (엄지 버튼은 메시지 루프에서 먼저 처리)
        switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_BROWSER_BACKWARD: GoHist(FocusNode(), -1); return TRUE;
        case APPCOMMAND_BROWSER_FORWARD:  GoHist(FocusNode(), +1); return TRUE;
        }
        break;
    case WM_SETTINGCHANGE:   // 설정 → 개인 설정 → 색에서 앱 모드를 바꿈
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") && g_themePref == 0) ApplyTheme();
        break;
    case WM_COPYDATA:
        return OpenFromOtherProcess((const COPYDATASTRUCT*)lp) ? TRUE : FALSE;
    case WM_APP_NAV: {   // 셸 콜백에서 미룬 제자리 이동. 그사이 Pane이 닫혔을 수 있다.
        bool alive = false;
        ForEachPane([&](Node* x) { if (x == (Node*)wp) alive = true; });
        if (alive) Navigate((Node*)wp, (PCIDLIST_ABSOLUTE)lp);
        ILFree((PIDLIST_ABSOLUTE)lp);
        return 0;
    }
    case WM_APP_INFO:      // 미리 보기 스레드의 결과
        ApplyInfoResult((InfoRes*)lp);
        return 0;
    case WM_DESTROY:
        StopPreview();         // 미리 보기 처리기(다른 프로세스)를 먼저 내린다
        StopPreviewThread();   // 그 스레드가 처리기를 내리고 끝날 때까지 기다린다
        SaveSettings();    // 탭이 살아 있을 때 (마지막 탭을 닫아 끝나면 빈 세션)
        DestroyRibbon();   // 리본 상태도 저장
        for (Tab* t : g_tabs) { FreeSubtree(t->root); if (t->icon) DestroyIcon(t->icon); delete t; }
        g_tabs.clear();
        g_cur = -1;
        DestroySidebar();
        FreeCrumbs();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

#ifdef SELFTEST
// ---- 자체 검사: 트리 조작, 방향 이동, 히스토리, 브레드크럼, 검색 URL -------------
int main() {
    CoInitialize(nullptr);
    Tab t;
    Node* a = NewPane(&t, nullptr);
    t.root = t.focus = a;
    Node* b = NewPane(&t, nullptr);
    InsertSplit(&t, a, b, DIR_V);          // a | b
    Node* c = NewPane(&t, nullptr);
    InsertSplit(&t, b, c, DIR_H);          // a | (b / c)
    std::vector<Node*> v;
    CollectLeaves(t.root, v);
    assert(v.size() == 3 && v[0] == a && v[1] == b && v[2] == c);

    HDWP d = nullptr;
    Layout(t.root, RECT{ 0, 0, 202, 102 }, d);
    assert(a->rc.right == 100 && b->rc.left == 102);
    assert(b->rc.bottom == 50 && c->rc.top == 52);
    assert(Neighbor(t.root, a, 1, 0) == b);     // 같은 거리면 선순위 앞쪽
    assert(Neighbor(t.root, c, -1, 0) == a);
    assert(Neighbor(t.root, b, 0, 1) == c);
    assert(Neighbor(t.root, c, 0, -1) == b);
    assert(Neighbor(t.root, a, -1, 0) == nullptr);
    assert(Neighbor(t.root, a, 0, 1) == nullptr);

    Node* sib = RemovePane(&t, b);             // a | c
    assert(sib == c && t.root->a == a && t.root->b == c && c->parent == t.root);
    FreePane(b);
    sib = RemovePane(&t, a);                   // c
    assert(t.root == c && !c->parent);
    FreePane(a);

    // 히스토리
    PIDLIST_ABSOLUTE A = nullptr, B = nullptr, C = nullptr;
    SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &A);
    SHGetKnownFolderIDList(FOLDERID_Documents, 0, nullptr, &B);
    SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &C);
    assert(A && B && C);
    OnPaneNavigated(c, A);
    OnPaneNavigated(c, B);
    assert(c->hist.size() == 2 && c->hi == 1);
    OnPaneNavigated(c, B);                     // 새로 고침: 쌓지 않음
    assert(c->hist.size() == 2);
    c->pend = 0; OnPaneNavigated(c, A);        // 뒤로
    assert(c->hist.size() == 2 && c->hi == 0 && c->pend == -1);
    c->pend = 1; OnPaneNavigated(c, B);        // 앞으로
    assert(c->hi == 1);
    c->pend = 0; OnPaneNavigated(c, A);
    OnPaneNavigated(c, C);                     // 새 이동: 앞쪽(B) 버림
    assert(c->hist.size() == 2 && c->hi == 1 && ILIsEqual(c->hist[1], C));
    // 검색어는 검색 결과를 벗어나면 지워진다
    c->query = L"x"; c->searching = true; OnPaneNavigated(c, A);
    assert(c->query == L"x");
    OnPaneNavigated(c, B);
    assert(c->query.empty());

    // 브레드크럼: 바탕 화면부터 문서까지
    BuildCrumbs(B);
    assert(g_crumbs.size() >= 2 && ILIsEmpty(g_crumbs[0].pidl) && ILIsEqual(g_crumbs.back().pidl, B));
    for (size_t i = 1; i < g_crumbs.size(); ++i) assert(!g_crumbs[i].name.empty() && g_crumbs[i].w > 0);
    FreeCrumbs();
    assert(FitCrumbs({ 18, 50, 50, 50 }, 168) == 1);   // 다 들어감
    assert(FitCrumbs({ 18, 50, 50, 50 }, 118) == 2);   // 앞 하나 숨김
    assert(FitCrumbs({ 18, 50, 50, 50 }, 10) == 3);    // 마지막은 남긴다

    // 검색 폴더: 우리 브라우저가 열 수 있는 PIDL이 나와야 한다 (한글·AQS 포함)
    PIDLIST_ABSOLUTE SR = BuildSearch(A, L"notepad");
    assert(SR);
    ILFree(SR);
    SR = BuildSearch(A, L"메모 kind:folder");
    assert(SR);
    ILFree(SR);
    assert(!BuildSearch(A, L""));

    // 리본: 보기 모드 구간
    assert(ViewCmd(FVM_ICON, 256) == cmdViewXL && ViewCmd(FVM_ICON, 96) == cmdViewL);
    assert(ViewCmd(FVM_ICON, 48) == cmdViewM && ViewCmd(FVM_ICON, 32) == cmdViewS);
    assert(ViewCmd(FVM_DETAILS, 16) == cmdViewDetails && ViewCmd(FVM_CONTENT, 32) == cmdViewContent);
    for (const ViewMode& vm : kViews) assert(ViewCmd(vm.mode, vm.size) == vm.id);   // 설정한 모드가 그대로 체크돼야
    // 리본: 글리프 비트맵에 실제로 그림이 있어야
    for (wchar_t g : { (wchar_t)0xE8C8, (wchar_t)1 }) {
        HBITMAP hb = GlyphBitmap(g, 32, IC_B);
        assert(hb);
        DIBSECTION ds;
        GetObjectW(hb, sizeof(ds), &ds);
        DWORD* px = (DWORD*)ds.dsBm.bmBits;
        int opaque = 0;
        for (int i = 0; i < 32 * 32; ++i) if (px[i] >> 24) ++opaque;
        assert(opaque > 20);
        DeleteObject(hb);
    }
    assert(IsImagePath(L"a.JPG") && IsImagePath(L"b.png") && !IsImagePath(L"c.txt"));
    // Ctrl+휠 단계: 자세히 → 목록 → 작은 아이콘 → 아이콘 32 → 커짐, 끝에서 멈춤
    {
        ViewStep v = NextView(FVM_DETAILS, 16, 1);
        assert(v.mode == FVM_LIST);
        v = NextView(FVM_DETAILS, 16, 3);
        assert(v.mode == FVM_ICON && v.size == 48);
        v = NextView(FVM_ICON, 48, 1);
        assert(v.mode == FVM_ICON && v.size == 54);
        v = NextView(FVM_ICON, 250, 3);
        assert(v.mode == FVM_ICON && v.size == 256);
        v = NextView(FVM_ICON, 54, -1);
        assert(v.mode == FVM_ICON && v.size == 48);
        v = NextView(FVM_ICON, 48, -1);
        assert(v.mode == FVM_SMALLICON);
        // 뷰가 '작은 아이콘'으로 보고해도 위로 가면 아이콘 48 (멈춰 있지 않음)
        v = NextView(FVM_SMALLICON, 16, 2);
        assert(v.mode == FVM_ICON && v.size == 54);
        v = NextView(FVM_SMALLICON, 16, -3);
        assert(v.mode == FVM_TILE);
        v = NextView(FVM_CONTENT, 32, -5);
        assert(v.mode == FVM_CONTENT);
        v = NextView(FVM_THUMBNAIL, 96, 0);
        assert(v.mode == FVM_ICON && v.size == 96);
        // 한쪽 끝까지 올렸다가 내리면 다시 자세히로 (단계가 되돌아온다)
        v = NextView(FVM_DETAILS, 16, 40);
        assert(v.mode == FVM_ICON && v.size == 256);
        v = NextView(v.mode, v.size, -60);
        assert(v.mode == FVM_CONTENT);
    }
    // 사진 도구 회전 동사: 2x1 BMP를 돌려 1x2가 되는지 (환경에 따라 없을 수 있어 정보로만)
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring f = std::wstring(tmp) + L"ee_rotate_test.bmp";
        unsigned char bmp[62] = { 'B', 'M', 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24 };
        bmp[34] = 8;   // 이미지 크기 (2px*3B → 4B 정렬 = 8)
        FILE* fp = nullptr;
        _wfopen_s(&fp, f.c_str(), L"wb");
        fwrite(bmp, 1, 62, fp);
        fclose(fp);
        PIDLIST_ABSOLUTE p = nullptr;
        SHParseDisplayName(f.c_str(), nullptr, &p, 0, nullptr);
        HRESULT hr = ItemVerb(p, "rotate90");
        ILFree(p);
        Sleep(500);
        _wfopen_s(&fp, f.c_str(), L"rb");
        unsigned char h[26] = {};
        fread(h, 1, 26, fp);
        fclose(fp);
        printf("rotate90 verb: hr=%08lx -> %dx%d (%s)\n", hr, h[18], h[22], (h[18] == 1 && h[22] == 2) ? "works" : "NOT supported");
        DeleteFileW(f.c_str());
    }

    // 탭: 백그라운드 삽입, 현재 탭 유지, 오른쪽/다른 탭 닫기
    {
        g_tabs.clear(); g_cur = -1;
        Tab* t0 = NewTabAt(0, nullptr, true);
        Tab* t1 = NewTabAt(1, nullptr, false);
        Tab* t2 = NewTabAt(2, nullptr, false);
        assert(g_cur == 0 && g_tabs.size() == 3);
        Tab* bg = NewTabAt(g_cur + 1, nullptr, false);       // 새 탭에서 열기: 바로 오른쪽, 선택 안 바뀜
        assert(g_tabs[1] == bg && g_tabs[0] == t0 && g_cur == 0);
        SelectTab(3);                                          // t2
        NewTabAt(0, nullptr, false);                           // 앞에 끼우면 현재 인덱스가 밀린다
        assert(g_cur == 4 && Cur() == t2);
        CloseTabsRight(1);                                     // 현재(t2)가 닫힘 → 1번이 선택
        assert(g_tabs.size() == 2 && g_cur == 1 && Cur() == t0);
        NewTabAt(2, nullptr, false);
        CloseOtherTabs(1);
        assert(g_tabs.size() == 1 && g_tabs[0] == t0 && g_cur == 0);
        (void)t1;
        // 복제: 구조·비율·포커스 위치
        Node* a2 = t0->root;
        Node* b2 = NewPane(t0, nullptr);
        InsertSplit(t0, a2, b2, DIR_V);
        t0->root->ratio = 0.3f;
        Node* c2 = NewPane(t0, nullptr);
        InsertSplit(t0, b2, c2, DIR_H);
        t0->focus = c2;
        DuplicateTab(0);
        Tab* d = g_tabs[1];
        assert(g_cur == 1 && !d->root->isPane && d->root->dir == DIR_V && d->root->ratio == 0.3f);
        assert(!d->root->b->isPane && d->root->b->dir == DIR_H && d->focus == d->root->b->b);
        assert(d->root->a->parent == d->root && d->root->b->b->parent == d->root->b);
        for (Tab* t : g_tabs) { FreeSubtree(t->root); delete t; }
        g_tabs.clear(); g_cur = -1;
    }

    // 세션: 직렬화 왕복, 깨진 문자열, ini 저장/복원 (임시 파일)
    {
        Tab t;
        std::vector<PIDLIST_ABSOLUTE> none;
        size_t idx = 0;
        const wchar_t* s = L"V0.3000;P;H0.7000;P;P;";
        t.root = ParseTree(s, &t, none, idx);
        assert(t.root && *s == 0 && idx == 3);
        assert(t.root->dir == DIR_V && t.root->ratio == 0.3f && t.root->b->dir == DIR_H);
        std::wstring out;
        std::vector<Node*> leaves;
        SerializeTree(t.root, out, leaves);
        assert(out == L"V0.3000;P;H0.7000;P;P;" && leaves.size() == 3);
        FreeSubtree(t.root);
        for (const wchar_t* bad : { L"", L"X;", L"V0.5;P;", L"V0.5P;P;", L"Vabc;P;P;" }) {
            const wchar_t* b = bad;
            idx = 0;
            assert(!ParseTree(b, &t, none, idx));
        }
        s = L"V9;P;P;";   // 비율은 0.05~0.95로
        idx = 0;
        t.root = ParseTree(s, &t, none, idx);
        assert(t.root && t.root->ratio == 0.95f);
        FreeSubtree(t.root);

        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        g_ini = std::wstring(tmp) + L"ee_selftest.ini";
        DeleteFileW(g_ini.c_str());
        Tab* t0 = NewTabAt(0, nullptr, true);
        Tab* t1 = NewTabAt(1, nullptr, false);
        Node* x = NewPane(t1, nullptr);
        InsertSplit(t1, t1->root, x, DIR_H);
        t1->focus = x;
        t1->name = L"작업 탭";
        t1->fixedName = true;
        SelectTab(1);
        g_restore = true;
        SaveSession();
        for (Tab* tt : g_tabs) { FreeSubtree(tt->root); delete tt; }
        g_tabs.clear(); g_cur = -1;
        (void)t0;
        assert(RestoreSession());
        assert(g_tabs.size() == 2 && g_cur == 1);
        Tab* r1 = g_tabs[1];
        assert(!r1->root->isPane && r1->root->dir == DIR_H && r1->focus == r1->root->b);
        assert(r1->fixedName && r1->name == L"작업 탭" && !g_tabs[0]->fixedName);
        g_restore = false;   // 끄면 탭을 저장하지 않는다
        SaveSession();
        for (Tab* tt : g_tabs) { FreeSubtree(tt->root); delete tt; }
        g_tabs.clear(); g_cur = -1;
        assert(!RestoreSession() && g_tabs.empty());
        DeleteFileW(g_ini.c_str());
        g_ini.clear();
    }

    ILFree(A); ILFree(B); ILFree(C);
    FreePane(c);
    printf("selftest ok\n");
    return 0;
}
#else
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    g_inst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;   // STA + 드래그&드롭

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = PaneProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kPaneClass;
    g_paneAtom = RegisterClassExW(&wc);

    InitSettingsPath();
    LoadSettings();

    // 인자: [/newwindow] [폴더 (파싱 이름 또는 ::{CLSID})]. 탐색기식 /e, /select 등은 무시.
    std::wstring path;
    bool forceNew = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (!lstrcmpiW(argv[i], L"/newwindow")) forceNew = true;
        else if (argv[i][0] != L'/' && path.empty()) path = argv[i];
    }
    if (argv) LocalFree(argv);

    // SPEC §11: 창 1개 = 프로세스 1개. 폴더 열기는 실행 중인 창의 새 탭으로 (설정)
    HWND other = FindWindowW(L"ExtendedExplorer", nullptr);
    if (other && !path.empty() && !forceNew && g_openInTab) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        COPYDATASTRUCT cd = { COPY_OPEN, (DWORD)((path.size() + 1) * sizeof(wchar_t)), (void*)path.c_str() };
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &res) && res) {
            OleUninitialize();
            return 0;
        }
    }

    wc.lpfnWndProc = MainProc;
    wc.style = CS_DBLCLKS;   // 탭 더블클릭 = 이름 바꾸기
    wc.lpszClassName = L"ExtendedExplorer";
    // 앱 아이콘 (app.ico → 리소스 100)
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(100), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(100), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, L"ExtendedExplorer", L"ExtendedExplorer",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                             nullptr, nullptr, hi, nullptr);
    if (!g_main) return 1;
    g_dpi = GetDpiForWindow(g_main);

    g_addrEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, g_main, nullptr, hi, nullptr);
    g_searchEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, g_main, nullptr, hi, nullptr);
    SetWindowSubclass(g_addrEdit, EditSubProc, 1, 0);
    SetWindowSubclass(g_searchEdit, EditSubProc, 2, 0);
    SHAutoComplete(g_addrEdit, SHACF_FILESYS_DIRS);
    WNDCLASSEXW pc = { sizeof(pc) };   // 미리 보기 창 (처리기 / 텍스트가 들어감)
    pc.lpfnWndProc = PreviewHostProc;
    pc.hInstance = hi;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"EEPreview";
    RegisterClassExW(&pc);
    g_pvHost = CreateWindowExW(0, L"EEPreview", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, g_main, nullptr, hi, nullptr);
    g_pvText = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                               ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL, 0, 0, 0, 0, g_pvHost, nullptr, hi, nullptr);
    SendMessageW(g_pvText, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    StartPreviewThread();   // 미리 보기·썸네일은 이 스레드에서 (UI가 멈추지 않게)
    g_tabEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, g_main, nullptr, hi, nullptr);
    SetWindowSubclass(g_tabEdit, EditSubProc, 3, 0);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            0, 0, 0, 0, g_main, nullptr, hi, nullptr);
    if (g_tip) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_main;
        ti.uId = 1;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 800);
    }
    MakeFonts();
    show = ApplyWindowPlacement(show);
    CreateRibbon();
    CreateSidebar();
    ApplyTheme();   // 창을 보이기 전에 (Pane 뷰는 만들어질 때 HookView에서)

    // 세션 복원은 첫 창에서만 (두 번째 창까지 복원하면 탭이 겹친다). 열 폴더가 있으면 그 뒤에 새 탭.
    bool restored = !other && !forceNew && g_restore && RestoreSession();
    PIDLIST_ABSOLUTE start = nullptr;
    if (!path.empty()) SHParseDisplayName(path.c_str(), nullptr, &start, 0, nullptr);
    if (!start && !restored) start = DefaultPidl();
    if (start) { NewTab(start); ILFree(start); }

    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    if (Cur()) FocusPane(Cur()->focus);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g_treeWnd && m.message == WM_KEYDOWN && (m.hwnd == g_treeWnd || IsChild(g_treeWnd, m.hwnd)))
            g_treeInputTick = GetTickCount();   // 사이드바 선택 변경이 키보드 조작인지 가리는 데 쓴다
        if (SplitterClick(m)) continue;
        if (PaneMiddleClick(m)) continue;
        if (ThumbButton(m)) continue;
        if (CtrlWheel(m)) continue;
        if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && HandleKey(m)) continue;
        if (m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST && ViewAccel(&m)) continue;
        IShellItem* treeHit = TreeItemAt(m);
        if (treeHit && (m.message == WM_MBUTTONUP || (m.wParam & MK_CONTROL))) {   // SPEC §5: 새 탭에서 열기
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(treeHit, &p))) { OpenInNewTab(p); ILFree(p); }
            treeHit->Release();
            continue;   // 사이드바 선택·현재 Pane은 그대로
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (treeHit) { TreeNavigate(treeHit); treeHit->Release(); }   // 트리가 클릭을 다 처리한 뒤
        SyncFocus();
    }
    if (g_font) DeleteObject(g_font);
    OleUninitialize();
    return 0;
}
#endif
