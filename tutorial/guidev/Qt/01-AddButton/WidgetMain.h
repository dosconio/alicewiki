#ifndef WIDGETMAIN_H
#define WIDGETMAIN_H

#include <QWidget>
#include <QPushButton>
#include <cpp/basic>

class WidgetMain : public QWidget
{
	Q_OBJECT
public:
	explicit WidgetMain(QWidget *parent = nullptr);
	~WidgetMain();
	uni::String title;
	QPushButton* button_hello;

private:
	void TitleSet(uni::String* str);
// signals:

};

#endif // WIDGETMAIN_H
