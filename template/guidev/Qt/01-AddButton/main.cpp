#include <QApplication>
#include <QWidget>
#include <QDebug>// for output message in the Qt
#include <cpp/basic>
#include "WidgetMain.h"

void change_str(stduint i){
	qDebug() << "nya~" << i << "\n";
}

int subm(int argc, char *argv[]){
	int state{0};
	QApplication q(argc, argv);
	WidgetMain win;
	win.show();
	win.title = "Nihao!";
	state = q.exec();// loop for events
	return state;
}

int main(int argc, char *argv[]) {
	malc_count = 0;// flush Qt system
	qDebug() << "Ciallo~" << '\n';
	int state = subm(argc, argv);
	return state; // ABANDON: return malc_count + state;
}
