#include <QApplication>
#include <QWidget>
#include <QDebug>// for output message in the Qt
#include <cpp/basic>

int main(int argc, char *argv[]) {
	qDebug() << "Ciallo~" << '\n';
	int state{0};
	QApplication q(argc, argv);
	QWidget win;
	win.show();
	state = q.exec();// loop for events
	if (malc_count) qDebug() << "Memory leak: " << malc_count << '\n';
	else qDebug() << "Oyasmi~" << '\n';
	return state;
}
