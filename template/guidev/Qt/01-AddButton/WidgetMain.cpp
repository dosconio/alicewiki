#include "WidgetMain.h"

WidgetMain::WidgetMain(QWidget *parent) : QWidget{parent} {
	title = "Title";
	title.onproperty(std::bind(&WidgetMain::TitleSet, this, std::placeholders::_1));

	this->setWindowTitle(title.reflect());
	this->resize(800, 600);

	button_hello = new QPushButton();

	button_hello->setParent(this);
	button_hello->resize(100, 40);
	button_hello->move(10, 10);
	button_hello->setText("Close");
	connect(button_hello, &QPushButton::clicked, this, &QWidget::close);
	button_hello->show();
}

void WidgetMain::TitleSet(uni::String* str) {
	this->setWindowTitle(str->reflect());
}

WidgetMain::~WidgetMain() {
	QDelete(button_hello);
}

