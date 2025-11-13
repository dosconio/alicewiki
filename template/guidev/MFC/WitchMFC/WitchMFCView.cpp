
// WitchMFCView.cpp : implementation of the CWitchMFCView class
//

#include "pch.h"
#include "framework.h"
// SHARED_HANDLERS can be defined in an ATL project implementing preview, thumbnail
// and search filter handlers and allows sharing of document code with that project.
#ifndef SHARED_HANDLERS
#include "WitchMFC.h"
#endif

#include "WitchMFCDoc.h"
#include "WitchMFCView.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

my_vci_t my_vci;
void my_vci_t::DrawPoint(const Point& disp, Color color) const {
	CClientDC dc(pCDC);
	dc.SetPixel(disp.x, disp.y, RGB(color.r, color.g, color.b));
}
void my_vci_t::DrawRectangle(const uni::Rectangle& rect) const {
	CClientDC dc(pCDC);
	Color color = rect.color;
	CBrush brush(RGB(color.r, color.g, color.b));
	dc.FillRect(CRect(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height), &brush);
}
VideoControlBlock Screen(nullptr, my_vci);
VideoConsole VConsole(my_vci, Size2(80, 25));

// CWitchMFCView

IMPLEMENT_DYNCREATE(CWitchMFCView, CView)

BEGIN_MESSAGE_MAP(CWitchMFCView, CView)
	ON_WM_CONTEXTMENU()
	ON_WM_RBUTTONUP()
	ON_COMMAND(ID_DRAW_DRAWPOINTS, &CWitchMFCView::OnDrawDrawpoints)
	ON_COMMAND(ID_DRAW_DRAWLINE, &CWitchMFCView::OnDrawDrawline)
	ON_COMMAND(ID_DRAW_DRAWRECT, &CWitchMFCView::OnDrawDrawrect)
	ON_COMMAND(ID_DRAW_DRAWCIRCLE, &CWitchMFCView::OnDrawDrawcircle)
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_COMMAND(ID_DRAW_DRAWFONT, &CWitchMFCView::OnDrawDrawfont)
END_MESSAGE_MAP()

// CWitchMFCView construction/destruction

CWitchMFCView::CWitchMFCView() noexcept
{
	my_vci.pCDC = this;

}

CWitchMFCView::~CWitchMFCView()
{
}

BOOL CWitchMFCView::PreCreateWindow(CREATESTRUCT& cs)
{
	// TODO: Modify the Window class or styles here by modifying
	//  the CREATESTRUCT cs

	return CView::PreCreateWindow(cs);
}

// CWitchMFCView drawing

void CWitchMFCView::OnDraw(CDC* /*pDC*/)
{
	CWitchMFCDoc* pDoc = GetDocument();
	ASSERT_VALID(pDoc);
	if (!pDoc)
		return;

	// TODO: add draw code for native data here
}

void CWitchMFCView::OnRButtonUp(UINT /* nFlags */, CPoint point)
{
	ClientToScreen(&point);
	OnContextMenu(this, point);
}

void CWitchMFCView::OnContextMenu(CWnd* /* pWnd */, CPoint point)
{
#ifndef SHARED_HANDLERS
	theApp.GetContextMenuManager()->ShowPopupMenu(IDR_POPUP_EDIT, point.x, point.y, this, TRUE);
#endif
}


// CWitchMFCView diagnostics

#ifdef _DEBUG
void CWitchMFCView::AssertValid() const
{
	CView::AssertValid();
}

void CWitchMFCView::Dump(CDumpContext& dc) const
{
	CView::Dump(dc);
}

CWitchMFCDoc* CWitchMFCView::GetDocument() const // non-debug version is inline
{
	ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(CWitchMFCDoc)));
	return (CWitchMFCDoc*)m_pDocument;
}
#endif //_DEBUG


// CWitchMFCView message handlers


void CWitchMFCView::OnDrawDrawpoints()
{
	Screen.Draw(Point(5, 10), Color::Black);
}


void CWitchMFCView::OnDrawDrawline()
{
	Screen.DrawLine(Point(1, 1), Size2(10, 20), Color::Green);
}


void CWitchMFCView::OnDrawDrawrect()
{
	uni::Rectangle rec(Point(50, 100), Size2(600, 100), Color::Blue);
	Screen.Draw(rec);
}


void CWitchMFCView::OnDrawDrawcircle()
{
	uni::Circle cir(Point(200, 200), 100, Color::Red, false);
	Screen.Draw(cir);
	cir.filled = true;
	cir.center = Point(300, 300);
	cir.radius = 88;
	Screen.Draw(cir);
}


void CWitchMFCView::OnLButtonDown(UINT nFlags, CPoint point)
{
	point_dn = point;
	CView::OnLButtonDown(nFlags, point);
}


void CWitchMFCView::OnLButtonUp(UINT nFlags, CPoint point)
{
	point_up = point;
	CClientDC dc(this);
	CPen pen(PS_SOLID, 1, RGB(0, 255, 0));
	dc.SelectObject(&pen);
	dc.MoveTo(point_dn);
	dc.LineTo(point_up);
	CView::OnLButtonUp(nFlags, point);
}


void CWitchMFCView::OnDrawDrawfont()
{
	VConsole.OutFormat("Hello %x", 0x1234);
}
