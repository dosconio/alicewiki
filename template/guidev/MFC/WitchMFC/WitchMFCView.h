
// WitchMFCView.h : interface of the CWitchMFCView class
//

#pragma once

#include <cpp/Device/_Video.hpp>

class CWitchMFCView : public CView
{
protected: // create from serialization only
	CWitchMFCView() noexcept;
	DECLARE_DYNCREATE(CWitchMFCView)

// Attributes
public:
	CWitchMFCDoc* GetDocument() const;

// Operations
public:

// Overrides
public:
	virtual void OnDraw(CDC* pDC);  // overridden to draw this view
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
protected:

// Implementation
public:
	virtual ~CWitchMFCView();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

protected:

// Generated message map functions
protected:
	afx_msg void OnFilePrintPreview();
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnDrawDrawpoints();
	afx_msg void OnDrawDrawline();
	afx_msg void OnDrawDrawrect();
	afx_msg void OnDrawDrawcircle();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);

//
public:
	CPoint point_dn;
	CPoint point_up;
	afx_msg void OnDrawDrawfont();
};

using namespace uni;
class my_vci_t : public VideoControlInterface { 
	virtual void SetCursor(const Point& disp) const {}
	virtual Point GetCursor() const { return Point(0, 0); }
	virtual void DrawPoint(const Point& disp, Color color) const;
	virtual void DrawRectangle(const uni::Rectangle& rect) const;
	virtual void DrawFont(const Point& disp, const DisplayFont& font) const {}
	virtual Color GetColor(Point p) const { return Color::FromRGB888(0); }
public:
	CWitchMFCView* pCDC;
};
extern my_vci_t my_vci;
extern VideoControlBlock Screen;

#ifndef _DEBUG  // debug version in WitchMFCView.cpp
inline CWitchMFCDoc* CWitchMFCView::GetDocument() const
   { return reinterpret_cast<CWitchMFCDoc*>(m_pDocument); }
#endif

