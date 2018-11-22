#pragma once

#include "Player.h"
#include "CP.h"
#include <QtCore/QThread>
#include <QtWidgets/qwidget.h>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QApplication>

class X :public QThread
{
	Q_OBJECT
public:
	void run()
	{
		while (true)
		{
			cout << "123" << endl;
			QThread::msleep(100);
		}
	}
};

int main(int argc, char *argv[])
{
	PlayCore pc1;
	string url = "D:/work/Resource/oceans.mp4";
	//string url2 = "rtmp://pili-live-rtmp.train.bjlxhz.com/lx-train/20001560_1";
	pc1.play(url);
	while (true)
	{
		this_thread::sleep_for(chrono::milliseconds(200));
	}
	//thread th = thread(&PlayCore::play, &pc1, ref(url));
	//PlayCore pc2;
	//string url2 = "D:/work/Resource/1.mp4";
	//thread th2 = thread(&PlayCore::play, &pc2, ref(url));

	//PlayCore pc3;
	//string url3 = "D:/work/Resource/oceans.mp4";
	//thread th3 = thread(&PlayCore::play, &pc3, ref(url));

	//th.join();
	//th2.join();
	//th3.join();

}